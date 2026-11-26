// Copyright (c) 2026 The Brave Authors. All rights reserved.
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this file,
// You can obtain one at https://mozilla.org/MPL/2.0/.

#include "brave/components/ko/machines/browser/machines_handler.h"

#include <algorithm>
#include <vector>

#include "base/functional/bind.h"
#include "base/location.h"
#include "base/task/thread_pool.h"
#include "brave/components/ko/machines/core/address_allowlist.h"
#include "brave/components/ko/machines/core/machine.h"
#include "brave/components/ko/machines/core/pref_names.h"
#include "brave/components/ko/machines/core/prefs.h"
#include "components/os_crypt/async/browser/os_crypt_async.h"
#include "components/os_crypt/async/common/encryptor.h"
#include "components/prefs/pref_service.h"

namespace ko_machines {

namespace {

MachineKind FromMojom(mojom::MachineKind kind) {
  switch (kind) {
    case mojom::MachineKind::kScreenSharing:
      return MachineKind::kScreenSharing;
    case mojom::MachineKind::kVnc:
      return MachineKind::kVnc;
  }
}

mojom::MachineKind ToMojom(MachineKind kind) {
  switch (kind) {
    case MachineKind::kScreenSharing:
      return mojom::MachineKind::kScreenSharing;
    case MachineKind::kVnc:
      return mojom::MachineKind::kVnc;
  }
}

mojom::ImportStatus ToImportStatus(ReadConfigStatus status) {
  switch (status) {
    case ReadConfigStatus::kOk:
      return mojom::ImportStatus::kImported;
    case ReadConfigStatus::kNotFound:
      return mojom::ImportStatus::kNotFound;
    case ReadConfigStatus::kInsecure:
      return mojom::ImportStatus::kInsecureFile;
    case ReadConfigStatus::kInvalid:
      return mojom::ImportStatus::kInvalidFile;
  }
}

// Validates `input` into `machine`'s editable fields.
mojom::MachineError ApplyInput(const mojom::MachineInput& input,
                               Machine& machine) {
  std::optional<std::string> name = NormalizeMachineName(input.name);
  if (!name) {
    return mojom::MachineError::kInvalidName;
  }
  std::optional<std::string> host = CanonicalizeMachineHost(input.host);
  if (!host) {
    return mojom::MachineError::kInvalidHost;
  }
  std::optional<uint16_t> port = ValidateMachinePort(input.port);
  if (!port) {
    return mojom::MachineError::kInvalidPort;
  }
  machine.name = std::move(*name);
  machine.host = std::move(*host);
  machine.port = *port;
  machine.kind = FromMojom(input.kind);
  return mojom::MachineError::kNone;
}

bool HasSameEndpoint(const std::vector<Machine>& machines,
                     const Machine& machine) {
  return std::ranges::any_of(machines, [&machine](const Machine& other) {
    return other.id != machine.id && other.host == machine.host &&
           other.port == machine.port;
  });
}

}  // namespace

MachinesHandler::MachinesHandler(
    mojo::PendingReceiver<mojom::MachinesHandler> receiver,
    PrefService& prefs,
    os_crypt_async::OSCryptAsync* os_crypt_async,
    base::FilePath data_dir)
    : prefs_(prefs),
      os_crypt_async_(os_crypt_async),
      data_dir_(std::move(data_dir)),
      receiver_(this, std::move(receiver)) {
  pref_change_registrar_.Init(&prefs);
  const auto on_change = base::BindRepeating(&MachinesHandler::OnPrefsChanged,
                                             base::Unretained(this));
  pref_change_registrar_.Add(prefs::kMachines, on_change);
  pref_change_registrar_.Add(prefs::kAllowedCidrs, on_change);
  pref_change_registrar_.Add(prefs::kCredentials, on_change);
  MaybeSeed();
}

MachinesHandler::~MachinesHandler() = default;

void MachinesHandler::SetPage(mojo::PendingRemote<mojom::MachinesPage> page) {
  page_.reset();
  page_.Bind(std::move(page));
}

void MachinesHandler::GetMachines(GetMachinesCallback callback) {
  std::vector<mojom::MachinePtr> machines;
  for (const Machine& machine : ko_machines::GetMachines(*prefs_)) {
    machines.push_back(mojom::Machine::New(
        machine.id, machine.name, machine.host, machine.port,
        ToMojom(machine.kind), HasStoredCredentials(*prefs_, machine.id)));
  }
  std::vector<std::string> allowed;
  std::vector<std::string> rejected;
  for (const std::string& cidr : GetAllowedCidrs(*prefs_)) {
    AddressAllowlist::Block block;
    (ParseAllowedBlock(cidr, &block) ? allowed : rejected).push_back(cidr);
  }
  std::move(callback).Run(std::move(machines), std::move(allowed),
                          std::move(rejected));
}

void MachinesHandler::AddMachine(mojom::MachineInputPtr input,
                                 AddMachineCallback callback) {
  std::vector<Machine> machines = ko_machines::GetMachines(*prefs_);
  if (machines.size() >= kMaxMachines) {
    std::move(callback).Run(mojom::MachineError::kTooMany, std::nullopt);
    return;
  }
  Machine machine;
  machine.id = GenerateMachineId();
  const mojom::MachineError error = ApplyInput(*input, machine);
  if (error != mojom::MachineError::kNone) {
    std::move(callback).Run(error, std::nullopt);
    return;
  }
  if (HasSameEndpoint(machines, machine)) {
    std::move(callback).Run(mojom::MachineError::kDuplicate, std::nullopt);
    return;
  }
  const std::string id = machine.id;
  machines.push_back(std::move(machine));
  SetMachines(*prefs_, machines);
  std::move(callback).Run(mojom::MachineError::kNone, id);
}

void MachinesHandler::UpdateMachine(const std::string& id,
                                    mojom::MachineInputPtr input,
                                    UpdateMachineCallback callback) {
  std::vector<Machine> machines = ko_machines::GetMachines(*prefs_);
  auto it = std::ranges::find(machines, id, &Machine::id);
  if (it == machines.end()) {
    std::move(callback).Run(mojom::MachineError::kNotFound);
    return;
  }
  Machine updated = *it;
  const mojom::MachineError error = ApplyInput(*input, updated);
  if (error != mojom::MachineError::kNone) {
    std::move(callback).Run(error);
    return;
  }
  if (HasSameEndpoint(machines, updated)) {
    std::move(callback).Run(mojom::MachineError::kDuplicate);
    return;
  }
  // Saved credentials belong to the old host; never offer them to a new one.
  if (updated.host != it->host) {
    ClearStoredCredentials(*prefs_, id);
  }
  *it = std::move(updated);
  SetMachines(*prefs_, machines);
  std::move(callback).Run(mojom::MachineError::kNone);
}

void MachinesHandler::RemoveMachine(const std::string& id,
                                    RemoveMachineCallback callback) {
  std::vector<Machine> machines = ko_machines::GetMachines(*prefs_);
  const size_t removed = std::erase_if(
      machines, [&id](const Machine& machine) { return machine.id == id; });
  ClearStoredCredentials(*prefs_, id);
  if (removed) {
    SetMachines(*prefs_, machines);
  }
  std::move(callback).Run(removed > 0);
}

void MachinesHandler::SetCredentials(const std::string& id,
                                     const std::optional<std::string>& username,
                                     const std::string& password,
                                     SetCredentialsCallback callback) {
  if (!FindMachine(*prefs_, id)) {
    std::move(callback).Run(mojom::MachineError::kNotFound);
    return;
  }
  MachineCredentials credentials{username.value_or(std::string()), password};
  if (!AreValidCredentials(credentials)) {
    std::move(callback).Run(mojom::MachineError::kInvalidCredentials);
    return;
  }
  if (!os_crypt_async_) {
    std::move(callback).Run(mojom::MachineError::kEncryptionUnavailable);
    return;
  }
  os_crypt_async_->GetInstance(base::BindOnce(
      &MachinesHandler::OnEncryptorForSet, weak_factory_.GetWeakPtr(), id,
      std::move(credentials), std::move(callback)));
}

void MachinesHandler::OnEncryptorForSet(
    std::string id,
    MachineCredentials credentials,
    SetCredentialsCallback callback,
    scoped_refptr<os_crypt_async::Encryptor> encryptor) {
  // The machine may have been removed while OSCrypt was starting.
  if (!FindMachine(*prefs_, id)) {
    std::move(callback).Run(mojom::MachineError::kNotFound);
    return;
  }
  std::optional<std::string> ciphertext =
      encryptor ? EncryptCredentials(*encryptor, credentials) : std::nullopt;
  if (!ciphertext) {
    // Fail closed: never fall back to storing plaintext.
    std::move(callback).Run(mojom::MachineError::kEncryptionUnavailable);
    return;
  }
  StoreCredentials(*prefs_, id, std::move(*ciphertext));
  std::move(callback).Run(mojom::MachineError::kNone);
}

void MachinesHandler::ClearCredentials(const std::string& id,
                                       ClearCredentialsCallback callback) {
  ClearStoredCredentials(*prefs_, id);
  std::move(callback).Run();
}

void MachinesHandler::GetCredentials(const std::string& id,
                                     GetCredentialsCallback callback) {
  if (!FindMachine(*prefs_, id) || !HasStoredCredentials(*prefs_, id) ||
      !os_crypt_async_) {
    std::move(callback).Run(std::nullopt, std::nullopt);
    return;
  }
  os_crypt_async_->GetInstance(
      base::BindOnce(&MachinesHandler::OnEncryptorForGet,
                     weak_factory_.GetWeakPtr(), id, std::move(callback)));
}

void MachinesHandler::OnEncryptorForGet(
    std::string id,
    GetCredentialsCallback callback,
    scoped_refptr<os_crypt_async::Encryptor> encryptor) {
  std::optional<std::string> stored = GetStoredCredentials(*prefs_, id);
  std::optional<MachineCredentials> credentials =
      encryptor && stored ? DecryptCredentials(*encryptor, *stored)
                          : std::nullopt;
  if (!credentials) {
    std::move(callback).Run(std::nullopt, std::nullopt);
    return;
  }
  std::optional<std::string> username;
  if (!credentials->username.empty()) {
    username = std::move(credentials->username);
  }
  std::move(callback).Run(std::move(username),
                          std::move(credentials->password));
}

void MachinesHandler::ImportFromFile(ImportFromFileCallback callback) {
  base::ThreadPool::PostTaskAndReplyWithResult(
      FROM_HERE,
      {base::MayBlock(), base::TaskPriority::USER_VISIBLE,
       base::TaskShutdownBehavior::SKIP_ON_SHUTDOWN},
      base::BindOnce(&ReadMachinesConfig, data_dir_),
      base::BindOnce(&MachinesHandler::OnImportRead, weak_factory_.GetWeakPtr(),
                     std::move(callback)));
}

void MachinesHandler::OnImportRead(ImportFromFileCallback callback,
                                   ReadConfigResult result) {
  if (result.status != ReadConfigStatus::kOk) {
    std::move(callback).Run(ToImportStatus(result.status), 0, false);
    return;
  }
  prefs_->SetBoolean(prefs::kSeeded, true);
  const auto [added, allowlist_updated] = ApplyConfig(result.config);
  std::move(callback).Run(mojom::ImportStatus::kImported,
                          static_cast<uint32_t>(added), allowlist_updated);
}

void MachinesHandler::MaybeSeed() {
  if (prefs_->GetBoolean(prefs::kSeeded)) {
    return;
  }
  base::ThreadPool::PostTaskAndReplyWithResult(
      FROM_HERE,
      {base::MayBlock(), base::TaskPriority::USER_VISIBLE,
       base::TaskShutdownBehavior::SKIP_ON_SHUTDOWN},
      base::BindOnce(&ReadMachinesConfig, data_dir_),
      base::BindOnce(&MachinesHandler::OnSeedRead, weak_factory_.GetWeakPtr()));
}

void MachinesHandler::OnSeedRead(ReadConfigResult result) {
  // Another page may have seeded meanwhile.
  if (prefs_->GetBoolean(prefs::kSeeded)) {
    return;
  }
  prefs_->SetBoolean(prefs::kSeeded, true);
  if (result.status == ReadConfigStatus::kOk) {
    ApplyConfig(result.config);
  }
}

std::pair<size_t, bool> MachinesHandler::ApplyConfig(
    const MachinesConfig& config) {
  std::vector<Machine> machines = ko_machines::GetMachines(*prefs_);
  const size_t added = MergeMachines(machines, config.machines);
  if (added) {
    SetMachines(*prefs_, machines);
  }
  bool allowlist_updated = false;
  if (config.allowed_cidrs &&
      *config.allowed_cidrs != GetAllowedCidrs(*prefs_)) {
    SetAllowedCidrs(*prefs_, *config.allowed_cidrs);
    allowlist_updated = true;
  }
  return {added, allowlist_updated};
}

void MachinesHandler::OnPrefsChanged() {
  if (page_.is_bound()) {
    page_->OnMachinesChanged();
  }
}

}  // namespace ko_machines
