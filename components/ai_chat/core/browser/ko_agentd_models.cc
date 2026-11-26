// Copyright (c) 2026 The Brave Authors. All rights reserved.
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this file,
// You can obtain one at https://mozilla.org/MPL/2.0/.

#include "brave/components/ai_chat/core/browser/ko_agentd_models.h"

#include <algorithm>
#include <atomic>
#include <utility>

#include "base/check.h"
#include "base/command_line.h"
#include "base/containers/flat_set.h"
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
#include "brave/components/ko/common/agentd_config.h"
#include "brave/components/ko/common/ko_paths.h"
#include "net/traffic_annotation/network_traffic_annotation.h"
#include "services/network/public/cpp/shared_url_loader_factory.h"
#include "url/url_constants.h"

namespace ai_chat::ko {

namespace {

constexpr char kAgentdHost[] = "127.0.0.1";
constexpr char kAgentdDefaultModelDisplayName[] = "K-O agentd";
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

// The port the last fetch read; 0 until then. Written on the fetchers'
// sequence, read wherever agentd URLs are built or checked.
std::atomic<uint16_t> g_agentd_port{0};

std::optional<uint16_t> GetAgentdPortOverride() {
  const base::CommandLine* command_line =
      base::CommandLine::ForCurrentProcess();
  if (!command_line->HasSwitch(kAgentdPortSwitch)) {
    return std::nullopt;
  }
  unsigned port = 0;
  if (!base::StringToUint(command_line->GetSwitchValueASCII(kAgentdPortSwitch),
                          &port) ||
      port == 0 || port > 65535) {
    return std::nullopt;
  }
  return static_cast<uint16_t>(port);
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
                            uint16_t port,
                            bool is_suggested) {
  auto options = mojom::CustomModelOptions::New();
  options->model_request_name = info.id;
  options->context_size = info.context_size;
  options->endpoint = GetAgentdUrl(kAgentdChatCompletionsPath, port);
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
  return ::ko::GetDataDir();
}

uint16_t GetAgentdPort() {
  if (std::optional<uint16_t> port = GetAgentdPortOverride()) {
    return *port;
  }
  const uint16_t port = g_agentd_port.load(std::memory_order_relaxed);
  return port ? port : ::ko::kAgentdDefaultPort;
}

GURL GetAgentdUrl(std::string_view path, uint16_t port) {
  CHECK(path.starts_with("/"));
  return GURL(
      base::StrCat({url::kHttpScheme, url::kStandardSchemeSeparator,
                    kAgentdHost, ":", base::NumberToString(port), path}));
}

GURL GetAgentdUrl(std::string_view path) {
  return GetAgentdUrl(path, GetAgentdPort());
}

bool IsAgentdEndpoint(const GURL& url) {
  if (!url.is_valid() || !url.SchemeIs(url::kHttpScheme) ||
      url.host() != kAgentdHost) {
    return false;
  }
  const int port = url.EffectiveIntPort();
  return port == GetAgentdPort() || port == ::ko::kAgentdDefaultPort;
}

bool IsAgentdModelKey(std::string_view key) {
  return key == kChatAutomaticModelKey ||
         key.starts_with(kAgentdModelKeyPrefix);
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
    const std::string& token,
    uint16_t port) {
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
                               token, port, /*is_suggested=*/true));

  for (const auto& info : models) {
    result.push_back(
        CreateModel(base::StrCat({kAgentdModelKeyPrefix, info.id}),
                    info.display_name.empty() ? info.id : info.display_name,
                    info, token, port, /*is_suggested=*/false));
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
  // The token and the port it belongs to are read together.
  base::ThreadPool::PostTaskAndReplyWithResult(
      FROM_HERE,
      {base::MayBlock(), base::TaskPriority::USER_VISIBLE,
       base::TaskShutdownBehavior::CONTINUE_ON_SHUTDOWN},
      base::BindOnce(&::ko::ReadAgentdConfig, data_dir_),
      base::BindOnce(&AgentdModelsFetcher::OnConfigRead,
                     weak_ptr_factory_.GetWeakPtr(), std::move(callback)));
}

void AgentdModelsFetcher::OnConfigRead(FetchCallback callback,
                                       ::ko::AgentdConfig config) {
  const uint16_t port = GetAgentdPortOverride().value_or(config.port);
  if (config.token_status != ::ko::AgentdTokenStatus::kOk) {
    // No token, no request: agentd rejects unauthenticated calls anyway.
    Result out;
    out.port = port;
    g_agentd_port.store(port, std::memory_order_relaxed);
    std::move(callback).Run(std::move(out));
    return;
  }

  api_request_helper::APIRequestOptions options;
  options.max_body_size = kMaxModelsResponseBytes;
  options.timeout = kModelsRequestTimeout;
  options.allow_redirects = false;
  const std::string authorization = base::StrCat({"Bearer ", config.token});
  api_request_helper_->Request(
      "GET", GetAgentdUrl(kAgentdModelsPath, port), "", "application/json",
      base::BindOnce(&AgentdModelsFetcher::OnModelsResponse,
                     weak_ptr_factory_.GetWeakPtr(), std::move(callback),
                     std::move(config.token), port),
      {{"Authorization", authorization}}, options);
}

void AgentdModelsFetcher::OnModelsResponse(
    FetchCallback callback,
    std::string token,
    uint16_t port,
    api_request_helper::APIRequestResult result) {
  Result out;
  out.token = std::move(token);
  out.port = port;
  if (result.Is2XXResponseCode()) {
    out.models = ParseAgentdModels(result.value_body());
  }
  // Published with the models built from |out|, so IsAgentdEndpoint() matches
  // their endpoint.
  g_agentd_port.store(port, std::memory_order_relaxed);
  std::move(callback).Run(std::move(out));
}

void ResetAgentdPortForTesting() {
  g_agentd_port.store(0, std::memory_order_relaxed);
}

}  // namespace ai_chat::ko
