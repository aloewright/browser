// Copyright (c) 2026 The Brave Authors. All rights reserved.
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this file,
// You can obtain one at https://mozilla.org/MPL/2.0/.

#include "brave/components/ko/machines/browser/machines_handler.h"

#include <memory>
#include <optional>
#include <string>
#include <tuple>
#include <vector>

#include "base/files/file_util.h"
#include "base/files/scoped_temp_dir.h"
#include "base/test/run_until.h"
#include "base/test/task_environment.h"
#include "base/test/test_future.h"
#include "brave/components/ko/machines/browser/credential_store.h"
#include "brave/components/ko/machines/core/machines_config.h"
#include "brave/components/ko/machines/core/pref_names.h"
#include "brave/components/ko/machines/core/prefs.h"
#include "build/build_config.h"
#include "components/os_crypt/async/browser/os_crypt_async.h"
#include "components/os_crypt/async/browser/test_utils.h"
#include "components/prefs/testing_pref_service.h"
#include "mojo/public/cpp/bindings/remote.h"
#include "testing/gtest/include/gtest/gtest.h"

namespace ko_machines {

namespace {

mojom::MachineInputPtr Input(
    const std::string& name,
    const std::string& host,
    uint16_t port = 5900,
    mojom::MachineKind kind = mojom::MachineKind::kScreenSharing) {
  return mojom::MachineInput::New(name, host, port, kind);
}

}  // namespace

class KOMachinesHandlerTest : public testing::Test {
 protected:
  void SetUp() override {
    ASSERT_TRUE(temp_dir_.CreateUniqueTempDir());
    RegisterProfilePrefs(prefs_.registry());
    os_crypt_ = os_crypt_async::GetTestOSCryptAsyncForTesting(
        /*is_sync_for_unittests=*/true);
  }

  void CreateHandler() {
    handler_ = std::make_unique<MachinesHandler>(
        remote_.BindNewPipeAndPassReceiver(), prefs_, os_crypt_.get(),
        temp_dir_.GetPath());
  }

  std::tuple<mojom::MachineError, std::optional<std::string>> Add(
      mojom::MachineInputPtr input) {
    base::test::TestFuture<mojom::MachineError, std::optional<std::string>>
        future;
    handler_->AddMachine(
        std::move(input),
        future.GetCallback<mojom::MachineError,
                           const std::optional<std::string>&>());
    return future.Take();
  }

  mojom::MachineError Update(const std::string& id,
                             mojom::MachineInputPtr input) {
    base::test::TestFuture<mojom::MachineError> future;
    handler_->UpdateMachine(id, std::move(input), future.GetCallback());
    return future.Take();
  }

  mojom::MachineError SetCredentials(const std::string& id,
                                     std::optional<std::string> username,
                                     const std::string& password) {
    base::test::TestFuture<mojom::MachineError> future;
    handler_->SetCredentials(id, username, password, future.GetCallback());
    return future.Take();
  }

  std::tuple<std::optional<std::string>, std::optional<std::string>>
  GetCredentials(const std::string& id) {
    base::test::TestFuture<std::optional<std::string>,
                           std::optional<std::string>>
        future;
    handler_->GetCredentials(
        id, future.GetCallback<const std::optional<std::string>&,
                               const std::optional<std::string>&>());
    return future.Take();
  }

  base::test::TaskEnvironment task_environment_;
  base::ScopedTempDir temp_dir_;
  TestingPrefServiceSimple prefs_;
  std::unique_ptr<os_crypt_async::OSCryptAsync> os_crypt_;
  mojo::Remote<mojom::MachinesHandler> remote_;
  std::unique_ptr<MachinesHandler> handler_;
};

TEST_F(KOMachinesHandlerTest, AddValidatesAndCanonicalizes) {
  prefs_.SetBoolean(prefs::kSeeded, true);
  CreateHandler();

  auto [error, id] = Add(Input("  Studio  ", "Studio.NetBird.example"));
  EXPECT_EQ(mojom::MachineError::kNone, error);
  ASSERT_TRUE(id);
  std::optional<Machine> stored = FindMachine(prefs_, *id);
  ASSERT_TRUE(stored);
  EXPECT_EQ("Studio", stored->name);
  EXPECT_EQ("studio.netbird.example", stored->host);

  EXPECT_EQ(mojom::MachineError::kInvalidName,
            std::get<0>(Add(Input("", "100.64.0.11"))));
  EXPECT_EQ(mojom::MachineError::kInvalidHost,
            std::get<0>(Add(Input("Box", "vnc://100.64.0.11"))));
  EXPECT_EQ(mojom::MachineError::kInvalidPort,
            std::get<0>(Add(Input("Box", "100.64.0.11", 0))));
  EXPECT_EQ(mojom::MachineError::kDuplicate,
            std::get<0>(Add(Input("Again", "studio.netbird.example"))));
  EXPECT_EQ(1u, GetMachines(prefs_).size());
}

TEST_F(KOMachinesHandlerTest, CredentialsAreEncryptedAndReturned) {
  prefs_.SetBoolean(prefs::kSeeded, true);
  CreateHandler();
  auto [error, id] = Add(Input("Studio", "100.64.0.10"));
  ASSERT_TRUE(id);

  EXPECT_EQ(mojom::MachineError::kNone,
            SetCredentials(*id, "alex", "s3cret-pass"));
  std::optional<std::string> stored = GetStoredCredentials(prefs_, *id);
  ASSERT_TRUE(stored);
  EXPECT_EQ(std::string::npos, stored->find("s3cret-pass"));

  auto [username, password] = GetCredentials(*id);
  EXPECT_EQ("alex", username);
  EXPECT_EQ("s3cret-pass", password);

  EXPECT_EQ(mojom::MachineError::kInvalidCredentials,
            SetCredentials(*id, std::nullopt, ""));
  EXPECT_EQ(mojom::MachineError::kNotFound,
            SetCredentials("missing", std::nullopt, "x"));
  auto [missing_username, missing_password] = GetCredentials("missing");
  EXPECT_FALSE(missing_username);
  EXPECT_FALSE(missing_password);
}

TEST_F(KOMachinesHandlerTest, ChangingHostForgetsCredentials) {
  prefs_.SetBoolean(prefs::kSeeded, true);
  CreateHandler();
  auto [error, id] = Add(Input("Studio", "100.64.0.10"));
  ASSERT_TRUE(id);
  ASSERT_EQ(mojom::MachineError::kNone,
            SetCredentials(*id, std::nullopt, "vncpass"));

  // Renaming keeps them.
  EXPECT_EQ(mojom::MachineError::kNone,
            Update(*id, Input("Studio Mac", "100.64.0.10")));
  EXPECT_TRUE(HasStoredCredentials(prefs_, *id));

  // A new host drops them.
  EXPECT_EQ(mojom::MachineError::kNone,
            Update(*id, Input("Studio Mac", "100.64.0.99")));
  EXPECT_FALSE(HasStoredCredentials(prefs_, *id));
  EXPECT_FALSE(std::get<1>(GetCredentials(*id)));
}

TEST_F(KOMachinesHandlerTest, RemoveDropsCredentials) {
  prefs_.SetBoolean(prefs::kSeeded, true);
  CreateHandler();
  auto [error, id] = Add(Input("Studio", "100.64.0.10"));
  ASSERT_TRUE(id);
  ASSERT_EQ(mojom::MachineError::kNone,
            SetCredentials(*id, std::nullopt, "vncpass"));

  base::test::TestFuture<bool> removed;
  handler_->RemoveMachine(*id, removed.GetCallback());
  EXPECT_TRUE(removed.Take());
  EXPECT_TRUE(GetMachines(prefs_).empty());
  EXPECT_FALSE(HasStoredCredentials(prefs_, *id));
}

TEST_F(KOMachinesHandlerTest, SeedsFromConfigFileOnce) {
  const base::FilePath config =
      temp_dir_.GetPath().Append(kMachinesConfigFileName);
  ASSERT_TRUE(base::WriteFile(config, R"({
    "allowed_cidrs": ["100.64.0.0/10", "192.168.1.0/24"],
    "machines": [{"id": "studio", "name": "Studio", "host": "100.64.0.10"}]
  })"));
#if BUILDFLAG(IS_POSIX)
  ASSERT_TRUE(base::SetPosixFilePermissions(config, 0600));
#endif

  CreateHandler();
  ASSERT_TRUE(
      base::test::RunUntil([&] { return prefs_.GetBoolean(prefs::kSeeded); }));
  EXPECT_TRUE(FindMachine(prefs_, "studio"));
  EXPECT_EQ((std::vector<std::string>{"100.64.0.0/10", "192.168.1.0/24"}),
            GetAllowedCidrs(prefs_));

  // Seeding runs once: a removed machine stays removed on the next start.
  SetMachines(prefs_, {});
  handler_.reset();
  remote_.reset();
  CreateHandler();
  EXPECT_TRUE(prefs_.GetBoolean(prefs::kSeeded));
  EXPECT_TRUE(GetMachines(prefs_).empty());
}

TEST_F(KOMachinesHandlerTest, ImportAddsNewMachinesOnly) {
  prefs_.SetBoolean(prefs::kSeeded, true);
  CreateHandler();
  ASSERT_EQ(mojom::MachineError::kNone,
            std::get<0>(Add(Input("Existing", "100.64.0.10"))));

  {
    base::test::TestFuture<mojom::ImportStatus, uint32_t, bool> future;
    handler_->ImportFromFile(future.GetCallback());
    EXPECT_EQ(mojom::ImportStatus::kNotFound, future.Get<0>());
  }

  const base::FilePath config =
      temp_dir_.GetPath().Append(kMachinesConfigFileName);
  ASSERT_TRUE(base::WriteFile(config, R"({"machines": [
    {"name": "Same endpoint", "host": "100.64.0.10"},
    {"name": "New", "host": "100.64.0.20"}
  ]})"));
#if BUILDFLAG(IS_POSIX)
  ASSERT_TRUE(base::SetPosixFilePermissions(config, 0600));
#endif
  base::test::TestFuture<mojom::ImportStatus, uint32_t, bool> future;
  handler_->ImportFromFile(future.GetCallback());
  auto [status, added, allowlist_updated] = future.Take();
  EXPECT_EQ(mojom::ImportStatus::kImported, status);
  EXPECT_EQ(1u, added);
  EXPECT_FALSE(allowlist_updated);
  EXPECT_EQ(2u, GetMachines(prefs_).size());
}

#if BUILDFLAG(IS_POSIX)
TEST_F(KOMachinesHandlerTest, ImportRefusesInsecureFile) {
  prefs_.SetBoolean(prefs::kSeeded, true);
  CreateHandler();
  const base::FilePath config =
      temp_dir_.GetPath().Append(kMachinesConfigFileName);
  ASSERT_TRUE(base::WriteFile(config, R"({"allowed_cidrs": ["10.0.0.0/8"]})"));
  ASSERT_TRUE(base::SetPosixFilePermissions(config, 0666));

  base::test::TestFuture<mojom::ImportStatus, uint32_t, bool> future;
  handler_->ImportFromFile(future.GetCallback());
  EXPECT_EQ(mojom::ImportStatus::kInsecureFile, future.Get<0>());
  EXPECT_EQ(std::vector<std::string>{kDefaultAllowedCidr},
            GetAllowedCidrs(prefs_));
}
#endif  // BUILDFLAG(IS_POSIX)

}  // namespace ko_machines
