// Copyright (c) 2026 The Brave Authors. All rights reserved.
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this file,
// You can obtain one at https://mozilla.org/MPL/2.0/.

// K-O never talks to Brave's AI servers: no premium, ratings, feedback,
// shared conversations or Brave-hosted features. The stock suites for these
// (ai_chat_credential_manager, ai_chat_service, conversation_handler) are
// left out of K-O builds, so this covers what K-O does instead.

#include <memory>
#include <optional>
#include <string>

#include "base/files/file_path.h"
#include "base/files/scoped_temp_dir.h"
#include "base/memory/scoped_refptr.h"
#include "base/test/bind.h"
#include "base/test/scoped_feature_list.h"
#include "base/test/task_environment.h"
#include "base/test/test_future.h"
#include "base/time/time.h"
#include "brave/components/ai_chat/core/browser/ai_chat_credential_manager.h"
#include "brave/components/ai_chat/core/browser/ai_chat_service.h"
#include "brave/components/ai_chat/core/browser/conversation_handler.h"
#include "brave/components/ai_chat/core/browser/model_service.h"
#include "brave/components/ai_chat/core/common/features.h"
#include "brave/components/ai_chat/core/common/mojom/ai_chat.mojom.h"
#include "brave/components/ai_chat/core/common/mojom/common.mojom.h"
#include "brave/components/ai_chat/core/common/pref_names.h"
#include "brave/components/ko/buildflags/buildflags.h"
#include "brave/components/skus/common/skus_sdk.mojom.h"
#include "components/os_crypt/async/browser/os_crypt_async.h"
#include "components/os_crypt/async/browser/test_utils.h"
#include "components/sync_preferences/testing_pref_service_syncable.h"
#include "mojo/public/cpp/bindings/pending_remote.h"
#include "services/network/public/cpp/network_context_getter.h"
#include "services/network/public/cpp/shared_url_loader_factory.h"
#include "services/network/public/cpp/weak_wrapper_shared_url_loader_factory.h"
#include "services/network/test/test_url_loader_factory.h"
#include "testing/gtest/include/gtest/gtest.h"
#include "url/gurl.h"

static_assert(BUILDFLAG(IS_KO));

namespace ai_chat {

class KOBraveServicesTest : public testing::Test {
 protected:
  void SetUp() override {
    ASSERT_TRUE(temp_dir_.CreateUniqueTempDir());
    prefs::RegisterProfilePrefs(prefs_.registry());
    prefs::RegisterLocalStatePrefs(local_state_.registry());
    ModelService::RegisterProfilePrefs(prefs_.registry());
    shared_url_loader_factory_ =
        base::MakeRefCounted<network::WeakWrapperSharedURLLoaderFactory>(
            &url_loader_factory_);
    model_service_ = std::make_unique<ModelService>(
        &prefs_, os_crypt_.get(), network::NetworkContextGetter(),
        /*url_loader_factory=*/nullptr, base::FilePath());
  }

  void TearDown() override {
    ai_chat_service_.reset();
    task_environment_.RunUntilIdle();
    model_service_.reset();
  }

  // Fails the test if anything asks for the SKUs service.
  std::unique_ptr<AIChatCredentialManager> CreateCredentialManager() {
    return std::make_unique<AIChatCredentialManager>(
        base::BindLambdaForTesting([] {
          ADD_FAILURE() << "K-O must not bind the SKUs service";
          return mojo::PendingRemote<skus::mojom::SkusService>();
        }),
        &local_state_);
  }

  AIChatService* CreateService() {
    ai_chat_service_ = std::make_unique<AIChatService>(
        model_service_.get(), /*tab_tracker_service=*/nullptr,
        CreateCredentialManager(), &prefs_, /*ai_chat_metrics=*/nullptr,
        os_crypt_.get(), shared_url_loader_factory_, "", temp_dir_.GetPath());
    return ai_chat_service_.get();
  }

  base::test::TaskEnvironment task_environment_;
  base::ScopedTempDir temp_dir_;
  sync_preferences::TestingPrefServiceSyncable prefs_;
  sync_preferences::TestingPrefServiceSyncable local_state_;
  std::unique_ptr<os_crypt_async::OSCryptAsync> os_crypt_ =
      os_crypt_async::GetTestOSCryptAsyncForTesting(
          /*is_sync_for_unittests=*/true);
  network::TestURLLoaderFactory url_loader_factory_;
  scoped_refptr<network::SharedURLLoaderFactory> shared_url_loader_factory_;
  std::unique_ptr<ModelService> model_service_;
  std::unique_ptr<AIChatService> ai_chat_service_;
};

TEST_F(KOBraveServicesTest, NoPremiumAndNoCredentials) {
  std::unique_ptr<AIChatCredentialManager> manager = CreateCredentialManager();

  base::test::TestFuture<mojom::PremiumStatus, mojom::PremiumInfoPtr> status;
  manager->GetPremiumStatus(status.GetCallback());
  EXPECT_EQ(status.Get<0>(), mojom::PremiumStatus::Inactive);

  base::test::TestFuture<std::optional<CredentialCacheEntry>> credential;
  manager->FetchPremiumCredential(credential.GetCallback());
  EXPECT_FALSE(credential.Get().has_value());

  manager->PutCredentialInCache(
      {"credential", base::Time::Now() + base::Days(1)});
  EXPECT_TRUE(
      local_state_.GetDict(prefs::kBraveChatPremiumCredentialCache).empty());
}

TEST_F(KOBraveServicesTest, RatingsAndFeedbackSendNothing) {
  ConversationHandler* conversation = CreateService()->CreateConversation();
  ASSERT_TRUE(conversation);

  base::test::TestFuture<std::optional<std::string>> rating;
  conversation->RateMessage(
      /*is_liked=*/true, "turn-uuid",
      rating.GetCallback<const std::optional<std::string>&>());
  EXPECT_FALSE(rating.Get().has_value());

  base::test::TestFuture<bool> feedback;
  conversation->SendFeedback("category", "feedback", "rating-id",
                             /*send_hostname=*/false, feedback.GetCallback());
  EXPECT_FALSE(feedback.Get());

  EXPECT_EQ(url_loader_factory_.total_requests(), 0u);
}

TEST_F(KOBraveServicesTest, ShareConversationFailsLocally) {
  base::test::TestFuture<std::optional<GURL>> shared_url;
  CreateService()->ShareConversation(
      "ciphertext", "key-fragment", "conversation-uuid", "title",
      /*copy_to_clipboard=*/false,
      shared_url.GetCallback<const std::optional<GURL>&>());
  EXPECT_FALSE(shared_url.Get().has_value());
  EXPECT_EQ(url_loader_factory_.total_requests(), 0u);
}

TEST_F(KOBraveServicesTest, BraveHostedFeaturesStayOffWhenForcedOn) {
  base::test::ScopedFeatureList feature_list;
  feature_list.InitWithFeatures(
      {features::kOpenAIChatFromBraveSearch, features::kNEARModels,
       features::kBraveSummaryModel, features::kRichSearchWidgets,
       features::kAIChatDeepResearch, features::kAIChatConversationShare},
      {});
  EXPECT_FALSE(features::IsOpenAIChatFromBraveSearchEnabled());
  EXPECT_FALSE(features::IsNEARModelsEnabled());
  EXPECT_FALSE(features::IsBraveSummaryModelEnabled());
  EXPECT_FALSE(features::IsRichSearchWidgetsEnabled());
  EXPECT_FALSE(features::IsAIChatDeepResearchEnabled());
  EXPECT_FALSE(features::IsConversationShareEnabled());
}

}  // namespace ai_chat
