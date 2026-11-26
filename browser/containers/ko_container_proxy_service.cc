// Copyright (c) 2026 The Brave Authors. All rights reserved.
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this file,
// You can obtain one at https://mozilla.org/MPL/2.0/.

#include "brave/browser/containers/ko_container_proxy_service.h"

#include <utility>

#include "base/base64.h"
#include "base/check.h"
#include "base/containers/span.h"
#include "base/functional/bind.h"
#include "base/logging.h"
#include "base/strings/string_number_conversions.h"
#include "base/strings/utf_string_conversions.h"
#include "brave/components/containers/content/browser/storage_partition_utils.h"
#include "components/os_crypt/async/browser/os_crypt_async.h"
#include "components/os_crypt/async/common/encryptor.h"
#include "components/prefs/pref_service.h"
#include "content/public/browser/web_contents.h"
#include "crypto/hash.h"
#include "mojo/public/cpp/bindings/pending_remote.h"
#include "services/network/public/mojom/network_context.mojom.h"
#include "third_party/blink/public/common/renderer_preferences/renderer_preferences.h"
#include "third_party/blink/public/mojom/peerconnection/webrtc_ip_handling_policy.mojom.h"
#include "url/scheme_host_port.h"
#include "url/url_constants.h"

namespace containers {

namespace {

// Matches content::StoragePartitionImplMap.
constexpr size_t kPartitionNameHashBytes = 6;

std::string_view StripBrackets(std::string_view host) {
  if (host.size() > 2 && host.front() == '[' && host.back() == ']') {
    return host.substr(1, host.size() - 2);
  }
  return host;
}

bool IsHttpScheme(ContainerProxyScheme scheme) {
  return scheme == ContainerProxyScheme::kHttp ||
         scheme == ContainerProxyScheme::kHttps;
}

// Credentials stay bound to the server they were entered for.
bool IsSameServer(std::string_view stored, const ContainerProxyServer& server) {
  auto parsed = ParseContainerProxyServer(stored);
  return parsed.has_value() && *parsed == server;
}

}  // namespace

KoContainerProxyService::KoContainerProxyService(
    PrefService* prefs,
    os_crypt_async::OSCryptAsync* os_crypt_async,
    ProfileProxyConfiguredCallback profile_has_proxy,
    ContainerExistsCallback container_exists,
    ForEachContainerTabCallback for_each_container_tab)
    : prefs_(prefs),
      profile_has_proxy_(std::move(profile_has_proxy)),
      container_exists_(std::move(container_exists)),
      for_each_container_tab_(std::move(for_each_container_tab)) {
  CHECK(prefs_);
  for (auto& id : GetContainerIdsWithStoredProxy(*prefs_)) {
    proxied_ids_.insert(std::move(id));
  }
  pref_change_registrar_.Init(prefs_);
  pref_change_registrar_.Add(
      kKoContainerProxiesPref,
      base::BindRepeating(&KoContainerProxyService::OnProxiesPrefChanged,
                          base::Unretained(this)));
  // No OSCrypt means credentials stay pending: proxied contexts never get a
  // config and their requests never leave (fail closed).
  if (os_crypt_async) {
    os_crypt_async->GetInstance(
        base::BindOnce(&KoContainerProxyService::OnEncryptorReady,
                       weak_factory_.GetWeakPtr()));
  }
}

KoContainerProxyService::~KoContainerProxyService() = default;

// static
base::FilePath KoContainerProxyService::GetRelativePartitionPath(
    std::string_view container_id) {
  const auto hash = crypto::hash::Sha256(container_id);
  return base::FilePath(FILE_PATH_LITERAL("Storage"))
      .Append(FILE_PATH_LITERAL("ext"))
      .Append(base::FilePath::FromUTF8Unsafe(kContainersStoragePartitionDomain))
      .AppendASCII(
          base::HexEncode(base::span(hash).first<kPartitionNameHashBytes>()));
}

void KoContainerProxyService::ApplyWebRtcPolicy(
    content::WebContents* web_contents) const {
  if (!web_contents) {
    return;
  }
  const std::string id = GetContainerIdForWebContents(web_contents);
  if (id.empty() || !HasProxy(id)) {
    return;
  }
  blink::RendererPreferences* prefs = web_contents->GetMutableRendererPrefs();
  prefs->webrtc_ip_handling_policy =
      blink::mojom::WebRtcIpHandlingPolicy::kDisableNonProxiedUdp;
  prefs->webrtc_ip_handling_urls.clear();
  prefs->webrtc_local_ips_allowed_urls.clear();
  web_contents->SyncRendererPrefs();
}

void KoContainerProxyService::ConfigureNetworkContextParams(
    const base::FilePath& relative_partition_path,
    network::mojom::NetworkContextParams* params) {
  CHECK(params);
  std::string container_id;
  for (const auto& id : GetContainerIdsWithStoredProxy(*prefs_)) {
    if (GetRelativePartitionPath(id) == relative_partition_path) {
      container_id = id;
      break;
    }
  }
  if (container_id.empty()) {
    return;
  }

  // Detach from the profile-wide monitor so profile, policy, extension or
  // system proxy changes cannot redirect this container, and drop the
  // initial profile config so nothing goes out before ours is set.
  params->proxy_config_client_receiver.reset();
  params->proxy_config_poller_client.reset();
  params->initial_proxy_config.reset();

  mojo::PendingRemote<network::mojom::ProxyConfigClient> client;
  params->proxy_config_client_receiver = client.InitWithNewPipeAndPassReceiver();
  clients_[container_id].Add(std::move(client));

  bool pending = false;
  if (auto config = GetConfig(container_id, &pending)) {
    params->initial_proxy_config = std::move(*config);
  }
  // While pending, the network service holds requests until PushConfig().
}

bool KoContainerProxyService::HasProxy(std::string_view container_id) const {
  return GetStoredContainerProxy(*prefs_, container_id).has_value();
}

std::optional<net::AuthCredentials>
KoContainerProxyService::GetHttpProxyCredentials(
    std::string_view container_id,
    const url::SchemeHostPort& challenger) const {
  auto stored = GetStoredContainerProxy(*prefs_, container_id);
  if (!stored || stored->encrypted_credentials.empty()) {
    return std::nullopt;
  }
  auto server = ParseContainerProxyServer(stored->server);
  if (!server.has_value() || !IsHttpScheme(server->scheme)) {
    return std::nullopt;
  }
  const std::string_view expected_scheme =
      server->scheme == ContainerProxyScheme::kHttps ? url::kHttpsScheme
                                                     : url::kHttpScheme;
  if (challenger.scheme() != expected_scheme ||
      StripBrackets(challenger.host()) != StripBrackets(server->host) ||
      challenger.port() != server->port) {
    return std::nullopt;
  }
  auto credentials = DecryptCredentials(stored->encrypted_credentials);
  if (!credentials) {
    return std::nullopt;
  }
  return net::AuthCredentials(base::UTF8ToUTF16(credentials->username),
                              base::UTF8ToUTF16(credentials->password));
}

mojom::ContainerProxyPtr KoContainerProxyService::GetProxyForUI(
    const std::string& id) {
  auto stored = GetStoredContainerProxy(*prefs_, id);
  if (!stored) {
    return nullptr;
  }
  // The stored server never contains credentials.
  return mojom::ContainerProxy::New(stored->server,
                                    !stored->encrypted_credentials.empty());
}

void KoContainerProxyService::SetProxy(const std::string& id,
                                       std::optional<ParsedContainerProxy> proxy,
                                       bool keep_credentials,
                                       SetProxyCallback callback) {
  if (!proxy) {
    RemoveStoredContainerProxy(*prefs_, id);
    std::move(callback).Run(true);
    return;
  }

  if (proxy->credentials && !encryptor_) {
    pending_until_encryptor_.push_back(base::BindOnce(
        &KoContainerProxyService::SetProxy, weak_factory_.GetWeakPtr(), id,
        std::move(proxy), keep_credentials, std::move(callback)));
    return;
  }

  StoredContainerProxy entry;
  entry.server = proxy->server.ToString();
  if (proxy->credentials) {
    auto ciphertext = encryptor_->EncryptString(
        SerializeContainerProxyCredentials(*proxy->credentials));
    if (!ciphertext) {
      // Never fall back to storing credentials in plain text.
      LOG(ERROR) << "Could not encrypt container proxy credentials";
      std::move(callback).Run(false);
      return;
    }
    entry.encrypted_credentials = base::Base64Encode(*ciphertext);
  } else if (keep_credentials) {
    auto existing = GetStoredContainerProxy(*prefs_, id);
    if (existing && IsSameServer(existing->server, proxy->server)) {
      entry.encrypted_credentials = existing->encrypted_credentials;
    }
  }
  SetStoredContainerProxy(*prefs_, id, entry);
  std::move(callback).Run(true);
}

void KoContainerProxyService::Shutdown() {
  weak_factory_.InvalidateWeakPtrs();
  pref_change_registrar_.RemoveAll();
  pending_until_encryptor_.clear();
  clients_.clear();
}

void KoContainerProxyService::PruneStaleEntries() {
  if (!container_exists_) {
    return;
  }
  for (const auto& id : GetContainerIdsWithStoredProxy(*prefs_)) {
    if (!container_exists_.Run(id)) {
      RemoveStoredContainerProxy(*prefs_, id);
    }
  }
}

std::optional<net::ProxyConfigWithAnnotation>
KoContainerProxyService::GetConfig(std::string_view container_id,
                                   bool* pending) const {
  *pending = false;
  auto stored = GetStoredContainerProxy(*prefs_, container_id);
  if (!stored) {
    return std::nullopt;
  }
  auto server = ParseContainerProxyServer(stored->server);
  if (!server.has_value()) {
    LOG(ERROR) << "Unusable container proxy entry: "
               << ContainerProxyErrorToString(server.error());
    return CreateFailClosedContainerProxyConfig();
  }
  // HTTP(S) proxy credentials are answered on the 407 challenge instead.
  if (stored->encrypted_credentials.empty() || IsHttpScheme(server->scheme)) {
    return CreateContainerProxyConfig(*server, nullptr);
  }
  if (!encryptor_) {
    *pending = true;
    return std::nullopt;
  }
  auto credentials = DecryptCredentials(stored->encrypted_credentials);
  // Without credentials an authenticating proxy rejects us: still no direct
  // connection.
  return CreateContainerProxyConfig(*server,
                                    credentials ? &*credentials : nullptr);
}

std::optional<ContainerProxyCredentials>
KoContainerProxyService::DecryptCredentials(std::string_view encrypted) const {
  if (!encryptor_) {
    return std::nullopt;
  }
  auto ciphertext = base::Base64Decode(encrypted);
  if (!ciphertext) {
    return std::nullopt;
  }
  auto plaintext = encryptor_->DecryptData(*ciphertext);
  if (!plaintext) {
    LOG(ERROR) << "Could not decrypt container proxy credentials";
    return std::nullopt;
  }
  return DeserializeContainerProxyCredentials(*plaintext);
}

void KoContainerProxyService::OnEncryptorReady(
    scoped_refptr<os_crypt_async::Encryptor> encryptor) {
  encryptor_ = std::move(encryptor);
  for (const auto& [id, unused] : clients_) {
    PushConfig(id);
  }
  auto pending = std::move(pending_until_encryptor_);
  for (auto& closure : pending) {
    std::move(closure).Run();
  }
}

void KoContainerProxyService::OnProxiesPrefChanged() {
  std::set<std::string, std::less<>> now;
  for (auto& id : GetContainerIdsWithStoredProxy(*prefs_)) {
    now.insert(std::move(id));
  }
  for (const auto& [id, unused] : clients_) {
    PushConfig(id);
  }
  // Newly proxied containers: tighten WebRTC in tabs that are already open.
  if (for_each_container_tab_) {
    for (const auto& id : now) {
      if (!proxied_ids_.contains(id)) {
        for_each_container_tab_.Run(
            id, base::BindRepeating(&KoContainerProxyService::ApplyWebRtcPolicy,
                                    weak_factory_.GetWeakPtr()));
      }
    }
  }
  proxied_ids_ = std::move(now);
}

void KoContainerProxyService::PushConfig(const std::string& container_id) {
  auto it = clients_.find(container_id);
  if (it == clients_.end() || it->second.empty()) {
    return;
  }

  bool pending = false;
  std::optional<net::ProxyConfigWithAnnotation> config =
      GetConfig(container_id, &pending);
  if (pending) {
    return;
  }
  if (!config) {
    // The proxy was removed while tabs use this context. Go direct only if
    // that matches what the profile would do; otherwise stay closed until the
    // context is recreated.
    const bool can_go_direct = container_exists_ &&
                               container_exists_.Run(container_id) &&
                               profile_has_proxy_ &&
                               profile_has_proxy_.Run() == false;
    config = can_go_direct ? net::ProxyConfigWithAnnotation::CreateDirect()
                           : CreateFailClosedContainerProxyConfig();
  }
  for (const auto& client : it->second) {
    client->OnProxyConfigUpdated(*config);
  }
}

}  // namespace containers
