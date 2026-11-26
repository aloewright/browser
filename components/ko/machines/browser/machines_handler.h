// Copyright (c) 2026 The Brave Authors. All rights reserved.
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this file,
// You can obtain one at https://mozilla.org/MPL/2.0/.

#ifndef BRAVE_COMPONENTS_KO_MACHINES_BROWSER_MACHINES_HANDLER_H_
#define BRAVE_COMPONENTS_KO_MACHINES_BROWSER_MACHINES_HANDLER_H_

#include <cstddef>
#include <optional>
#include <string>
#include <utility>

#include "base/files/file_path.h"
#include "base/memory/raw_ptr.h"
#include "base/memory/raw_ref.h"
#include "base/memory/scoped_refptr.h"
#include "base/memory/weak_ptr.h"
#include "brave/components/ko/machines/browser/credential_store.h"
#include "brave/components/ko/machines/common/ko_machines.mojom.h"
#include "brave/components/ko/machines/core/machines_config.h"
#include "components/prefs/pref_change_registrar.h"
#include "mojo/public/cpp/bindings/pending_receiver.h"
#include "mojo/public/cpp/bindings/pending_remote.h"
#include "mojo/public/cpp/bindings/receiver.h"
#include "mojo/public/cpp/bindings/remote.h"

class PrefService;

namespace os_crypt_async {
class Encryptor;
class OSCryptAsync;
}  // namespace os_crypt_async

namespace ko_machines {

// mojom::MachinesHandler: the machine list, credentials and import. On first
// use it seeds the list from machines.json in the K-O data dir.
class MachinesHandler : public mojom::MachinesHandler {
 public:
  MachinesHandler(mojo::PendingReceiver<mojom::MachinesHandler> receiver,
                  PrefService& prefs,
                  os_crypt_async::OSCryptAsync* os_crypt_async,
                  base::FilePath data_dir);
  MachinesHandler(const MachinesHandler&) = delete;
  MachinesHandler& operator=(const MachinesHandler&) = delete;
  ~MachinesHandler() override;

  // mojom::MachinesHandler:
  void SetPage(mojo::PendingRemote<mojom::MachinesPage> page) override;
  void GetMachines(GetMachinesCallback callback) override;
  void AddMachine(mojom::MachineInputPtr input,
                  AddMachineCallback callback) override;
  void UpdateMachine(const std::string& id,
                     mojom::MachineInputPtr input,
                     UpdateMachineCallback callback) override;
  void RemoveMachine(const std::string& id,
                     RemoveMachineCallback callback) override;
  void SetCredentials(const std::string& id,
                      const std::optional<std::string>& username,
                      const std::string& password,
                      SetCredentialsCallback callback) override;
  void ClearCredentials(const std::string& id,
                        ClearCredentialsCallback callback) override;
  void GetCredentials(const std::string& id,
                      GetCredentialsCallback callback) override;
  void ImportFromFile(ImportFromFileCallback callback) override;

 private:
  void MaybeSeed();
  void OnSeedRead(ReadConfigResult result);
  void OnImportRead(ImportFromFileCallback callback, ReadConfigResult result);
  // Applies a read config; returns {added, allowlist_updated}.
  std::pair<size_t, bool> ApplyConfig(const MachinesConfig& config);

  void OnEncryptorForSet(std::string id,
                         MachineCredentials credentials,
                         SetCredentialsCallback callback,
                         scoped_refptr<os_crypt_async::Encryptor> encryptor);
  void OnEncryptorForGet(std::string id,
                         GetCredentialsCallback callback,
                         scoped_refptr<os_crypt_async::Encryptor> encryptor);

  void OnPrefsChanged();

  const raw_ref<PrefService> prefs_;
  const raw_ptr<os_crypt_async::OSCryptAsync> os_crypt_async_;
  const base::FilePath data_dir_;
  PrefChangeRegistrar pref_change_registrar_;
  mojo::Receiver<mojom::MachinesHandler> receiver_;
  mojo::Remote<mojom::MachinesPage> page_;
  base::WeakPtrFactory<MachinesHandler> weak_factory_{this};
};

}  // namespace ko_machines

#endif  // BRAVE_COMPONENTS_KO_MACHINES_BROWSER_MACHINES_HANDLER_H_
