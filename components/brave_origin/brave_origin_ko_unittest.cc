/* Copyright (c) 2026 The Brave Authors. All rights reserved.
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this file,
 * You can obtain one at https://mozilla.org/MPL/2.0/. */

// K-O has no Origin purchase: whatever the feature flag, a cached purchase or
// the Linux free tier say, Origin is never purchased and never asks the SKU
// service.

#include <memory>
#include <optional>
#include <utility>

#include "base/memory/raw_ptr.h"
#include "base/test/scoped_feature_list.h"
#include "base/test/task_environment.h"
#include "base/test/test_future.h"
#include "base/values.h"
#include "brave/components/brave_origin/brave_origin_policy_info.h"
#include "brave/components/brave_origin/brave_origin_policy_manager.h"
#include "brave/components/brave_origin/brave_origin_service.h"
#include "brave/components/brave_origin/brave_origin_settings_handler_impl.h"
#include "brave/components/brave_origin/brave_origin_utils.h"
#include "brave/components/brave_origin/features.h"
#include "brave/components/brave_origin/pref_names.h"
#include "brave/components/skus/browser/pref_names.h"
#include "build/build_config.h"
#include "components/policy/core/common/mock_policy_service.h"
#include "components/prefs/pref_registry_simple.h"
#include "components/prefs/testing_pref_service.h"
#include "testing/gtest/include/gtest/gtest.h"

namespace brave_origin {

namespace {

constexpr char kTestProfileId[] = "test-profile-id";
constexpr char kTestBrowserPref[] = "test.browser.pref";
constexpr char kTestBrowserPolicyKey[] = "test.browser.pref.policy";

// Counts SKU service requests.
class CountingDelegate : public BraveOriginService::Delegate {
 public:
  explicit CountingDelegate(int* skus_requests)
      : skus_requests_(skus_requests) {}
  void OpenOriginSettings() override {}
  mojo::PendingRemote<skus::mojom::SkusService> GetSkusService() override {
    ++*skus_requests_;
    return {};
  }

 private:
  raw_ptr<int> skus_requests_;
};

}  // namespace

class BraveOriginKoTest : public testing::Test {
 public:
  void SetUp() override {
    // K-O ignores the flag, so force it on.
    feature_list_.InitAndEnableFeature(features::kBraveOrigin);

    local_state_.registry()->RegisterDictionaryPref(kBraveOriginPolicies);
    local_state_.registry()->RegisterBooleanPref(kOriginPurchaseValidated,
                                                 false);
    local_state_.registry()->RegisterBooleanPref(kOriginPoliciesWereEnforced,
                                                 false);
    local_state_.registry()->RegisterDictionaryPref(skus::prefs::kSkusState);
#if BUILDFLAG(IS_LINUX)
    local_state_.registry()->RegisterBooleanPref(kOriginFreeTierAccepted,
                                                 false);
#endif
    local_state_.registry()->RegisterBooleanPref(kTestBrowserPref, false);

    BraveOriginPolicyMap browser_policies;
    browser_policies.emplace(
        kTestBrowserPolicyKey,
        BraveOriginPolicyInfo(kTestBrowserPref, false, true,
                              "test.browser.pref.brave_origin_key"));
    BraveOriginPolicyManager::GetInstance()->Init(
        std::move(browser_policies), BraveOriginPolicyMap(), &local_state_);
  }

  void TearDown() override {
    service_.reset();
    BraveOriginPolicyManager::GetInstance()->Shutdown();
  }

 protected:
  void CreateService() {
    service_ = std::make_unique<BraveOriginService>(
        &local_state_, &profile_prefs_, kTestProfileId,
        &profile_policy_service_, &browser_policy_service_,
        std::make_unique<CountingDelegate>(&skus_requests_));
  }

  base::test::TaskEnvironment task_environment_;
  base::test::ScopedFeatureList feature_list_;
  TestingPrefServiceSimple local_state_;
  TestingPrefServiceSimple profile_prefs_;
  policy::MockPolicyService profile_policy_service_;
  policy::MockPolicyService browser_policy_service_;
  int skus_requests_ = 0;
  std::unique_ptr<BraveOriginService> service_;
};

TEST_F(BraveOriginKoTest, FeatureNeverEnabled) {
  EXPECT_FALSE(IsBraveOriginFeatureEnabled());
}

TEST_F(BraveOriginKoTest, CachedPurchaseIgnored) {
  BraveOriginPolicyManager::GetInstance()->SetPurchased(true);
  EXPECT_FALSE(IsBraveOriginPurchased());
}

TEST_F(BraveOriginKoTest, NeverAsksSkuService) {
  CreateService();

  base::test::TestFuture<bool> purchased;
  service_->CheckPurchaseState(purchased.GetCallback());
  EXPECT_FALSE(purchased.Get());

  // New SKU credentials don't trigger a check either.
  base::DictValue skus_state;
  skus_state.Set("skus:production", "{}");
  local_state_.SetDict(skus::prefs::kSkusState, std::move(skus_state));

  EXPECT_EQ(0, skus_requests_);
}

TEST_F(BraveOriginKoTest, PoliciesNotControlled) {
  BraveOriginPolicyManager::GetInstance()->SetPurchased(true);
  CreateService();

  EXPECT_FALSE(
      service_->IsPolicyControlledByBraveOrigin(kTestBrowserPolicyKey));
  EXPECT_FALSE(service_->SetPolicyValue(kTestBrowserPolicyKey, true));
  EXPECT_FALSE(local_state_.GetBoolean(kTestBrowserPref));
}

TEST_F(BraveOriginKoTest, SettingsHandlerOffersNothing) {
  BraveOriginPolicyManager::GetInstance()->SetPurchased(true);
  CreateService();
  BraveOriginSettingsHandlerImpl handler(service_.get());

  base::test::TestFuture<bool> is_user;
  handler.IsBraveOriginUser(is_user.GetCallback());
  EXPECT_FALSE(is_user.Get());

  base::test::TestFuture<bool> refreshed;
  handler.RefreshPurchaseState(refreshed.GetCallback());
  EXPECT_FALSE(refreshed.Get());

  base::test::TestFuture<std::optional<bool>> value;
  handler.GetPolicyValue(kTestBrowserPolicyKey, value.GetCallback());
  EXPECT_FALSE(value.Get().has_value());

  base::test::TestFuture<bool> proceed_free;
  handler.ProceedFree(proceed_free.GetCallback());
  EXPECT_FALSE(proceed_free.Get());

  EXPECT_EQ(0, skus_requests_);
}

#if BUILDFLAG(IS_LINUX)
TEST_F(BraveOriginKoTest, FreeTierAcceptedEarlierIsIgnored) {
  local_state_.SetBoolean(kOriginFreeTierAccepted, true);
  CreateService();
  EXPECT_FALSE(BraveOriginPolicyManager::GetInstance()->IsPurchased());
}

TEST_F(BraveOriginKoTest, AcceptFreeTierDoesNothing) {
  CreateService();
  service_->AcceptFreeTier();
  EXPECT_FALSE(service_->IsFreeTierAccepted());
  EXPECT_FALSE(BraveOriginPolicyManager::GetInstance()->IsPurchased());
}
#endif  // BUILDFLAG(IS_LINUX)

}  // namespace brave_origin
