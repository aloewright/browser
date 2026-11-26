// Copyright (c) 2026 The Brave Authors. All rights reserved.
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this file,
// You can obtain one at https://mozilla.org/MPL/2.0/.

#ifndef BRAVE_BROWSER_CONTAINERS_KO_CONTAINER_PROXY_SERVICE_H_
#define BRAVE_BROWSER_CONTAINERS_KO_CONTAINER_PROXY_SERVICE_H_

#include <map>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <vector>

#include "base/files/file_path.h"
#include "base/functional/callback.h"
#include "base/memory/raw_ptr.h"
#include "base/memory/scoped_refptr.h"
#include "base/memory/weak_ptr.h"
#include "brave/components/containers/core/browser/container_proxy.h"
#include "brave/components/containers/core/browser/containers_settings_handler.h"
#include "components/keyed_service/core/keyed_service.h"
#include "components/prefs/pref_change_registrar.h"
#include "mojo/public/cpp/bindings/remote_set.h"
#include "net/base/auth.h"
#include "net/proxy_resolution/proxy_config_with_annotation.h"
#include "services/network/public/mojom/network_context.mojom-forward.h"
#include "services/network/public/mojom/proxy_config_with_annotation.mojom.h"

class PrefService;

namespace content {
class WebContents;
}  // namespace content

namespace os_crypt_async {
class Encryptor;
class OSCryptAsync;
}  // namespace os_crypt_async

namespace url {
class SchemeHostPort;
}  // namespace url

namespace containers {

// K-O: routes each proxied container's network context through its proxy
// (no DIRECT fallback) and stores proxies for the settings UI. Credentials
// are OSCrypt-encrypted at rest and decrypted only in the browser process.
class KoContainerProxyService
    : public KeyedService,
      public ContainersSettingsHandler::ProxyDelegate {
 public:
  // Returns whether the profile has a proxy of its own (settings, policy,
  // extension or system); nullopt while unknown.
  using ProfileProxyConfiguredCallback =
      base::RepeatingCallback<std::optional<bool>()>;
  // Returns whether `id` is still a known container.
  using ContainerExistsCallback =
      base::RepeatingCallback<bool(std::string_view id)>;
  // Visits tabs whose container is `id`.
  using ForEachContainerTabCallback = base::RepeatingCallback<void(
      std::string_view id,
      base::RepeatingCallback<void(content::WebContents*)>)>;

  KoContainerProxyService(PrefService* prefs,
                          os_crypt_async::OSCryptAsync* os_crypt_async,
                          ProfileProxyConfiguredCallback profile_has_proxy,
                          ContainerExistsCallback container_exists,
                          ForEachContainerTabCallback for_each_container_tab);
  ~KoContainerProxyService() override;

  KoContainerProxyService(const KoContainerProxyService&) = delete;
  KoContainerProxyService& operator=(const KoContainerProxyService&) = delete;

  // Mirrors content's layout: Storage/ext/containers/<hash of id>.
  static base::FilePath GetRelativePartitionPath(std::string_view container_id);

  // Forces WebRTC to stay behind the proxy for `web_contents` if its
  // container is proxied.
  void ApplyWebRtcPolicy(content::WebContents* web_contents) const;

  // For proxied containers, replaces the profile-wide proxy client with a
  // container-specific one. No-op for other partitions.
  void ConfigureNetworkContextParams(
      const base::FilePath& relative_partition_path,
      network::mojom::NetworkContextParams* params);

  bool HasProxy(std::string_view container_id) const;

  // Credentials for a 407 from the HTTP(S) proxy of `container_id`, only if
  // `challenger` is exactly that proxy.
  std::optional<net::AuthCredentials> GetHttpProxyCredentials(
      std::string_view container_id,
      const url::SchemeHostPort& challenger) const;

  // ContainersSettingsHandler::ProxyDelegate:
  mojom::ContainerProxyPtr GetProxyForUI(const std::string& id) override;
  void SetProxy(const std::string& id,
                std::optional<ParsedContainerProxy> proxy,
                bool keep_credentials,
                SetProxyCallback callback) override;

  // KeyedService:
  void Shutdown() override;

  // Drops entries for containers that no longer exist.
  void PruneStaleEntries();

  bool encryptor_ready_for_testing() const { return !!encryptor_; }

 private:
  // nullopt with `*pending` set means credentials are still waiting for the
  // encryptor; nullopt otherwise means the container has no proxy.
  std::optional<net::ProxyConfigWithAnnotation> GetConfig(
      std::string_view container_id,
      bool* pending) const;
  std::optional<ContainerProxyCredentials> DecryptCredentials(
      std::string_view encrypted) const;

  void OnEncryptorReady(scoped_refptr<os_crypt_async::Encryptor> encryptor);
  void OnProxiesPrefChanged();
  void PushConfig(const std::string& container_id);

  raw_ptr<PrefService> prefs_;
  ProfileProxyConfiguredCallback profile_has_proxy_;
  ContainerExistsCallback container_exists_;
  ForEachContainerTabCallback for_each_container_tab_;
  scoped_refptr<os_crypt_async::Encryptor> encryptor_;
  // Work that needs the encryptor, run once it is ready.
  std::vector<base::OnceClosure> pending_until_encryptor_;
  // Live network contexts of proxied containers.
  std::map<std::string, mojo::RemoteSet<network::mojom::ProxyConfigClient>,
           std::less<>>
      clients_;
  // Proxied container ids as of the last pref change.
  std::set<std::string, std::less<>> proxied_ids_;
  PrefChangeRegistrar pref_change_registrar_;
  base::WeakPtrFactory<KoContainerProxyService> weak_factory_{this};
};

}  // namespace containers

#endif  // BRAVE_BROWSER_CONTAINERS_KO_CONTAINER_PROXY_SERVICE_H_
