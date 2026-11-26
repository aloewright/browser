// Copyright (c) 2026 The Brave Authors. All rights reserved.
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this file,
// You can obtain one at https://mozilla.org/MPL/2.0/.

#include "brave/components/ai_chat/core/browser/ko_agentd_models.h"

#include <algorithm>
#include <utility>

#include "base/check.h"
#include "base/command_line.h"
#include "base/containers/flat_set.h"
#include "base/containers/span.h"
#include "base/files/file.h"
#include "base/files/file_util.h"
#include "base/functional/bind.h"
#include "base/strings/strcat.h"
#include "base/strings/string_number_conversions.h"
#include "base/strings/string_util.h"
#include "base/task/thread_pool.h"
#include "base/time/time.h"
#include "brave/components/ai_chat/core/browser/model_service.h"
#include "brave/components/ai_chat/core/browser/model_validator.h"
#include "brave/components/ai_chat/core/common/constants.h"
#include "brave/components/api_request_helper/api_request_helper.h"
#include "build/build_config.h"
#include "net/traffic_annotation/network_traffic_annotation.h"
#include "services/network/public/cpp/shared_url_loader_factory.h"
#include "url/url_constants.h"

#if BUILDFLAG(IS_POSIX)
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#endif

#if BUILDFLAG(IS_MAC)
#include "base/base_paths.h"
#include "base/path_service.h"
#elif BUILDFLAG(IS_LINUX)
#include "base/environment.h"
#include "base/nix/xdg_util.h"
#endif

namespace ai_chat::ko {

namespace {

constexpr char kAgentdHost[] = "127.0.0.1";
constexpr char kAgentdDefaultModelDisplayName[] = "K-O agentd";
constexpr size_t kMinTokenLength = 16;
constexpr size_t kMaxTokenLength = 512;
constexpr size_t kMaxModelIdLength = 200;
constexpr size_t kMaxDisplayNameLength = 100;
constexpr size_t kMaxModelsResponseBytes = 1024 * 1024;
constexpr base::TimeDelta kModelsRequestTimeout = base::Seconds(10);

constexpr char kCacheIdKey[] = "id";
constexpr char kCacheNameKey[] = "name";
constexpr char kCacheVisionKey[] = "vision";
constexpr char kCacheToolsKey[] = "tools";
constexpr char kCacheContextKey[] = "context";

constexpr net::NetworkTrafficAnnotationTag kTrafficAnnotation =
    net::DefineNetworkTrafficAnnotation("ko_agentd_models", R"(
      semantics {
        sender: "K-O Assistant"
        description:
          "Lists the models offered by the user's local ko-agentd daemon."
        trigger: "The assistant's model list is loaded or refreshed."
        data: "The ko-agentd bearer token. Nothing leaves the device."
        destination: LOCAL
      }
      policy {
        cookies_allowed: NO
        setting: "Stop ko-agentd or disable the assistant."
      })");

bool IsTokenChar(char c) {
  return base::IsAsciiAlphaNumeric(c) || c == '-' || c == '_' || c == '.' ||
         c == '~' || c == '+' || c == '/' || c == '=';
}

bool IsModelIdChar(char c) {
  return base::IsAsciiAlphaNumeric(c) || c == '-' || c == '_' || c == '.' ||
         c == '/' || c == ':' || c == '@' || c == '+';
}

bool IsValidModelId(std::string_view id) {
  return !id.empty() && id.size() <= kMaxModelIdLength &&
         std::ranges::all_of(id, IsModelIdChar);
}

// Display names come from agentd, so keep them short and printable.
std::string SanitizeDisplayName(const std::string* name) {
  if (!name || name->size() > kMaxDisplayNameLength ||
      !base::IsStringUTF8(*name)) {
    return std::string();
  }
  std::string out(base::TrimWhitespaceASCII(*name, base::TRIM_ALL));
  for (char c : out) {
    if (base::IsAsciiControl(c)) {
      return std::string();
    }
  }
  return out;
}

uint32_t ClampContextSize(std::optional<int> size) {
  if (!size || !ModelValidator::IsValidContextSize(*size)) {
    return kAgentdDefaultContextSize;
  }
  return static_cast<uint32_t>(*size);
}

std::optional<bool> FindCapability(const base::DictValue& entry,
                                   std::string_view key) {
  if (std::optional<bool> value = entry.FindBool(key)) {
    return value;
  }
  if (const base::DictValue* caps = entry.FindDict("capabilities")) {
    return caps->FindBool(key);
  }
  return std::nullopt;
}

mojom::ModelPtr CreateModel(std::string key,
                            std::string display_name,
                            const AgentdModelInfo& info,
                            const std::string& token,
                            bool is_suggested) {
  auto options = mojom::CustomModelOptions::New();
  options->model_request_name = info.id;
  options->context_size = info.context_size;
  options->endpoint = GetAgentdUrl(kAgentdChatCompletionsPath);
  options->api_key = token;

  auto model = mojom::Model::New();
  model->key = std::move(key);
  model->display_name = std::move(display_name);
  model->vision_support = info.vision;
  model->supports_tools = info.tools;
  // Lets Leo's tools, including the content agent's, run on agentd models.
  if (info.tools) {
    model->supported_capabilities = {
        mojom::ConversationCapability::CONTENT_AGENT};
  }
  model->is_suggested_model = is_suggested;
  model->is_near_model = false;
  model->supports_private_inference = false;
  model->options =
      mojom::ModelOptions::NewCustomModelOptions(std::move(options));
  ModelService::SetAssociatedContentLengthMetrics(*model);
  return model;
}

}  // namespace

AgentdModelInfo::AgentdModelInfo() = default;
AgentdModelInfo::AgentdModelInfo(const AgentdModelInfo&) = default;
AgentdModelInfo& AgentdModelInfo::operator=(const AgentdModelInfo&) = default;
AgentdModelInfo::AgentdModelInfo(AgentdModelInfo&&) = default;
AgentdModelInfo& AgentdModelInfo::operator=(AgentdModelInfo&&) = default;
AgentdModelInfo::~AgentdModelInfo() = default;

AgentdFetchResult::AgentdFetchResult() = default;
AgentdFetchResult::AgentdFetchResult(AgentdFetchResult&&) = default;
AgentdFetchResult& AgentdFetchResult::operator=(AgentdFetchResult&&) = default;
AgentdFetchResult::~AgentdFetchResult() = default;

base::FilePath GetDataDir() {
#if BUILDFLAG(IS_MAC)
  base::FilePath app_support;
  if (!base::PathService::Get(base::DIR_APP_DATA, &app_support)) {
    return base::FilePath();
  }
  return app_support.Append("K-O");
#elif BUILDFLAG(IS_LINUX)
  std::unique_ptr<base::Environment> env = base::Environment::Create();
  base::FilePath data_home =
      base::nix::GetXDGDirectory(env.get(), "XDG_DATA_HOME", ".local/share");
  if (data_home.empty() || !data_home.IsAbsolute()) {
    base::FilePath home = base::GetHomeDir();
    if (home.empty()) {
      return base::FilePath();
    }
    data_home = home.Append(".local").Append("share");
  }
  return data_home.Append("k-o");
#else
  return base::FilePath();
#endif
}

uint16_t GetAgentdPort() {
  const base::CommandLine* command_line =
      base::CommandLine::ForCurrentProcess();
  if (command_line->HasSwitch(kAgentdPortSwitch)) {
    unsigned port = 0;
    if (base::StringToUint(command_line->GetSwitchValueASCII(kAgentdPortSwitch),
                           &port) &&
        port > 0 && port <= 65535) {
      return static_cast<uint16_t>(port);
    }
  }
  return kAgentdDefaultPort;
}

GURL GetAgentdUrl(std::string_view path) {
  CHECK(path.starts_with("/"));
  return GURL(base::StrCat({url::kHttpScheme, url::kStandardSchemeSeparator,
                            kAgentdHost, ":",
                            base::NumberToString(GetAgentdPort()), path}));
}

bool IsAgentdEndpoint(const GURL& url) {
  return url.is_valid() && url.SchemeIs(url::kHttpScheme) &&
         url.host() == kAgentdHost && url.EffectiveIntPort() == GetAgentdPort();
}

bool IsAgentdModelKey(std::string_view key) {
  return key == kChatAutomaticModelKey ||
         key.starts_with(kAgentdModelKeyPrefix);
}

bool IsValidAgentdToken(std::string_view token) {
  return token.size() >= kMinTokenLength && token.size() <= kMaxTokenLength &&
         std::ranges::all_of(token, IsTokenChar);
}

std::optional<std::string> ReadAgentdToken(const base::FilePath& data_dir) {
#if BUILDFLAG(IS_POSIX)
  if (data_dir.empty()) {
    return std::nullopt;
  }
  const base::FilePath path = data_dir.Append(kAgentdTokenFileName);
  if (base::IsLink(path)) {
    return std::nullopt;
  }
  base::File file(path, base::File::FLAG_OPEN | base::File::FLAG_READ);
  if (!file.IsValid()) {
    return std::nullopt;
  }
  // Check the opened file itself so a swap after the check can't matter.
  struct stat st;
  if (fstat(file.GetPlatformFile(), &st) != 0 || !S_ISREG(st.st_mode) ||
      st.st_uid != geteuid() || (st.st_mode & (S_IRWXG | S_IRWXO)) != 0 ||
      st.st_size > static_cast<off_t>(kMaxTokenLength + 2)) {
    return std::nullopt;
  }
  std::string contents(kMaxTokenLength + 2, '\0');
  std::optional<size_t> read =
      file.ReadAtCurrentPos(base::as_writable_byte_span(contents));
  if (!read) {
    return std::nullopt;
  }
  contents.resize(*read);
  std::string token(base::TrimWhitespaceASCII(contents, base::TRIM_ALL));
  if (!IsValidAgentdToken(token)) {
    return std::nullopt;
  }
  return token;
#else
  return std::nullopt;
#endif
}

std::optional<std::vector<AgentdModelInfo>> ParseAgentdModels(
    const base::Value& body) {
  const base::DictValue* dict = body.GetIfDict();
  if (!dict) {
    return std::nullopt;
  }
  const base::ListValue* data = dict->FindList("data");
  if (!data) {
    return std::nullopt;
  }

  std::vector<AgentdModelInfo> models;
  base::flat_set<std::string> seen;
  for (const base::Value& item : *data) {
    if (models.size() >= kMaxAgentdModels) {
      break;
    }
    const base::DictValue* entry = item.GetIfDict();
    if (!entry) {
      continue;
    }
    const std::string* id = entry->FindString("id");
    if (!id || !IsValidModelId(*id) || seen.contains(*id)) {
      continue;
    }
    seen.insert(*id);

    AgentdModelInfo info;
    info.id = *id;
    info.display_name = SanitizeDisplayName(entry->FindString("display_name"));
    if (info.display_name.empty()) {
      info.display_name = SanitizeDisplayName(entry->FindString("name"));
    }
    info.vision = FindCapability(*entry, "vision").value_or(false);
    info.tools = FindCapability(*entry, "tools").value_or(true);
    std::optional<int> context = entry->FindInt("context_length");
    if (!context) {
      context = entry->FindInt("context_window");
    }
    info.context_size = ClampContextSize(context);
    models.push_back(std::move(info));
  }
  return models;
}

base::ListValue AgentdModelsToCache(
    const std::vector<AgentdModelInfo>& models) {
  base::ListValue list;
  for (const auto& info : models) {
    base::DictValue entry;
    entry.Set(kCacheIdKey, info.id);
    entry.Set(kCacheNameKey, info.display_name);
    entry.Set(kCacheVisionKey, info.vision);
    entry.Set(kCacheToolsKey, info.tools);
    entry.Set(kCacheContextKey, static_cast<int>(info.context_size));
    list.Append(std::move(entry));
  }
  return list;
}

std::vector<AgentdModelInfo> AgentdModelsFromCache(
    const base::ListValue& list) {
  std::vector<AgentdModelInfo> models;
  base::flat_set<std::string> seen;
  for (const base::Value& item : list) {
    if (models.size() >= kMaxAgentdModels) {
      break;
    }
    const base::DictValue* entry = item.GetIfDict();
    if (!entry) {
      continue;
    }
    const std::string* id = entry->FindString(kCacheIdKey);
    if (!id || !IsValidModelId(*id) || seen.contains(*id)) {
      continue;
    }
    seen.insert(*id);
    AgentdModelInfo info;
    info.id = *id;
    info.display_name = SanitizeDisplayName(entry->FindString(kCacheNameKey));
    info.vision = entry->FindBool(kCacheVisionKey).value_or(false);
    info.tools = entry->FindBool(kCacheToolsKey).value_or(true);
    info.context_size = ClampContextSize(entry->FindInt(kCacheContextKey));
    models.push_back(std::move(info));
  }
  return models;
}

std::vector<mojom::ModelPtr> BuildAgentdModels(
    const std::vector<AgentdModelInfo>& models,
    const std::string& token) {
  std::vector<mojom::ModelPtr> result;
  result.reserve(models.size() + 1);

  // agentd lists its preferred model first; the default entry follows it.
  AgentdModelInfo default_info;
  if (!models.empty()) {
    default_info = models.front();
  } else {
    default_info.id = kAgentdFallbackModelName;
  }
  result.push_back(CreateModel(kChatAutomaticModelKey,
                               kAgentdDefaultModelDisplayName, default_info,
                               token, /*is_suggested=*/true));

  for (const auto& info : models) {
    result.push_back(CreateModel(
        base::StrCat({kAgentdModelKeyPrefix, info.id}),
        info.display_name.empty() ? info.id : info.display_name, info, token,
        /*is_suggested=*/false));
  }
  return result;
}

AgentdModelsFetcher::AgentdModelsFetcher(
    scoped_refptr<network::SharedURLLoaderFactory> url_loader_factory,
    base::FilePath data_dir)
    : data_dir_(std::move(data_dir)),
      api_request_helper_(
          std::make_unique<api_request_helper::APIRequestHelper>(
              kTrafficAnnotation,
              std::move(url_loader_factory))) {}

AgentdModelsFetcher::~AgentdModelsFetcher() = default;

void AgentdModelsFetcher::Fetch(FetchCallback callback) {
  weak_ptr_factory_.InvalidateWeakPtrs();
  api_request_helper_->CancelAll();
  base::ThreadPool::PostTaskAndReplyWithResult(
      FROM_HERE,
      {base::MayBlock(), base::TaskPriority::USER_VISIBLE,
       base::TaskShutdownBehavior::CONTINUE_ON_SHUTDOWN},
      base::BindOnce(&ReadAgentdToken, data_dir_),
      base::BindOnce(&AgentdModelsFetcher::OnTokenRead,
                     weak_ptr_factory_.GetWeakPtr(), std::move(callback)));
}

void AgentdModelsFetcher::OnTokenRead(FetchCallback callback,
                                      std::optional<std::string> token) {
  if (!token) {
    // No token, no request: agentd rejects unauthenticated calls anyway.
    std::move(callback).Run(Result());
    return;
  }

  api_request_helper::APIRequestOptions options;
  options.max_body_size = kMaxModelsResponseBytes;
  options.timeout = kModelsRequestTimeout;
  options.allow_redirects = false;
  api_request_helper_->Request(
      "GET", GetAgentdUrl(kAgentdModelsPath), "", "application/json",
      base::BindOnce(&AgentdModelsFetcher::OnModelsResponse,
                     weak_ptr_factory_.GetWeakPtr(), std::move(callback),
                     *token),
      {{"Authorization", base::StrCat({"Bearer ", *token})}}, options);
}

void AgentdModelsFetcher::OnModelsResponse(
    FetchCallback callback,
    std::string token,
    api_request_helper::APIRequestResult result) {
  Result out;
  out.token = std::move(token);
  if (result.Is2XXResponseCode()) {
    out.models = ParseAgentdModels(result.value_body());
  }
  std::move(callback).Run(std::move(out));
}

}  // namespace ai_chat::ko
