// Copyright (c) 2025 The Brave Authors. All rights reserved.
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this file,
// You can obtain one at https://mozilla.org/MPL/2.0/.

#ifndef BRAVE_COMPONENTS_CONTAINERS_CORE_BROWSER_CONTAINERS_SETTINGS_HANDLER_H_
#define BRAVE_COMPONENTS_CONTAINERS_CORE_BROWSER_CONTAINERS_SETTINGS_HANDLER_H_

#include <optional>
#include <string>
#include <vector>

#include "base/functional/callback.h"
#include "base/memory/raw_ptr.h"
#include "base/memory/weak_ptr.h"
#include "brave/components/containers/core/mojom/containers.mojom.h"
#include "brave/components/ko/buildflags/buildflags.h"
#include "components/prefs/pref_change_registrar.h"
#include "mojo/public/cpp/bindings/remote.h"

#if BUILDFLAG(IS_KO)
#include "brave/components/containers/core/browser/container_proxy.h"
#endif

namespace containers {

// Handles container management operations from the settings UI. This class
// implements the mojom::ContainersSettingsHandler interface to process requests
// from settings UI and manages container data persistence through prefs.
class ContainersSettingsHandler : public mojom::ContainersSettingsHandler {
 public:
#if BUILDFLAG(IS_KO)
  // K-O: stores per-container proxies outside the synced containers list.
  class ProxyDelegate {
   public:
    using SetProxyCallback = base::OnceCallback<void(bool success)>;

    virtual ~ProxyDelegate() = default;

    // Proxy as shown to the UI (never with credentials), or null.
    virtual mojom::ContainerProxyPtr GetProxyForUI(const std::string& id) = 0;

    // Stores `proxy`; nullopt removes it. With `keep_credentials` and no
    // credentials in `proxy`, the stored ones are kept if the server is
    // unchanged. `callback` gets false if nothing could be stored.
    virtual void SetProxy(const std::string& id,
                          std::optional<ParsedContainerProxy> proxy,
                          bool keep_credentials,
                          SetProxyCallback callback) = 0;
  };

  // `proxy_delegate` must outlive this handler.
  ContainersSettingsHandler(PrefService* prefs, ProxyDelegate* proxy_delegate);
#endif  // BUILDFLAG(IS_KO)

  explicit ContainersSettingsHandler(PrefService* prefs);
  ~ContainersSettingsHandler() override;

  ContainersSettingsHandler(const ContainersSettingsHandler&) = delete;
  ContainersSettingsHandler& operator=(const ContainersSettingsHandler&) =
      delete;

  // mojom::SettingsPageHandler:
  //
  // Establishes a connection with the UI for browser -> UI notifications.
  void BindUI(mojo::PendingRemote<mojom::ContainersSettingsUI> ui) override;
  // Retrieves the current list of containers from preferences.
  void GetContainers(GetContainersCallback callback) override;
  // Creates a new container or updates an existing one.
  void AddContainer(mojom::ContainerPtr container,
                    AddContainerCallback callback) override;
  // Updates an existing container.
  void UpdateContainer(mojom::ContainerPtr container,
                       UpdateContainerCallback callback) override;
  // Removes a container and all its associated data. Returns async response
  // after data cleanup is complete.
  void RemoveContainer(const std::string& id,
                       RemoveContainerCallback callback) override;
  // Reorders containers to match the order of `ordered_ids`.
  void ReorderContainers(const std::vector<std::string>& ordered_ids,
                         ReorderContainersCallback callback) override;

  // Returns an error if the given container properties are invalid.
  static std::optional<mojom::ContainerOperationError>
  ValidateEditableContainerProperties(const mojom::ContainerPtr& container);

 private:
  using OperationCallback =
      base::OnceCallback<void(std::optional<mojom::ContainerOperationError>)>;

  // Called when the containers list in preferences changes.
  void OnContainersChanged();

  // Returns the containers to send to the UI.
  std::vector<mojom::ContainerPtr> GetContainersForUI() const;

  // Appends a validated container with a set id.
  void AddContainerToPrefs(mojom::ContainerPtr container);

  // Replaces a validated container. Returns kNotFound if it is gone.
  std::optional<mojom::ContainerOperationError> UpdateContainerInPrefs(
      mojom::ContainerPtr container);

#if BUILDFLAG(IS_KO)
  void OnProxyStored(mojom::ContainerPtr container,
                     bool is_new,
                     OperationCallback callback,
                     bool success);

  raw_ptr<ProxyDelegate> proxy_delegate_ = nullptr;
#endif  // BUILDFLAG(IS_KO)

  // Interface to communicate with the settings page in the renderer.
  mojo::Remote<mojom::ContainersSettingsUI> ui_;

  // Profile preferences service for container data persistence.
  raw_ptr<PrefService> prefs_ = nullptr;

  // Watches for changes to container-related preferences.
  PrefChangeRegistrar pref_change_registrar_;

  base::WeakPtrFactory<ContainersSettingsHandler> weak_ptr_factory_{this};
};

}  // namespace containers

#endif  // BRAVE_COMPONENTS_CONTAINERS_CORE_BROWSER_CONTAINERS_SETTINGS_HANDLER_H_
