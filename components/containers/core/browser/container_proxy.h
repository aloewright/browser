// Copyright (c) 2026 The Brave Authors. All rights reserved.
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this file,
// You can obtain one at https://mozilla.org/MPL/2.0/.

#ifndef BRAVE_COMPONENTS_CONTAINERS_CORE_BROWSER_CONTAINER_PROXY_H_
#define BRAVE_COMPONENTS_CONTAINERS_CORE_BROWSER_CONTAINER_PROXY_H_

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "base/types/expected.h"

class PrefService;

namespace net {
class ProxyConfigWithAnnotation;
}  // namespace net

namespace containers {

// K-O: optional proxy for a container's network context. Only the server
// (scheme, host, port) is stored in prefs as text; credentials are stored
// OSCrypt-encrypted and never leave the browser process towards the UI.

// Local-only (never synced) dictionary pref:
//   container id -> {"server": "<scheme>://<host>:<port>",
//                    "credentials": "<base64 OSCrypt ciphertext>"}
inline constexpr char kKoContainerProxiesPref[] = "ko.containers.proxies";

enum class ContainerProxyScheme {
  kHttp,
  kHttps,
  kSocks5,
  // Same as kSocks5 in Chromium: SOCKS5 always resolves names on the proxy.
  kSocks5h,
};

enum class ContainerProxyError {
  kEmpty,
  kTooLong,
  kInvalidUrl,
  kUnsupportedScheme,
  kInvalidPort,
  // Path, query or fragment.
  kUnexpectedComponents,
  kInvalidCredentials,
  // Credentials over a cleartext scheme to a publicly routable host.
  kInsecureCredentials,
};

// Scheme, host and port only, so it is safe to log and display.
struct ContainerProxyServer {
  ContainerProxyScheme scheme = ContainerProxyScheme::kSocks5h;
  // Canonical host. IPv6 literals keep their brackets.
  std::string host;
  uint16_t port = 0;

  // "<scheme>://<host>:<port>".
  std::string ToString() const;
  bool operator==(const ContainerProxyServer&) const = default;
};

struct ContainerProxyCredentials {
  ContainerProxyCredentials();
  ContainerProxyCredentials(std::string username, std::string password);
  ContainerProxyCredentials(const ContainerProxyCredentials&);
  ContainerProxyCredentials& operator=(const ContainerProxyCredentials&);
  ContainerProxyCredentials(ContainerProxyCredentials&&);
  ContainerProxyCredentials& operator=(ContainerProxyCredentials&&);
  ~ContainerProxyCredentials();

  // Returns a fixed marker; never the values.
  std::string ToString() const;
  bool operator==(const ContainerProxyCredentials&) const = default;

  std::string username;
  std::string password;
};

struct ParsedContainerProxy {
  ParsedContainerProxy();
  ParsedContainerProxy(const ParsedContainerProxy&);
  ParsedContainerProxy& operator=(const ParsedContainerProxy&);
  ParsedContainerProxy(ParsedContainerProxy&&);
  ParsedContainerProxy& operator=(ParsedContainerProxy&&);
  ~ParsedContainerProxy();

  // Server only, plus a redaction marker if credentials are present.
  std::string ToString() const;

  ContainerProxyServer server;
  std::optional<ContainerProxyCredentials> credentials;
};

// Parses "<scheme>://[user[:password]@]host[:port][/]" where scheme is http,
// https, socks5 or socks5h. Userinfo may be percent-encoded. Default ports
// are 80, 443 and 1080.
base::expected<ParsedContainerProxy, ContainerProxyError>
ParseContainerProxyUrl(std::string_view url);

// Parses a stored server string. Credentials are rejected.
base::expected<ContainerProxyServer, ContainerProxyError>
ParseContainerProxyServer(std::string_view server);

// Checks length/charset limits (RFC 1929) and refuses to send credentials in
// cleartext to a publicly routable host.
base::expected<void, ContainerProxyError> ValidateContainerProxyCredentials(
    const ContainerProxyServer& server,
    const ContainerProxyCredentials& credentials);

// Stable name for logs and UI error codes.
std::string_view ContainerProxyErrorToString(ContainerProxyError error);

// Plaintext form of credentials that is encrypted before storage.
std::string SerializeContainerProxyCredentials(
    const ContainerProxyCredentials& credentials);
std::optional<ContainerProxyCredentials> DeserializeContainerProxyCredentials(
    std::string_view serialized);

// Single proxy, no DIRECT fallback. SOCKS5 credentials travel with the proxy
// server; HTTP(S) proxy credentials are answered on the 407 challenge.
net::ProxyConfigWithAnnotation CreateContainerProxyConfig(
    const ContainerProxyServer& server,
    const ContainerProxyCredentials* credentials);

// Points at a closed local port so requests fail instead of going direct.
net::ProxyConfigWithAnnotation CreateFailClosedContainerProxyConfig();

struct StoredContainerProxy {
  std::string server;
  // Base64 OSCrypt ciphertext; empty when there are no credentials.
  std::string encrypted_credentials;
};

std::optional<StoredContainerProxy> GetStoredContainerProxy(
    const PrefService& prefs,
    std::string_view container_id);
std::vector<std::string> GetContainerIdsWithStoredProxy(
    const PrefService& prefs);
void SetStoredContainerProxy(PrefService& prefs,
                             std::string_view container_id,
                             const StoredContainerProxy& entry);
void RemoveStoredContainerProxy(PrefService& prefs,
                                std::string_view container_id);

}  // namespace containers

#endif  // BRAVE_COMPONENTS_CONTAINERS_CORE_BROWSER_CONTAINER_PROXY_H_
