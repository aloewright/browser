// Copyright (c) 2026 The Brave Authors. All rights reserved.
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this file,
// You can obtain one at https://mozilla.org/MPL/2.0/.

#include "brave/components/ai_chat/core/browser/ko_agentd_models.h"

#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "base/command_line.h"
#include "base/files/file_path.h"
#include "base/files/file_util.h"
#include "base/files/scoped_temp_dir.h"
#include "base/memory/scoped_refptr.h"
#include "base/test/bind.h"
#include "base/test/scoped_command_line.h"
#include "base/test/task_environment.h"
#include "base/test/test_future.h"
#include "base/test/values_test_util.h"
#include "brave/components/ai_chat/core/common/constants.h"
#include "brave/components/ko/common/agentd_config.h"
#include "build/build_config.h"
#include "net/http/http_status_code.h"
#include "services/network/public/cpp/resource_request.h"
#include "services/network/public/cpp/weak_wrapper_shared_url_loader_factory.h"
#include "services/network/test/test_url_loader_factory.h"
#include "testing/gtest/include/gtest/gtest.h"
#include "url/gurl.h"

namespace ai_chat::ko {

namespace {

// Placeholder in agentd's format (43 base64url characters), not a real token.
constexpr char kToken[] = "test_token_not_a_real_secret_0123456789-ABC";

AgentdModelInfo MakeInfo(std::string id, bool tools) {
  AgentdModelInfo info;
  info.id = std::move(id);
  info.tools = tools;
  return info;
}

}  // namespace

// GetAgentdPort() is process-wide; start and end every test without it.
class KOAgentdModelsTest : public testing::Test {
 protected:
  void SetUp() override { ResetAgentdPortForTesting(); }
  void TearDown() override { ResetAgentdPortForTesting(); }
};

TEST_F(KOAgentdModelsTest, UrlsAreLoopbackOnly) {
  EXPECT_EQ(GetAgentdPort(), ::ko::kAgentdDefaultPort);
  EXPECT_EQ(GetAgentdUrl(kAgentdModelsPath).spec(),
            "http://127.0.0.1:47821/v1/models");
  EXPECT_EQ(GetAgentdUrl(kAgentdModelsPath, 48123).spec(),
            "http://127.0.0.1:48123/v1/models");
  EXPECT_TRUE(IsAgentdEndpoint(GetAgentdUrl(kAgentdChatCompletionsPath)));
  EXPECT_FALSE(
      IsAgentdEndpoint(GURL("http://localhost:47821/v1/chat/completions")));
  EXPECT_FALSE(IsAgentdEndpoint(GURL("https://127.0.0.1:47821/v1/models")));
  EXPECT_FALSE(IsAgentdEndpoint(GURL("http://127.0.0.1:1234/v1/models")));
  EXPECT_FALSE(IsAgentdEndpoint(GURL("http://example.com:47821/v1/models")));
}

TEST_F(KOAgentdModelsTest, PortSwitch) {
  base::test::ScopedCommandLine command_line;
  command_line.GetProcessCommandLine()->AppendSwitchASCII(kAgentdPortSwitch,
                                                          "50000");
  EXPECT_EQ(GetAgentdPort(), 50000);
  EXPECT_EQ(GetAgentdUrl(kAgentdModelsPath).spec(),
            "http://127.0.0.1:50000/v1/models");
  EXPECT_TRUE(IsAgentdEndpoint(GURL("http://127.0.0.1:50000/v1/models")));
}

TEST_F(KOAgentdModelsTest, InvalidPortSwitchIsIgnored) {
  for (const char* value : {"0", "70000", "abc", "-1", ""}) {
    base::test::ScopedCommandLine command_line;
    command_line.GetProcessCommandLine()->AppendSwitchASCII(kAgentdPortSwitch,
                                                            value);
    EXPECT_EQ(GetAgentdPort(), ::ko::kAgentdDefaultPort) << value;
  }
}

TEST_F(KOAgentdModelsTest, ModelKeys) {
  EXPECT_TRUE(IsAgentdModelKey(kChatAutomaticModelKey));
  EXPECT_TRUE(IsAgentdModelKey("ko-agentd:codex/gpt-5"));
  EXPECT_FALSE(IsAgentdModelKey("custom:1a2b3c4d"));
  EXPECT_FALSE(IsAgentdModelKey("chat-claude-sonnet"));
}

TEST_F(KOAgentdModelsTest, ToolsDefaultByProvider) {
  EXPECT_FALSE(AgentdModelSupportsToolsByDefault("claude-code/sonnet"));
  EXPECT_FALSE(AgentdModelSupportsToolsByDefault("codex/gpt-5"));
  EXPECT_FALSE(AgentdModelSupportsToolsByDefault(kAgentdFallbackModelName));
  EXPECT_TRUE(AgentdModelSupportsToolsByDefault("cf-gateway/openai/gpt-5"));
  EXPECT_TRUE(AgentdModelSupportsToolsByDefault("nanogpt/gpt-5-mini"));
  // Only the provider prefix counts.
  EXPECT_TRUE(AgentdModelSupportsToolsByDefault("nanogpt/codex/x"));
  EXPECT_TRUE(AgentdModelSupportsToolsByDefault("claude-code"));
}

TEST_F(KOAgentdModelsTest, ParsesModelList) {
  std::optional<std::vector<AgentdModelInfo>> models =
      ParseAgentdModels(base::test::ParseJson(R"({
        "object": "list",
        "data": [
          {"id": "claude-code/sonnet", "object": "model"},
          {"id": "codex/gpt-5", "display_name": "Codex GPT-5",
           "capabilities": {"vision": true, "tools": false},
           "context_length": 200000},
          {"id": "bad id"},
          {"id": "claude-code/sonnet"},
          {"name": "no id"},
          {"id": "nanogpt/x", "name": "bad\u0007name", "context_window": 1},
          "not a dict"
        ]
      })"));
  ASSERT_TRUE(models);
  ASSERT_EQ(models->size(), 3u);

  EXPECT_EQ((*models)[0].id, "claude-code/sonnet");
  EXPECT_EQ((*models)[0].display_name, "");
  EXPECT_FALSE((*models)[0].vision);
  // No capabilities listed, and claude-code can't hand tool calls back.
  EXPECT_FALSE((*models)[0].tools);
  EXPECT_EQ((*models)[0].context_size, kAgentdDefaultContextSize);

  EXPECT_EQ((*models)[1].id, "codex/gpt-5");
  EXPECT_EQ((*models)[1].display_name, "Codex GPT-5");
  EXPECT_TRUE((*models)[1].vision);
  EXPECT_FALSE((*models)[1].tools);
  EXPECT_EQ((*models)[1].context_size, 200000u);

  // Control characters drop the name; too small a context is ignored.
  EXPECT_EQ((*models)[2].id, "nanogpt/x");
  EXPECT_EQ((*models)[2].display_name, "");
  EXPECT_TRUE((*models)[2].tools);
  EXPECT_EQ((*models)[2].context_size, kAgentdDefaultContextSize);
}

TEST_F(KOAgentdModelsTest, ExplicitCapabilitiesWinOverIdDefaults) {
  std::optional<std::vector<AgentdModelInfo>> models =
      ParseAgentdModels(base::test::ParseJson(R"({
        "data": [
          {"id": "claude-code/sonnet", "capabilities": {"tools": true}},
          {"id": "codex/gpt-5", "tools": true},
          {"id": "nanogpt/x", "capabilities": {"tools": false}},
          {"id": "cf-gateway/openai/gpt-5"},
          {"id": "codex/gpt-5-mini", "capabilities": {"vision": true}}
        ]
      })"));
  ASSERT_TRUE(models);
  ASSERT_EQ(models->size(), 5u);
  EXPECT_TRUE((*models)[0].tools);
  EXPECT_TRUE((*models)[1].tools);
  EXPECT_FALSE((*models)[2].tools);
  EXPECT_TRUE((*models)[3].tools);
  EXPECT_FALSE((*models)[4].tools);
  EXPECT_TRUE((*models)[4].vision);
}

TEST_F(KOAgentdModelsTest, CliModelsWithoutCapabilitiesGetNoAgentTools) {
  std::optional<std::vector<AgentdModelInfo>> infos =
      ParseAgentdModels(base::test::ParseJson(R"({
        "data": [{"id": "claude-code/sonnet"}, {"id": "nanogpt/x"}]
      })"));
  ASSERT_TRUE(infos);
  std::vector<mojom::ModelPtr> models =
      BuildAgentdModels(*infos, kToken, ::ko::kAgentdDefaultPort);
  ASSERT_EQ(models.size(), 3u);
  // The automatic default follows claude-code/sonnet.
  for (size_t i : {0u, 1u}) {
    SCOPED_TRACE(models[i]->key);
    EXPECT_FALSE(models[i]->supports_tools);
    EXPECT_TRUE(models[i]->supported_capabilities.empty());
  }
  EXPECT_EQ(models[1]->key, "ko-agentd:claude-code/sonnet");
  EXPECT_EQ(models[2]->key, "ko-agentd:nanogpt/x");
  EXPECT_TRUE(models[2]->supports_tools);
  EXPECT_EQ(models[2]->supported_capabilities,
            std::vector<mojom::ConversationCapability>{
                mojom::ConversationCapability::CONTENT_AGENT});
}

TEST_F(KOAgentdModelsTest, RejectsWrongShape) {
  EXPECT_EQ(ParseAgentdModels(base::test::ParseJson("[]")), std::nullopt);
  EXPECT_EQ(ParseAgentdModels(base::test::ParseJson(R"({"data": {}})")),
            std::nullopt);
  std::optional<std::vector<AgentdModelInfo>> empty =
      ParseAgentdModels(base::test::ParseJson(R"({"data": []})"));
  ASSERT_TRUE(empty);
  EXPECT_TRUE(empty->empty());
}

TEST_F(KOAgentdModelsTest, BuildModelsLeadsWithAutomatic) {
  std::vector<mojom::ModelPtr> models = BuildAgentdModels(
      {MakeInfo("nanogpt/gpt-5-mini", true), MakeInfo("codex/gpt-5", false)},
      kToken, 48123);
  ASSERT_EQ(models.size(), 3u);

  EXPECT_EQ(models[0]->key, kChatAutomaticModelKey);
  EXPECT_TRUE(models[0]->is_suggested_model);
  ASSERT_TRUE(models[0]->options->is_custom_model_options());
  const auto& default_opts = models[0]->options->get_custom_model_options();
  EXPECT_EQ(default_opts->model_request_name, "nanogpt/gpt-5-mini");
  EXPECT_EQ(default_opts->endpoint,
            GURL("http://127.0.0.1:48123/v1/chat/completions"));
  EXPECT_EQ(default_opts->api_key, kToken);
  EXPECT_GT(default_opts->max_associated_content_length, 0u);

  EXPECT_EQ(models[1]->key, "ko-agentd:nanogpt/gpt-5-mini");
  EXPECT_EQ(models[1]->display_name, "nanogpt/gpt-5-mini");
  EXPECT_TRUE(models[1]->supports_tools);
  EXPECT_EQ(models[1]->supported_capabilities,
            std::vector<mojom::ConversationCapability>{
                mojom::ConversationCapability::CONTENT_AGENT});
  EXPECT_FALSE(models[1]->supports_private_inference);

  EXPECT_EQ(models[2]->key, "ko-agentd:codex/gpt-5");
  EXPECT_FALSE(models[2]->supports_tools);
  EXPECT_TRUE(models[2]->supported_capabilities.empty());
  EXPECT_EQ(models[2]->options->get_custom_model_options()->endpoint,
            GURL("http://127.0.0.1:48123/v1/chat/completions"));
}

TEST_F(KOAgentdModelsTest, BuildModelsWithoutAgentdKeepsDefault) {
  std::vector<mojom::ModelPtr> models =
      BuildAgentdModels({}, std::string(), ::ko::kAgentdDefaultPort);
  ASSERT_EQ(models.size(), 1u);
  EXPECT_EQ(models[0]->key, kChatAutomaticModelKey);
  EXPECT_EQ(models[0]->options->get_custom_model_options()->model_request_name,
            kAgentdFallbackModelName);
  EXPECT_TRUE(models[0]->options->get_custom_model_options()->api_key.empty());
  // The fallback is a claude-code model, so no agent tools.
  EXPECT_FALSE(models[0]->supports_tools);
  EXPECT_TRUE(models[0]->supported_capabilities.empty());
}

TEST_F(KOAgentdModelsTest, CacheWithoutToolsUsesIdDefaults) {
  base::ListValue cache;
  cache.Append(base::DictValue().Set("id", "claude-code/sonnet"));
  cache.Append(base::DictValue().Set("id", "nanogpt/x"));
  cache.Append(base::DictValue().Set("id", "codex/gpt-5").Set("tools", true));
  std::vector<AgentdModelInfo> models = AgentdModelsFromCache(cache);
  ASSERT_EQ(models.size(), 3u);
  EXPECT_FALSE(models[0].tools);
  EXPECT_TRUE(models[1].tools);
  EXPECT_TRUE(models[2].tools);
}

TEST_F(KOAgentdModelsTest, CacheRoundTrip) {
  AgentdModelInfo info = MakeInfo("codex/gpt-5", false);
  info.display_name = "Codex";
  info.vision = true;
  info.context_size = 64000;
  std::vector<AgentdModelInfo> models = {info, MakeInfo("claude-code/x", true)};
  base::ListValue cache = AgentdModelsToCache(models);
  // Nothing secret is cached.
  for (const base::Value& entry : cache) {
    EXPECT_EQ(entry.GetDict().size(), 5u);
  }
  EXPECT_EQ(AgentdModelsFromCache(cache), models);
}

class KOAgentdModelsFetcherTest : public testing::Test {
 protected:
  void SetUp() override {
    ResetAgentdPortForTesting();
    ASSERT_TRUE(temp_dir_.CreateUniqueTempDir());
    shared_factory_ =
        base::MakeRefCounted<network::WeakWrapperSharedURLLoaderFactory>(
            &url_loader_factory_);
  }

  void TearDown() override { ResetAgentdPortForTesting(); }

  void WriteFile(const char* name, std::string_view contents, int mode) {
    const base::FilePath path = temp_dir_.GetPath().AppendASCII(name);
    ASSERT_TRUE(base::WriteFile(path, contents));
#if BUILDFLAG(IS_POSIX)
    ASSERT_TRUE(base::SetPosixFilePermissions(path, mode));
#endif
  }

  void WriteToken() {
    WriteFile(::ko::kAgentdTokenFileName, std::string(kToken) + "\n", 0600);
  }

  void WritePort(std::string_view port, int mode = 0600) {
    WriteFile(::ko::kAgentdPortFileName, port, mode);
  }

  // Records every request URL; models requests on |port| get |body|.
  void ServeModels(uint16_t port, std::string_view body) {
    url_loader_factory_.SetInterceptor(base::BindLambdaForTesting(
        [&](const network::ResourceRequest& request) {
          requested_urls_.push_back(request.url);
        }));
    url_loader_factory_.AddResponse(
        GetAgentdUrl(kAgentdModelsPath, port).spec(), std::string(body));
  }

  AgentdFetchResult Fetch() {
    AgentdModelsFetcher fetcher(shared_factory_, temp_dir_.GetPath());
    base::test::TestFuture<AgentdFetchResult> future;
    fetcher.Fetch(future.GetCallback());
    return future.Take();
  }

  base::test::TaskEnvironment task_environment_;
  network::TestURLLoaderFactory url_loader_factory_;
  scoped_refptr<network::SharedURLLoaderFactory> shared_factory_;
  base::ScopedTempDir temp_dir_;
  std::vector<GURL> requested_urls_;
};

#if BUILDFLAG(IS_POSIX)
TEST_F(KOAgentdModelsFetcherTest, SendsTokenAndParsesModels) {
  WriteToken();
  std::optional<std::string> authorization;
  std::optional<network::mojom::RedirectMode> redirect_mode;
  url_loader_factory_.SetInterceptor(
      base::BindLambdaForTesting([&](const network::ResourceRequest& request) {
        authorization = request.headers.GetHeader("Authorization");
        redirect_mode = request.redirect_mode;
      }));
  url_loader_factory_.AddResponse(GetAgentdUrl(kAgentdModelsPath).spec(),
                                  R"({"data": [{"id": "codex/gpt-5"}]})");

  AgentdFetchResult result = Fetch();
  EXPECT_EQ(result.token, kToken);
  EXPECT_EQ(result.port, ::ko::kAgentdDefaultPort);
  ASSERT_TRUE(result.models);
  ASSERT_EQ(result.models->size(), 1u);
  EXPECT_EQ(result.models->front().id, "codex/gpt-5");
  EXPECT_EQ(authorization, std::string("Bearer ") + kToken);
  EXPECT_EQ(redirect_mode, network::mojom::RedirectMode::kError);
}

TEST_F(KOAgentdModelsFetcherTest, ErrorKeepsTokenWithoutModels) {
  WriteToken();
  url_loader_factory_.AddResponse(GetAgentdUrl(kAgentdModelsPath).spec(), "",
                                  net::HTTP_UNAUTHORIZED);
  AgentdFetchResult result = Fetch();
  EXPECT_EQ(result.token, kToken);
  EXPECT_FALSE(result.models);
}

TEST_F(KOAgentdModelsFetcherTest, UsesPortFromPortFile) {
  WriteToken();
  WritePort("48123\n");
  ServeModels(48123, R"({"data": [{"id": "nanogpt/x"}]})");

  AgentdFetchResult result = Fetch();
  EXPECT_EQ(result.token, kToken);
  EXPECT_EQ(result.port, 48123);
  ASSERT_TRUE(result.models);
  EXPECT_EQ(result.models->size(), 1u);
  EXPECT_EQ(requested_urls_,
            std::vector<GURL>{GURL("http://127.0.0.1:48123/v1/models")});

  // Chat endpoints built from now on, and the fail-closed check, follow it.
  EXPECT_EQ(GetAgentdPort(), 48123);
  EXPECT_EQ(GetAgentdUrl(kAgentdChatCompletionsPath).spec(),
            "http://127.0.0.1:48123/v1/chat/completions");
  EXPECT_TRUE(
      IsAgentdEndpoint(GURL("http://127.0.0.1:48123/v1/chat/completions")));
  EXPECT_FALSE(
      IsAgentdEndpoint(GURL("http://127.0.0.1:48124/v1/chat/completions")));
  // Models built before the port file was read point at the default port.
  EXPECT_TRUE(
      IsAgentdEndpoint(GURL("http://127.0.0.1:47821/v1/chat/completions")));
}

TEST_F(KOAgentdModelsFetcherTest, PortSwitchWinsOverPortFile) {
  base::test::ScopedCommandLine command_line;
  command_line.GetProcessCommandLine()->AppendSwitchASCII(kAgentdPortSwitch,
                                                          "50000");
  WriteToken();
  WritePort("48123");
  ServeModels(50000, R"({"data": []})");

  AgentdFetchResult result = Fetch();
  EXPECT_EQ(result.port, 50000);
  EXPECT_TRUE(result.models);
  EXPECT_EQ(requested_urls_,
            std::vector<GURL>{GURL("http://127.0.0.1:50000/v1/models")});
  EXPECT_EQ(GetAgentdPort(), 50000);
}

TEST_F(KOAgentdModelsFetcherTest, IgnoresPortFileOthersCanWrite) {
  WriteToken();
  WritePort("48123", 0666);
  ServeModels(::ko::kAgentdDefaultPort, R"({"data": []})");

  AgentdFetchResult result = Fetch();
  EXPECT_EQ(result.port, ::ko::kAgentdDefaultPort);
  EXPECT_EQ(requested_urls_,
            std::vector<GURL>{GURL("http://127.0.0.1:47821/v1/models")});
  EXPECT_EQ(GetAgentdPort(), ::ko::kAgentdDefaultPort);
}

TEST_F(KOAgentdModelsFetcherTest, PortFileWithoutTokenSendsNothing) {
  WritePort("48123");
  ServeModels(48123, R"({"data": []})");
  AgentdFetchResult result = Fetch();
  EXPECT_FALSE(result.token);
  EXPECT_FALSE(result.models);
  EXPECT_EQ(result.port, 48123);
  EXPECT_TRUE(requested_urls_.empty());
  EXPECT_EQ(GetAgentdPort(), 48123);
}

TEST_F(KOAgentdModelsFetcherTest, RefusesTokensNotInAgentdFormat) {
  ServeModels(::ko::kAgentdDefaultPort, R"({"data": []})");
  // Characters outside base64url, or the wrong length, are not agentd's.
  for (const std::string& bad :
       {std::string(42, 'a') + "+", std::string(42, 'a') + "/",
        std::string(42, 'a') + "=", std::string(32, 'a'),
        std::string(256, 'a')}) {
    SCOPED_TRACE(bad);
    WriteFile(::ko::kAgentdTokenFileName, bad, 0600);
    AgentdFetchResult result = Fetch();
    EXPECT_FALSE(result.token);
    EXPECT_FALSE(result.models);
  }
  EXPECT_TRUE(requested_urls_.empty());
}

TEST_F(KOAgentdModelsFetcherTest, RefusesTokenOthersCanRead) {
  WriteFile(::ko::kAgentdTokenFileName, kToken, 0644);
  ServeModels(::ko::kAgentdDefaultPort, R"({"data": []})");
  AgentdFetchResult result = Fetch();
  EXPECT_FALSE(result.token);
  EXPECT_TRUE(requested_urls_.empty());
}
#endif  // BUILDFLAG(IS_POSIX)

TEST_F(KOAgentdModelsFetcherTest, NoTokenSendsNothing) {
  bool requested = false;
  url_loader_factory_.SetInterceptor(base::BindLambdaForTesting(
      [&](const network::ResourceRequest&) { requested = true; }));
  AgentdFetchResult result = Fetch();
  EXPECT_FALSE(result.token);
  EXPECT_FALSE(result.models);
  EXPECT_FALSE(requested);
}

}  // namespace ai_chat::ko
