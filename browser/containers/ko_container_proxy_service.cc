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
#include "mojo/public/cpp/bindings/callback_helpers.h"
#include "mojo/public/cpp/bindings/pending_receiver.h"
#include "mojo/public/cpp/bindings/pending_remote.h"
#include "mojo/public/cpp/bindings/receiver.h"
#include "mojo/public/cpp/bindings/remote.h"
#include "services/network/public/mojom/network_context.mojom.h"
#include "services/network/public/mojom/proxy_config_with_annotation.mojom.h"
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

// Sits between the profile's ProxyConfigMonitor and one container network
// context. While the container is proxied, profile updates are only recorded;
// otherwise they pass through unchanged.
class KoContainerProxyService::ContextRelay
    : public network::mojom::ProxyConfigClient {
 public:
  ContextRelay(
      base::FilePath relative_partition_path,
      mojo::PendingReceiver<network::mojom::ProxyConfigClient> from_profile,
      std::optional<net::ProxyConfigWithAnnotation> profile_config,
      mojo::PendingRemote<network::mojom::ProxyConfigClient> to_context)
      : relative_partition_path_(std::move(relative_partition_path)),
        profile_config_(std::move(profile_config)),
        to_context_(std::move(to_context)) {
    if (from_profile.is_valid()) {
      from_profile_.Bind(std::move(from_profile));
    }
  }

  ContextRelay(const ContextRelay&) = delete;
  ContextRelay& operator=(const ContextRelay&) = delete;
  ~ContextRelay() override = default;

  const base::FilePath& relative_partition_path() const {
    return relative_partition_path_;
  }
  const std::optional<net::ProxyConfigWithAnnotation>& profile_config() const {
    return profile_config_;
  }
  // Container whose proxy this context uses, or empty to follow the profile.
  const std::string& proxied_container_id() const {
    return proxied_container_id_;
  }
  void set_proxied_container_id(std::string id) {
    proxied_container_id_ = std::move(id);
  }

  void SetDisconnectHandler(base::OnceClosure handler) {
    to_context_.set_disconnect_handler(std::move(handler));
  }

  void Send(const net::ProxyConfigWithAnnotation& config) {
    to_context_->OnProxyConfigUpdated(config);
  }

  // network::mojom::ProxyConfigClient:
  void OnProxyConfigUpdated(
      const net::ProxyConfigWithAnnotation& config) override {
    profile_config_ = config;
    if (proxied_container_id_.empty()) {
      Send(config);
    }
  }
  void FlushProxyConfig(FlushProxyConfigCallback callback) override {
    to_context_->FlushProxyConfig(
        mojo::WrapCallbackWithDefaultInvokeIfNotRun(std::move(callback)));
  }

 private:
  const base::FilePath relative_partition_path_;
  std::optional<net::ProxyConfigWithAnnotation> profile_config_;
  std::string proxied_container_id_;
  mojo::Receiver<network::mojom::ProxyConfigClient> from_profile_{this};
  mojo::Remote<network::mojom::ProxyConfigClient> to_context_;
};

KoContainerProxyService::KoContainerProxyService(
    PrefService* prefs,
    os_crypt_async::OSCryptAsync* os_crypt_async,
    ContainerExistsCallback container_exists,
    ForEachContainerTabCallback for_each_container_tab)
    : prefs_(prefs),
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
  // No OSCrypt means SOCKS credentials stay pending: those contexts never get
  // a config and their requests never leave (fail closed).
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

// static
bool KoContainerProxyService::IsContainerPartitionPath(
    const base::FilePath& relative_partition_path) {
  return GetRelativePartitionPath("any").DirName().IsParent(
      relative_partition_path);
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
  if (!IsContainerPartitionPath(relative_partition_path)) {
    return;
  }

  // Take over what the profile-wide monitor handed this context, so profile,
  // policy, extension or system proxy changes go through the relay.
  mojo::PendingReceiver<network::mojom::ProxyConfigClient> from_profile =
      std::move(params->proxy_config_client_receiver);
  std::optional<net::ProxyConfigWithAnnotation> profile_config =
      std::move(params->initial_proxy_config);
  params->proxy_config_client_receiver.reset();
  params->initial_proxy_config.reset();
  if (!from_profile.is_valid() && !profile_config) {
    // What the network service would use without any proxy params.
    profile_config = net::ProxyConfigWithAnnotation::CreateDirect();
  }

  mojo::PendingRemote<network::mojom::ProxyConfigClient> to_context;
  params->proxy_config_client_receiver =
      to_context.InitWithNewPipeAndPassReceiver();
  auto relay = std::make_unique<ContextRelay>(
      relative_partition_path, std::move(from_profile),
      std::move(profile_config), std::move(to_context));
  relay->SetDisconnectHandler(
      base::BindOnce(&KoContainerProxyService::OnRelayDisconnected,
                     weak_factory_.GetWeakPtr(), relay.get()));

  const std::string container_id =
      FindProxiedContainer(relative_partition_path);
  if (container_id.empty()) {
    // Pending profile config: the network service waits for the relay.
    params->initial_proxy_config = relay->profile_config();
  } else {
    bool pending = false;
    // Pending credentials: the network service holds requests until
    // UpdateRelay() sends the config.
    params->initial_proxy_config = GetConfig(container_id, &pending);
    relay->set_proxied_container_id(container_id);
  }
  relays_.push_back(std::move(relay));
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

void KoContainerProxyService::SetProxy(
    const std::string& id,
    std::optional<ParsedContainerProxy> proxy,
    bool keep_credentials,
    SetProxyCallback callback) {
  if (!proxy) {
    RemoveStoredContainerProxy(*prefs_, id);
    std::move(callback).Run(true);
    return;
  }

  if (proxy->credentials && !encryptor_) {
    // Reports failure if dropped (e.g. at shutdown) rather than never replying.
    pending_until_encryptor_.push_back(base::BindOnce(
        &KoContainerProxyService::SetProxy, weak_factory_.GetWeakPtr(), id,
        std::move(proxy), keep_credentials,
        mojo::WrapCallbackWithDefaultInvokeIfNotRun(std::move(callback),
                                                    false)));
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
  relays_.clear();
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

std::string KoContainerProxyService::FindProxiedContainer(
    const base::FilePath& relative_partition_path) const {
  for (auto& id : GetContainerIdsWithStoredProxy(*prefs_)) {
    if (GetRelativePartitionPath(id) == relative_partition_path) {
      return std::move(id);
    }
  }
  return std::string();
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
  for (auto& relay : relays_) {
    UpdateRelay(*relay);
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
  for (auto& relay : relays_) {
    UpdateRelay(*relay);
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

void KoContainerProxyService::UpdateRelay(ContextRelay& relay) {
  const std::string container_id =
      FindProxiedContainer(relay.relative_partition_path());
  if (!container_id.empty()) {
    bool pending = false;
    std::optional<net::ProxyConfigWithAnnotation> config =
        GetConfig(container_id, &pending);
    if (pending) {
      // Already waiting on this proxy: keep holding requests. Otherwise stop
      // the current (profile) config until the credentials can be decrypted.
      if (relay.proxied_container_id() != container_id) {
        relay.set_proxied_container_id(container_id);
        relay.Send(CreateFailClosedContainerProxyConfig());
      }
      return;
    }
    relay.set_proxied_container_id(container_id);
    relay.Send(*config);
    return;
  }

  const std::string previous = relay.proxied_container_id();
  if (previous.empty()) {
    return;
  }
  // The proxy was removed while the context is live. A deleted container's
  // tabs stay closed; otherwise follow the profile like any other container.
  if (!container_exists_ || !container_exists_.Run(previous)) {
    relay.Send(CreateFailClosedContainerProxyConfig());
    return;
  }
  relay.set_proxied_container_id(std::string());
  // With the profile's config still pending, the container proxy stays in
  // place until the relay passes the profile's first config on.
  if (relay.profile_config()) {
    relay.Send(*relay.profile_config());
  }
}

void KoContainerProxyService::OnRelayDisconnected(ContextRelay* relay) {
  std::erase_if(relays_, [relay](const std::unique_ptr<ContextRelay>& r) {
    return r.get() == relay;
  });
}

}  // namespace containers
