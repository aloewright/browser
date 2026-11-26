// Copyright (c) 2026 The Brave Authors. All rights reserved.
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this file,
// You can obtain one at https://mozilla.org/MPL/2.0/.

#include "brave/components/ai_chat/core/browser/ko_agentd_models.h"

#include <optional>
#include <string>
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
#include "build/build_config.h"
#include "net/http/http_status_code.h"
#include "services/network/public/cpp/resource_request.h"
#include "services/network/public/cpp/weak_wrapper_shared_url_loader_factory.h"
#include "services/network/test/test_url_loader_factory.h"
#include "testing/gtest/include/gtest/gtest.h"
#include "url/gurl.h"

namespace ai_chat::ko {

namespace {

constexpr char kToken[] = "test-token-0123456789abcdef";

AgentdModelInfo MakeInfo(std::string id, bool tools) {
  AgentdModelInfo info;
  info.id = std::move(id);
  info.tools = tools;
  return info;
}

}  // namespace

TEST(KOAgentdModelsTest, UrlsAreLoopbackOnly) {
  EXPECT_EQ(GetAgentdUrl(kAgentdModelsPath).spec(),
            "http://127.0.0.1:47821/v1/models");
  EXPECT_TRUE(IsAgentdEndpoint(GetAgentdUrl(kAgentdChatCompletionsPath)));
  EXPECT_FALSE(
      IsAgentdEndpoint(GURL("http://localhost:47821/v1/chat/completions")));
  EXPECT_FALSE(IsAgentdEndpoint(GURL("https://127.0.0.1:47821/v1/models")));
  EXPECT_FALSE(IsAgentdEndpoint(GURL("http://127.0.0.1:1234/v1/models")));
  EXPECT_FALSE(IsAgentdEndpoint(GURL("http://example.com:47821/v1/models")));
}

TEST(KOAgentdModelsTest, PortSwitch) {
  base::test::ScopedCommandLine command_line;
  command_line.GetProcessCommandLine()->AppendSwitchASCII(kAgentdPortSwitch,
                                                          "50000");
  EXPECT_EQ(GetAgentdPort(), 50000);
  EXPECT_EQ(GetAgentdUrl(kAgentdModelsPath).spec(),
            "http://127.0.0.1:50000/v1/models");
}

TEST(KOAgentdModelsTest, InvalidPortSwitchIsIgnored) {
  for (const char* value : {"0", "70000", "abc", "-1", ""}) {
    base::test::ScopedCommandLine command_line;
    command_line.GetProcessCommandLine()->AppendSwitchASCII(kAgentdPortSwitch,
                                                            value);
    EXPECT_EQ(GetAgentdPort(), kAgentdDefaultPort) << value;
  }
}

TEST(KOAgentdModelsTest, ModelKeys) {
  EXPECT_TRUE(IsAgentdModelKey(kChatAutomaticModelKey));
  EXPECT_TRUE(IsAgentdModelKey("ko-agentd:codex/gpt-5"));
  EXPECT_FALSE(IsAgentdModelKey("custom:1a2b3c4d"));
  EXPECT_FALSE(IsAgentdModelKey("chat-claude-sonnet"));
}

TEST(KOAgentdModelsTest, TokenValidation) {
  EXPECT_TRUE(IsValidAgentdToken(kToken));
  EXPECT_TRUE(IsValidAgentdToken("AbCdEfGhIjKlMnOp_-.~+/="));
  EXPECT_FALSE(IsValidAgentdToken(""));
  EXPECT_FALSE(IsValidAgentdToken("short"));
  EXPECT_FALSE(IsValidAgentdToken("has space 0123456789"));
  EXPECT_FALSE(IsValidAgentdToken("header\r\ninjection0123"));
  EXPECT_FALSE(IsValidAgentdToken(std::string(513, 'a')));
}

#if BUILDFLAG(IS_POSIX)
class KOAgentdTokenFileTest : public testing::Test {
 protected:
  void SetUp() override { ASSERT_TRUE(temp_dir_.CreateUniqueTempDir()); }

  base::FilePath token_path() const {
    return temp_dir_.GetPath().Append(kAgentdTokenFileName);
  }

  void WriteToken(std::string_view contents, int mode) {
    ASSERT_TRUE(base::WriteFile(token_path(), contents));
    ASSERT_TRUE(base::SetPosixFilePermissions(token_path(), mode));
  }

  base::ScopedTempDir temp_dir_;
};

TEST_F(KOAgentdTokenFileTest, ReadsOwnerOnlyToken) {
  WriteToken(std::string(kToken) + "\n", 0600);
  EXPECT_EQ(ReadAgentdToken(temp_dir_.GetPath()), kToken);
}

TEST_F(KOAgentdTokenFileTest, RejectsGroupOrOtherAccess) {
  WriteToken(kToken, 0640);
  EXPECT_EQ(ReadAgentdToken(temp_dir_.GetPath()), std::nullopt);
  WriteToken(kToken, 0604);
  EXPECT_EQ(ReadAgentdToken(temp_dir_.GetPath()), std::nullopt);
}

TEST_F(KOAgentdTokenFileTest, RejectsSymlink) {
  const base::FilePath target = temp_dir_.GetPath().Append("real");
  ASSERT_TRUE(base::WriteFile(target, kToken));
  ASSERT_TRUE(base::SetPosixFilePermissions(target, 0600));
  ASSERT_TRUE(base::CreateSymbolicLink(target, token_path()));
  EXPECT_EQ(ReadAgentdToken(temp_dir_.GetPath()), std::nullopt);
}

TEST_F(KOAgentdTokenFileTest, RejectsMissingMalformedOrOversized) {
  EXPECT_EQ(ReadAgentdToken(temp_dir_.GetPath()), std::nullopt);
  EXPECT_EQ(ReadAgentdToken(base::FilePath()), std::nullopt);
  WriteToken("not a token", 0600);
  EXPECT_EQ(ReadAgentdToken(temp_dir_.GetPath()), std::nullopt);
  WriteToken(std::string(4096, 'a'), 0600);
  EXPECT_EQ(ReadAgentdToken(temp_dir_.GetPath()), std::nullopt);
}
#endif  // BUILDFLAG(IS_POSIX)

TEST(KOAgentdModelsTest, ParsesModelList) {
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
  EXPECT_TRUE((*models)[0].tools);
  EXPECT_EQ((*models)[0].context_size, kAgentdDefaultContextSize);

  EXPECT_EQ((*models)[1].id, "codex/gpt-5");
  EXPECT_EQ((*models)[1].display_name, "Codex GPT-5");
  EXPECT_TRUE((*models)[1].vision);
  EXPECT_FALSE((*models)[1].tools);
  EXPECT_EQ((*models)[1].context_size, 200000u);

  // Control characters drop the name; too small a context is ignored.
  EXPECT_EQ((*models)[2].id, "nanogpt/x");
  EXPECT_EQ((*models)[2].display_name, "");
  EXPECT_EQ((*models)[2].context_size, kAgentdDefaultContextSize);
}

TEST(KOAgentdModelsTest, RejectsWrongShape) {
  EXPECT_EQ(ParseAgentdModels(base::test::ParseJson("[]")), std::nullopt);
  EXPECT_EQ(ParseAgentdModels(base::test::ParseJson(R"({"data": {}})")),
            std::nullopt);
  std::optional<std::vector<AgentdModelInfo>> empty =
      ParseAgentdModels(base::test::ParseJson(R"({"data": []})"));
  ASSERT_TRUE(empty);
  EXPECT_TRUE(empty->empty());
}

TEST(KOAgentdModelsTest, BuildModelsLeadsWithAutomatic) {
  std::vector<mojom::ModelPtr> models = BuildAgentdModels(
      {MakeInfo("nanogpt/gpt-5-mini", true), MakeInfo("codex/gpt-5", false)},
      kToken);
  ASSERT_EQ(models.size(), 3u);

  EXPECT_EQ(models[0]->key, kChatAutomaticModelKey);
  EXPECT_TRUE(models[0]->is_suggested_model);
  ASSERT_TRUE(models[0]->options->is_custom_model_options());
  const auto& default_opts = models[0]->options->get_custom_model_options();
  EXPECT_EQ(default_opts->model_request_name, "nanogpt/gpt-5-mini");
  EXPECT_EQ(default_opts->endpoint, GetAgentdUrl(kAgentdChatCompletionsPath));
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
}

TEST(KOAgentdModelsTest, BuildModelsWithoutAgentdKeepsDefault) {
  std::vector<mojom::ModelPtr> models = BuildAgentdModels({}, std::string());
  ASSERT_EQ(models.size(), 1u);
  EXPECT_EQ(models[0]->key, kChatAutomaticModelKey);
  EXPECT_EQ(models[0]->options->get_custom_model_options()->model_request_name,
            kAgentdFallbackModelName);
  EXPECT_TRUE(models[0]->options->get_custom_model_options()->api_key.empty());
}

TEST(KOAgentdModelsTest, CacheRoundTrip) {
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
    ASSERT_TRUE(temp_dir_.CreateUniqueTempDir());
    shared_factory_ =
        base::MakeRefCounted<network::WeakWrapperSharedURLLoaderFactory>(
            &url_loader_factory_);
  }

  void WriteToken() {
    const base::FilePath path =
        temp_dir_.GetPath().Append(kAgentdTokenFileName);
    ASSERT_TRUE(base::WriteFile(path, kToken));
#if BUILDFLAG(IS_POSIX)
    ASSERT_TRUE(base::SetPosixFilePermissions(path, 0600));
#endif
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
