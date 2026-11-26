// Copyright (c) 2026 The Brave Authors. All rights reserved.
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this file,
// You can obtain one at https://mozilla.org/MPL/2.0/.

#ifndef BRAVE_COMPONENTS_AI_CHAT_CORE_BROWSER_KO_AGENTD_MODELS_H_
#define BRAVE_COMPONENTS_AI_CHAT_CORE_BROWSER_KO_AGENTD_MODELS_H_

#include <stddef.h>

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "base/files/file_path.h"
#include "base/functional/callback.h"
#include "base/memory/scoped_refptr.h"
#include "base/memory/weak_ptr.h"
#include "base/values.h"
#include "brave/components/ai_chat/core/common/mojom/common.mojom.h"
#include "brave/components/ko/buildflags/buildflags.h"
#include "url/gurl.h"

static_assert(BUILDFLAG(IS_KO));

namespace api_request_helper {
class APIRequestHelper;
class APIRequestResult;
}  // namespace api_request_helper

namespace network {
class SharedURLLoaderFactory;
}  // namespace network

// K-O's built-in model source: the local ko-agentd daemon, which exposes an
// OpenAI-compatible API on 127.0.0.1 and keeps provider keys in the Keychain.
// The browser only ever holds agentd's per-install bearer token, in memory.
namespace ai_chat::ko {

inline constexpr uint16_t kAgentdDefaultPort = 47821;
// Overrides the agentd port. The host is always 127.0.0.1.
inline constexpr char kAgentdPortSwitch[] = "ko-agentd-port";
inline constexpr char kAgentdTokenFileName[] = "agentd.token";
inline constexpr char kAgentdChatCompletionsPath[] = "/v1/chat/completions";
inline constexpr char kAgentdModelsPath[] = "/v1/models";

// Keys of models listed by agentd. The default entry uses
// kChatAutomaticModelKey so every "automatic" fallback lands on agentd.
inline constexpr char kAgentdModelKeyPrefix[] = "ko-agentd:";
// Requested before agentd has listed its models.
inline constexpr char kAgentdFallbackModelName[] = "claude-code/default";
inline constexpr uint32_t kAgentdDefaultContextSize = 128000;
inline constexpr size_t kMaxAgentdModels = 200;

// Non-secret cache of the last /v1/models result (ids and hints only), so
// saved conversations resolve their model before agentd answers.
inline constexpr char kAgentdModelsCachePref[] =
    "brave.ai_chat.ko_agentd_models";

struct AgentdModelInfo {
  AgentdModelInfo();
  AgentdModelInfo(const AgentdModelInfo&);
  AgentdModelInfo& operator=(const AgentdModelInfo&);
  AgentdModelInfo(AgentdModelInfo&&);
  AgentdModelInfo& operator=(AgentdModelInfo&&);
  ~AgentdModelInfo();

  bool operator==(const AgentdModelInfo&) const = default;

  std::string id;
  std::string display_name;
  bool vision = false;
  bool tools = true;
  uint32_t context_size = kAgentdDefaultContextSize;
};

// macOS: ~/Library/Application Support/K-O. Linux: $XDG_DATA_HOME/k-o, else
// ~/.local/share/k-o. Empty on other platforms or if it can't be resolved.
base::FilePath GetDataDir();

uint16_t GetAgentdPort();
// http://127.0.0.1:<port><path>. |path| must start with '/'.
GURL GetAgentdUrl(std::string_view path);
bool IsAgentdEndpoint(const GURL& url);
// True for kChatAutomaticModelKey and kAgentdModelKeyPrefix keys.
bool IsAgentdModelKey(std::string_view key);

bool IsValidAgentdToken(std::string_view token);
// Reads <data_dir>/agentd.token. Returns nullopt unless it is a regular file
// owned by the current user with no group/other access and holds a valid
// token. Blocking.
std::optional<std::string> ReadAgentdToken(const base::FilePath& data_dir);

// Parses an OpenAI-style {"data": [{"id": ...}]} listing. Invalid entries are
// skipped; nullopt if the shape is wrong.
std::optional<std::vector<AgentdModelInfo>> ParseAgentdModels(
    const base::Value& body);

base::ListValue AgentdModelsToCache(const std::vector<AgentdModelInfo>& models);
std::vector<AgentdModelInfo> AgentdModelsFromCache(const base::ListValue& list);

// The always-present default model followed by one model per |models| entry,
// all pointing at agentd with |token| as API key.
std::vector<mojom::ModelPtr> BuildAgentdModels(
    const std::vector<AgentdModelInfo>& models,
    const std::string& token);

struct AgentdFetchResult {
  AgentdFetchResult();
  AgentdFetchResult(AgentdFetchResult&&);
  AgentdFetchResult& operator=(AgentdFetchResult&&);
  ~AgentdFetchResult();

  std::optional<std::string> token;
  // nullopt if agentd could not be reached or answered badly.
  std::optional<std::vector<AgentdModelInfo>> models;
};

// Reads the token, then lists agentd's models with it.
class AgentdModelsFetcher {
 public:
  using Result = AgentdFetchResult;
  using FetchCallback = base::OnceCallback<void(Result)>;

  AgentdModelsFetcher(
      scoped_refptr<network::SharedURLLoaderFactory> url_loader_factory,
      base::FilePath data_dir);
  AgentdModelsFetcher(const AgentdModelsFetcher&) = delete;
  AgentdModelsFetcher& operator=(const AgentdModelsFetcher&) = delete;
  ~AgentdModelsFetcher();

  // Drops any fetch in progress without running its callback.
  void Fetch(FetchCallback callback);

 private:
  void OnTokenRead(FetchCallback callback, std::optional<std::string> token);
  void OnModelsResponse(FetchCallback callback,
                        std::string token,
                        api_request_helper::APIRequestResult result);

  base::FilePath data_dir_;
  std::unique_ptr<api_request_helper::APIRequestHelper> api_request_helper_;
  base::WeakPtrFactory<AgentdModelsFetcher> weak_ptr_factory_{this};
};

}  // namespace ai_chat::ko

#endif  // BRAVE_COMPONENTS_AI_CHAT_CORE_BROWSER_KO_AGENTD_MODELS_H_
