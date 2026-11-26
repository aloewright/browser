// Copyright (c) 2026 The Brave Authors. All rights reserved.
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this file,
// You can obtain one at https://mozilla.org/MPL/2.0/.

#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "base/files/file_path.h"
#include "base/files/file_util.h"
#include "base/files/scoped_temp_dir.h"
#include "base/memory/scoped_refptr.h"
#include "base/test/bind.h"
#include "base/test/run_until.h"
#include "base/test/task_environment.h"
#include "brave/components/ai_chat/core/browser/engine/engine_consumer.h"
#include "brave/components/ai_chat/core/browser/ko_agentd_models.h"
#include "brave/components/ai_chat/core/browser/model_service.h"
#include "brave/components/ai_chat/core/common/constants.h"
#include "brave/components/ai_chat/core/common/mojom/common.mojom.h"
#include "brave/components/ai_chat/core/common/pref_names.h"
#include "brave/components/ko/common/agentd_config.h"
#include "build/build_config.h"
#include "components/os_crypt/async/browser/os_crypt_async.h"
#include "components/os_crypt/async/browser/test_utils.h"
#include "components/prefs/testing_pref_service.h"
#include "services/network/public/cpp/network_context_getter.h"
#include "services/network/public/cpp/resource_request.h"
#include "services/network/public/cpp/weak_wrapper_shared_url_loader_factory.h"
#include "services/network/test/test_url_loader_factory.h"
#include "testing/gtest/include/gtest/gtest.h"
#include "url/gurl.h"

#if BUILDFLAG(IS_MAC)
#include "base/base_paths.h"
#include "base/test/scoped_path_override.h"
#else
#include "base/scoped_environment_variable_override.h"
#endif

namespace ai_chat {

namespace {

// Placeholder in agentd's format (43 base64url characters), not a real token.
constexpr char kToken[] = "test_token_not_a_real_secret_0123456789-ABC";
constexpr char kDefaultModelKeyPref[] = "brave.ai_chat.default_model_key";

mojom::ModelPtr MakeCustomModel(std::string api_key, bool supports_tools) {
  auto options = mojom::CustomModelOptions::New();
  options->model_request_name = "gpt-5-mini";
  options->endpoint = GURL("https://nano-gpt.com/api/v1/chat/completions");
  options->api_key = std::move(api_key);
  options->context_size = 32000;

  auto model = mojom::Model::New();
  model->display_name = "NanoGPT";
  model->supports_tools = supports_tools;
  model->options =
      mojom::ModelOptions::NewCustomModelOptions(std::move(options));
  return model;
}

}  // namespace

class ModelServiceKOTest : public testing::Test {
 protected:
  void SetUp() override {
    ko::ResetAgentdPortForTesting();
    ASSERT_TRUE(temp_dir_.CreateUniqueTempDir());
#if BUILDFLAG(IS_MAC)
    app_data_override_.emplace(base::DIR_APP_DATA, temp_dir_.GetPath());
    data_dir_ = temp_dir_.GetPath().Append("K-O");
#else
    xdg_override_.emplace("XDG_DATA_HOME", temp_dir_.GetPath().value());
    data_dir_ = temp_dir_.GetPath().Append("k-o");
#endif
    ASSERT_TRUE(base::CreateDirectory(data_dir_));
    ASSERT_EQ(ko::GetDataDir(), data_dir_);

    prefs::RegisterProfilePrefs(pref_service_.registry());
    prefs::RegisterProfilePrefsForMigration(pref_service_.registry());
    ModelService::RegisterProfilePrefs(pref_service_.registry());
    shared_factory_ =
        base::MakeRefCounted<network::WeakWrapperSharedURLLoaderFactory>(
            &url_loader_factory_);
  }

  void TearDown() override {
    service_.reset();
    ko::ResetAgentdPortForTesting();
  }

  void WritePrivateFile(const char* name, std::string_view contents) {
    const base::FilePath path = data_dir_.AppendASCII(name);
    ASSERT_TRUE(base::WriteFile(path, contents));
#if BUILDFLAG(IS_POSIX)
    ASSERT_TRUE(base::SetPosixFilePermissions(path, 0600));
#endif
  }

  void WriteToken() {
    WritePrivateFile(::ko::kAgentdTokenFileName, std::string(kToken) + "\n");
  }

  ModelService* CreateService() {
    service_ = std::make_unique<ModelService>(
        &pref_service_, os_crypt_async_.get(), network::NetworkContextGetter(),
        shared_factory_, base::FilePath());
    return service_.get();
  }

  base::test::TaskEnvironment task_environment_;
  base::ScopedTempDir temp_dir_;
  base::FilePath data_dir_;
#if BUILDFLAG(IS_MAC)
  std::optional<base::ScopedPathOverride> app_data_override_;
#else
  std::optional<base::ScopedEnvironmentVariableOverride> xdg_override_;
#endif
  network::TestURLLoaderFactory url_loader_factory_;
  scoped_refptr<network::SharedURLLoaderFactory> shared_factory_;
  TestingPrefServiceSimple pref_service_;
  std::unique_ptr<os_crypt_async::OSCryptAsync> os_crypt_async_ =
      os_crypt_async::GetTestOSCryptAsyncForTesting(
          /*is_sync_for_unittests=*/true);
  std::unique_ptr<ModelService> service_;
};

TEST_F(ModelServiceKOTest, NoLeoModelsAndAgentdDefaultFirst) {
  ModelService* service = CreateService();
  const auto& models = service->GetModels();
  ASSERT_FALSE(models.empty());
  EXPECT_EQ(models[0]->key, kChatAutomaticModelKey);
  for (const auto& model : models) {
    EXPECT_TRUE(model->options->is_custom_model_options()) << model->key;
    EXPECT_FALSE(model->supports_private_inference) << model->key;
  }
  EXPECT_EQ(service->GetModel("chat-claude-sonnet"), nullptr);
  EXPECT_EQ(ModelService::GetModelForTesting(kChatAutomaticModelKey), nullptr);
  EXPECT_EQ(service->GetDefaultModelKey(), kChatAutomaticModelKey);

  std::unique_ptr<EngineConsumer> engine = service->GetEngineForModel(
      kChatAutomaticModelKey, shared_factory_, /*credential_manager=*/nullptr);
  EXPECT_TRUE(engine);
  // An unknown key still resolves to agentd's default.
  EXPECT_TRUE(service->GetEngineForModel("chat-claude-sonnet", shared_factory_,
                                         /*credential_manager=*/nullptr));
}

#if BUILDFLAG(IS_POSIX)
TEST_F(ModelServiceKOTest, LoadsAgentdModelsWithToken) {
  WriteToken();
  url_loader_factory_.AddResponse(
      ko::GetAgentdUrl(ko::kAgentdModelsPath).spec(),
      R"({"data": [{"id": "claude-code/sonnet"}, {"id": "codex/gpt-5"}]})");

  ModelService* service = CreateService();
  ASSERT_TRUE(base::test::RunUntil(
      [&] { return service->GetModel("ko-agentd:codex/gpt-5") != nullptr; }));

  const mojom::Model* automatic = service->GetModel(kChatAutomaticModelKey);
  ASSERT_TRUE(automatic);
  const auto& opts = automatic->options->get_custom_model_options();
  EXPECT_EQ(opts->model_request_name, "claude-code/sonnet");
  EXPECT_EQ(opts->api_key, kToken);
  EXPECT_EQ(opts->endpoint, ko::GetAgentdUrl(ko::kAgentdChatCompletionsPath));

  // Pages never get the token.
  for (const auto& model : service->GetModelsForUI()) {
    EXPECT_TRUE(model->options->get_custom_model_options()->api_key.empty());
  }
  for (const auto& model : service->GetModelsWithSubtitles()) {
    EXPECT_TRUE(
        model->model->options->get_custom_model_options()->api_key.empty());
  }
  // Nor do prefs: only ids are cached.
  const base::ListValue& cache =
      pref_service_.GetList(ko::kAgentdModelsCachePref);
  EXPECT_EQ(cache.size(), 2u);
  EXPECT_EQ(cache.DebugString().find(kToken), std::string::npos);
  // agentd models aren't user custom models.
  EXPECT_TRUE(service->GetCustomModels().empty());
}

TEST_F(ModelServiceKOTest, FollowsAgentdPortFile) {
  WriteToken();
  WritePrivateFile(::ko::kAgentdPortFileName, "48123\n");
  std::vector<GURL> requested;
  url_loader_factory_.SetInterceptor(
      base::BindLambdaForTesting([&](const network::ResourceRequest& request) {
        requested.push_back(request.url);
      }));
  url_loader_factory_.AddResponse(
      "http://127.0.0.1:48123/v1/models",
      R"({"data": [{"id": "nanogpt/gpt-5-mini"}]})");

  ModelService* service = CreateService();
  ASSERT_TRUE(base::test::RunUntil([&] {
    return service->GetModel("ko-agentd:nanogpt/gpt-5-mini") != nullptr;
  }));

  // Nothing went to the default port, where another listener could be.
  EXPECT_EQ(requested,
            std::vector<GURL>{GURL("http://127.0.0.1:48123/v1/models")});
  for (const char* key :
       {kChatAutomaticModelKey, "ko-agentd:nanogpt/gpt-5-mini"}) {
    SCOPED_TRACE(key);
    const mojom::Model* model = service->GetModel(key);
    ASSERT_TRUE(model);
    const auto& opts = model->options->get_custom_model_options();
    EXPECT_EQ(opts->endpoint,
              GURL("http://127.0.0.1:48123/v1/chat/completions"));
    EXPECT_EQ(opts->api_key, kToken);
    EXPECT_TRUE(ko::IsAgentdEndpoint(opts->endpoint));
  }
  EXPECT_TRUE(
      service->GetModel("ko-agentd:nanogpt/gpt-5-mini")->supports_tools);
}

TEST_F(ModelServiceKOTest, CachedModelsResolveBeforeAgentdAnswers) {
  pref_service_.SetList(ko::kAgentdModelsCachePref,
                        ko::AgentdModelsToCache({[] {
                          ko::AgentdModelInfo info;
                          info.id = "codex/gpt-5";
                          return info;
                        }()}));
  ModelService* service = CreateService();
  const mojom::Model* model = service->GetModel("ko-agentd:codex/gpt-5");
  ASSERT_TRUE(model);
  // No token yet, so requests fail closed until it is read.
  EXPECT_TRUE(model->options->get_custom_model_options()->api_key.empty());
}
#endif  // BUILDFLAG(IS_POSIX)

TEST_F(ModelServiceKOTest, CustomModelsWithToolsCanRunAgentTools) {
  ModelService* service = CreateService();
  service->AddCustomModel(MakeCustomModel("secret-key", true));
  service->AddCustomModel(MakeCustomModel("other-key", false));
  std::vector<mojom::ModelPtr> custom = service->GetCustomModels();
  ASSERT_EQ(custom.size(), 2u);
  EXPECT_EQ(custom[0]->supported_capabilities,
            std::vector<mojom::ConversationCapability>{
                mojom::ConversationCapability::CONTENT_AGENT});
  EXPECT_TRUE(custom[1]->supported_capabilities.empty());
}

TEST_F(ModelServiceKOTest, SavingEmptyApiKeyKeepsStoredKey) {
  ModelService* service = CreateService();
  service->AddCustomModel(MakeCustomModel("secret-key", true));
  std::vector<mojom::ModelPtr> custom = service->GetCustomModels();
  ASSERT_EQ(custom.size(), 1u);
  ASSERT_EQ(custom[0]->options->get_custom_model_options()->api_key,
            "secret-key");

  mojom::ModelPtr edited = custom[0]->Clone();
  edited->display_name = "Renamed";
  edited->options->get_custom_model_options()->api_key.clear();
  service->SaveCustomModel(0, std::move(edited));

  std::vector<mojom::ModelPtr> saved = service->GetCustomModels();
  ASSERT_EQ(saved.size(), 1u);
  EXPECT_EQ(saved[0]->display_name, "Renamed");
  EXPECT_EQ(saved[0]->options->get_custom_model_options()->api_key,
            "secret-key");
}

TEST_F(ModelServiceKOTest, SavingEmptyApiKeyForNewOriginDropsStoredKey) {
  ModelService* service = CreateService();
  service->AddCustomModel(MakeCustomModel("secret-key", true));
  std::vector<mojom::ModelPtr> custom = service->GetCustomModels();
  ASSERT_EQ(custom.size(), 1u);

  mojom::ModelPtr edited = custom[0]->Clone();
  edited->options->get_custom_model_options()->endpoint =
      GURL("https://example.com/v1/chat/completions");
  edited->options->get_custom_model_options()->api_key.clear();
  service->SaveCustomModel(0, std::move(edited));

  std::vector<mojom::ModelPtr> saved = service->GetCustomModels();
  ASSERT_EQ(saved.size(), 1u);
  EXPECT_EQ(saved[0]->options->get_custom_model_options()->endpoint,
            GURL("https://example.com/v1/chat/completions"));
  EXPECT_TRUE(saved[0]->options->get_custom_model_options()->api_key.empty());
}

TEST_F(ModelServiceKOTest, MigrationClearsLeoDefault) {
  pref_service_.SetString(kDefaultModelKeyPref, "chat-claude-sonnet");
  ModelService::MigrateProfilePrefs(&pref_service_);
  EXPECT_FALSE(pref_service_.HasPrefPath(kDefaultModelKeyPref));

  pref_service_.SetString(kDefaultModelKeyPref, "ko-agentd:codex/gpt-5");
  ModelService::MigrateProfilePrefs(&pref_service_);
  EXPECT_EQ(pref_service_.GetString(kDefaultModelKeyPref),
            "ko-agentd:codex/gpt-5");

  pref_service_.SetString(kDefaultModelKeyPref, "custom:1a2b3c4d");
  ModelService::MigrateProfilePrefs(&pref_service_);
  EXPECT_EQ(pref_service_.GetString(kDefaultModelKeyPref), "custom:1a2b3c4d");
}

}  // namespace ai_chat
