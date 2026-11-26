// Copyright (c) 2026 The Brave Authors. All rights reserved.
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this file,
// You can obtain one at https://mozilla.org/MPL/2.0/.

#include "brave/components/containers/core/browser/container_proxy.h"

#include <algorithm>
#include <array>
#include <utility>

#include "base/json/json_reader.h"
#include "base/json/json_writer.h"
#include "base/strings/strcat.h"
#include "base/strings/string_number_conversions.h"
#include "base/strings/string_util.h"
#include "base/values.h"
#include "components/prefs/pref_service.h"
#include "components/prefs/scoped_user_pref_update.h"
#include "net/base/host_port_pair.h"
#include "net/base/ip_address.h"
#include "net/base/proxy_server.h"
#include "net/proxy_resolution/proxy_config.h"
#include "net/proxy_resolution/proxy_config_with_annotation.h"
#include "net/traffic_annotation/network_traffic_annotation.h"
#include "url/gurl.h"

namespace containers {

namespace {

constexpr size_t kMaxProxyUrlLength = 2048;
// RFC 1929 limits each SOCKS5 field to 255 bytes; use it for HTTP too.
constexpr size_t kMaxCredentialLength = 255;

constexpr char kServerKey[] = "server";
constexpr char kCredentialsKey[] = "credentials";
constexpr char kUsernameKey[] = "u";
constexpr char kPasswordKey[] = "p";

// Discard port on loopback: nothing listens there by default, so requests
// fail fast with ERR_PROXY_CONNECTION_FAILED.
constexpr char kFailClosedHost[] = "127.0.0.1";
constexpr uint16_t kFailClosedPort = 9;

// Host suffixes treated as private networks (NetBird, mDNS, home LANs).
constexpr std::array<std::string_view, 7> kPrivateHostSuffixes = {
    ".netbird.cloud", ".netbird.selfhosted", ".internal", ".local",
    ".lan",           ".home.arpa",          ".localhost",
};

constexpr net::NetworkTrafficAnnotationTag kContainerProxyAnnotation =
    net::DefineNetworkTrafficAnnotation("ko_container_proxy", R"(
      semantics {
        sender: "K-O Containers"
        description:
          "Requests from a container that the user configured with a proxy "
          "are sent through that proxy."
        trigger: "Any network request made by a tab in such a container."
        data: "The request, sent to the user's proxy."
        destination: OTHER
        destination_other: "The proxy server configured by the user."
      }
      policy {
        cookies_allowed: YES
        cookies_store: "The container's storage partition."
        setting: "Settings > Containers > edit a container > Proxy."
        policy_exception_justification: "User-configured, per container."
      })");

struct SchemeInfo {
  std::string_view name;
  ContainerProxyScheme scheme;
  uint16_t default_port;
};

constexpr std::array<SchemeInfo, 4> kSchemes = {{
    {"http", ContainerProxyScheme::kHttp, 80},
    {"https", ContainerProxyScheme::kHttps, 443},
    {"socks5", ContainerProxyScheme::kSocks5, 1080},
    {"socks5h", ContainerProxyScheme::kSocks5h, 1080},
}};

const SchemeInfo* FindSchemeByName(std::string_view name) {
  for (const auto& info : kSchemes) {
    if (info.name == name) {
      return &info;
    }
  }
  return nullptr;
}

const SchemeInfo& GetSchemeInfo(ContainerProxyScheme scheme) {
  for (const auto& info : kSchemes) {
    if (info.scheme == scheme) {
      return info;
    }
  }
  NOTREACHED();
}

bool ContainsControlOrSpace(std::string_view value) {
  return std::ranges::any_of(value, [](char c) {
    return base::IsAsciiControl(c) || c == ' ';
  });
}

// Strict percent-decoding: every '%' must start a valid escape.
std::optional<std::string> PercentDecode(std::string_view input) {
  std::string out;
  out.reserve(input.size());
  for (size_t i = 0; i < input.size(); ++i) {
    if (input[i] != '%') {
      out.push_back(input[i]);
      continue;
    }
    if (i + 2 >= input.size() || !base::IsHexDigit(input[i + 1]) ||
        !base::IsHexDigit(input[i + 2])) {
      return std::nullopt;
    }
    out.push_back(static_cast<char>(base::HexDigitToInt(input[i + 1]) * 16 +
                                    base::HexDigitToInt(input[i + 2])));
    i += 2;
  }
  return out;
}

bool IsPrivateHost(std::string_view host) {
  std::string_view bare = host;
  if (bare.size() > 2 && bare.front() == '[' && bare.back() == ']') {
    bare = bare.substr(1, bare.size() - 2);
  }
  if (auto ip = net::IPAddress::FromIPLiteral(bare)) {
    return !ip->IsPubliclyRoutable();
  }
  if (bare == "localhost") {
    return true;
  }
  return std::ranges::any_of(kPrivateHostSuffixes, [bare](std::string_view s) {
    return base::EndsWith(bare, s);
  });
}

base::expected<ParsedContainerProxy, ContainerProxyError> Parse(
    std::string_view input,
    bool allow_credentials) {
  const std::string_view url = base::TrimWhitespaceASCII(input, base::TRIM_ALL);
  if (url.empty()) {
    return base::unexpected(ContainerProxyError::kEmpty);
  }
  if (url.size() > kMaxProxyUrlLength) {
    return base::unexpected(ContainerProxyError::kTooLong);
  }
  if (ContainsControlOrSpace(url)) {
    return base::unexpected(ContainerProxyError::kInvalidUrl);
  }

  const size_t scheme_end = url.find("://");
  if (scheme_end == std::string_view::npos || scheme_end == 0) {
    return base::unexpected(ContainerProxyError::kInvalidUrl);
  }
  const SchemeInfo* scheme_info =
      FindSchemeByName(base::ToLowerASCII(url.substr(0, scheme_end)));
  if (!scheme_info) {
    return base::unexpected(ContainerProxyError::kUnsupportedScheme);
  }

  std::string_view rest = url.substr(scheme_end + 3);
  const size_t authority_end = rest.find_first_of("/?#");
  if (authority_end != std::string_view::npos) {
    if (rest.substr(authority_end) != "/") {
      return base::unexpected(ContainerProxyError::kUnexpectedComponents);
    }
    rest = rest.substr(0, authority_end);
  }

  // Userinfo ends at the last '@'.
  std::string_view host_port = rest;
  std::optional<ContainerProxyCredentials> credentials;
  if (const size_t at = rest.rfind('@'); at != std::string_view::npos) {
    if (!allow_credentials) {
      return base::unexpected(ContainerProxyError::kInvalidCredentials);
    }
    const std::string_view userinfo = rest.substr(0, at);
    host_port = rest.substr(at + 1);
    const size_t colon = userinfo.find(':');
    auto username = PercentDecode(userinfo.substr(0, colon));
    auto password = PercentDecode(colon == std::string_view::npos
                                      ? std::string_view()
                                      : userinfo.substr(colon + 1));
    if (!username || !password) {
      return base::unexpected(ContainerProxyError::kInvalidCredentials);
    }
    credentials.emplace(std::move(*username), std::move(*password));
  }

  std::string_view host_part = host_port;
  std::string_view port_part;
  bool has_port = false;
  if (host_port.starts_with('[')) {
    const size_t close = host_port.find(']');
    if (close == std::string_view::npos) {
      return base::unexpected(ContainerProxyError::kInvalidUrl);
    }
    host_part = host_port.substr(0, close + 1);
    const std::string_view after = host_port.substr(close + 1);
    if (!after.empty()) {
      if (after.front() != ':') {
        return base::unexpected(ContainerProxyError::kInvalidUrl);
      }
      has_port = true;
      port_part = after.substr(1);
    }
  } else if (const size_t colon = host_port.rfind(':');
             colon != std::string_view::npos) {
    host_part = host_port.substr(0, colon);
    has_port = true;
    port_part = host_port.substr(colon + 1);
  }

  if (host_part.empty()) {
    return base::unexpected(ContainerProxyError::kInvalidUrl);
  }
  // Canonicalize the host with a standard scheme.
  const GURL host_url(base::StrCat({"http://", host_part, "/"}));
  if (!host_url.is_valid() || host_url.host().empty() ||
      host_url.has_username() || host_url.has_password() ||
      host_url.has_port() || host_url.path() != "/") {
    return base::unexpected(ContainerProxyError::kInvalidUrl);
  }

  uint32_t port = scheme_info->default_port;
  if (has_port) {
    if (port_part.empty() || !base::StringToUint(port_part, &port) ||
        port == 0 || port > 65535) {
      return base::unexpected(ContainerProxyError::kInvalidPort);
    }
  }

  ParsedContainerProxy parsed;
  parsed.server.scheme = scheme_info->scheme;
  parsed.server.host = std::string(host_url.host());
  parsed.server.port = static_cast<uint16_t>(port);
  if (credentials) {
    if (auto valid =
            ValidateContainerProxyCredentials(parsed.server, *credentials);
        !valid.has_value()) {
      return base::unexpected(valid.error());
    }
    parsed.credentials = std::move(credentials);
  }
  return parsed;
}

}  // namespace

std::string ContainerProxyServer::ToString() const {
  return base::StrCat({GetSchemeInfo(scheme).name, "://", host, ":",
                       base::NumberToString(port)});
}

ContainerProxyCredentials::ContainerProxyCredentials() = default;
ContainerProxyCredentials::ContainerProxyCredentials(std::string username,
                                                     std::string password)
    : username(std::move(username)), password(std::move(password)) {}
ContainerProxyCredentials::ContainerProxyCredentials(
    const ContainerProxyCredentials&) = default;
ContainerProxyCredentials& ContainerProxyCredentials::operator=(
    const ContainerProxyCredentials&) = default;
ContainerProxyCredentials::ContainerProxyCredentials(
    ContainerProxyCredentials&&) = default;
ContainerProxyCredentials& ContainerProxyCredentials::operator=(
    ContainerProxyCredentials&&) = default;
ContainerProxyCredentials::~ContainerProxyCredentials() = default;

std::string ContainerProxyCredentials::ToString() const {
  return "<redacted>";
}

ParsedContainerProxy::ParsedContainerProxy() = default;
ParsedContainerProxy::ParsedContainerProxy(const ParsedContainerProxy&) =
    default;
ParsedContainerProxy& ParsedContainerProxy::operator=(
    const ParsedContainerProxy&) = default;
ParsedContainerProxy::ParsedContainerProxy(ParsedContainerProxy&&) = default;
ParsedContainerProxy& ParsedContainerProxy::operator=(ParsedContainerProxy&&) =
    default;
ParsedContainerProxy::~ParsedContainerProxy() = default;

std::string ParsedContainerProxy::ToString() const {
  if (!credentials) {
    return server.ToString();
  }
  return base::StrCat({server.ToString(), " (credentials ",
                       credentials->ToString(), ")"});
}

base::expected<ParsedContainerProxy, ContainerProxyError>
ParseContainerProxyUrl(std::string_view url) {
  return Parse(url, /*allow_credentials=*/true);
}

base::expected<ContainerProxyServer, ContainerProxyError>
ParseContainerProxyServer(std::string_view server) {
  auto parsed = Parse(server, /*allow_credentials=*/false);
  if (!parsed.has_value()) {
    return base::unexpected(parsed.error());
  }
  return std::move(parsed->server);
}

base::expected<void, ContainerProxyError> ValidateContainerProxyCredentials(
    const ContainerProxyServer& server,
    const ContainerProxyCredentials& credentials) {
  if (credentials.username.empty() ||
      credentials.username.size() > kMaxCredentialLength ||
      credentials.password.size() > kMaxCredentialLength ||
      ContainsControlOrSpace(credentials.username) ||
      std::ranges::any_of(credentials.password,
                          [](char c) { return base::IsAsciiControl(c); }) ||
      !base::IsStringUTF8(credentials.username) ||
      !base::IsStringUTF8(credentials.password)) {
    return base::unexpected(ContainerProxyError::kInvalidCredentials);
  }
  // HTTP Basic cannot carry ':' in the username.
  if ((server.scheme == ContainerProxyScheme::kHttp ||
       server.scheme == ContainerProxyScheme::kHttps) &&
      credentials.username.find(':') != std::string::npos) {
    return base::unexpected(ContainerProxyError::kInvalidCredentials);
  }
  if (server.scheme != ContainerProxyScheme::kHttps &&
      !IsPrivateHost(server.host)) {
    return base::unexpected(ContainerProxyError::kInsecureCredentials);
  }
  return base::ok();
}

std::string_view ContainerProxyErrorToString(ContainerProxyError error) {
  switch (error) {
    case ContainerProxyError::kEmpty:
      return "empty";
    case ContainerProxyError::kTooLong:
      return "too_long";
    case ContainerProxyError::kInvalidUrl:
      return "invalid_url";
    case ContainerProxyError::kUnsupportedScheme:
      return "unsupported_scheme";
    case ContainerProxyError::kInvalidPort:
      return "invalid_port";
    case ContainerProxyError::kUnexpectedComponents:
      return "unexpected_components";
    case ContainerProxyError::kInvalidCredentials:
      return "invalid_credentials";
    case ContainerProxyError::kInsecureCredentials:
      return "insecure_credentials";
  }
  NOTREACHED();
}

std::string SerializeContainerProxyCredentials(
    const ContainerProxyCredentials& credentials) {
  return base::WriteJson(base::DictValue()
                             .Set(kUsernameKey, credentials.username)
                             .Set(kPasswordKey, credentials.password))
      .value_or(std::string());
}

std::optional<ContainerProxyCredentials> DeserializeContainerProxyCredentials(
    std::string_view serialized) {
  auto dict = base::JSONReader::ReadDict(serialized, base::JSON_PARSE_RFC);
  if (!dict) {
    return std::nullopt;
  }
  const std::string* username = dict->FindString(kUsernameKey);
  const std::string* password = dict->FindString(kPasswordKey);
  if (!username || !password || username->empty()) {
    return std::nullopt;
  }
  return ContainerProxyCredentials(*username, *password);
}

net::ProxyConfigWithAnnotation CreateContainerProxyConfig(
    const ContainerProxyServer& server,
    const ContainerProxyCredentials* credentials) {
  net::ProxyServer::Scheme net_scheme = net::ProxyServer::SCHEME_SOCKS5;
  switch (server.scheme) {
    case ContainerProxyScheme::kHttp:
      net_scheme = net::ProxyServer::SCHEME_HTTP;
      break;
    case ContainerProxyScheme::kHttps:
      net_scheme = net::ProxyServer::SCHEME_HTTPS;
      break;
    case ContainerProxyScheme::kSocks5:
    case ContainerProxyScheme::kSocks5h:
      net_scheme = net::ProxyServer::SCHEME_SOCKS5;
      break;
  }

  // HostPortPair wants IPv6 hosts without brackets.
  std::string_view host = server.host;
  if (host.size() > 2 && host.front() == '[' && host.back() == ']') {
    host = host.substr(1, host.size() - 2);
  }
  const bool socks_auth =
      credentials && net_scheme == net::ProxyServer::SCHEME_SOCKS5;
  const net::HostPortPair host_port_pair =
      socks_auth ? net::HostPortPair(credentials->username,
                                     credentials->password, host, server.port)
                 : net::HostPortPair(host, server.port);

  net::ProxyConfig config;
  // Keep the implicit loopback/link-local bypass: those must not reach the
  // proxy host's own local services.
  config.proxy_rules().type = net::ProxyConfig::ProxyRules::Type::PROXY_LIST;
  config.proxy_rules().single_proxies.SetSingleProxyServer(
      net::ProxyServer(net_scheme, host_port_pair));
  return net::ProxyConfigWithAnnotation(config, kContainerProxyAnnotation);
}

net::ProxyConfigWithAnnotation CreateFailClosedContainerProxyConfig() {
  ContainerProxyServer server;
  server.scheme = ContainerProxyScheme::kSocks5;
  server.host = kFailClosedHost;
  server.port = kFailClosedPort;
  return CreateContainerProxyConfig(server, nullptr);
}

std::optional<StoredContainerProxy> GetStoredContainerProxy(
    const PrefService& prefs,
    std::string_view container_id) {
  const base::DictValue* entry =
      prefs.GetDict(kKoContainerProxiesPref).FindDict(container_id);
  if (!entry) {
    return std::nullopt;
  }
  const std::string* server = entry->FindString(kServerKey);
  if (!server || server->empty()) {
    return std::nullopt;
  }
  StoredContainerProxy stored;
  stored.server = *server;
  if (const std::string* creds = entry->FindString(kCredentialsKey)) {
    stored.encrypted_credentials = *creds;
  }
  return stored;
}

std::vector<std::string> GetContainerIdsWithStoredProxy(
    const PrefService& prefs) {
  std::vector<std::string> ids;
  for (const auto [id, value] : prefs.GetDict(kKoContainerProxiesPref)) {
    if (value.is_dict()) {
      ids.push_back(id);
    }
  }
  return ids;
}

void SetStoredContainerProxy(PrefService& prefs,
                             std::string_view container_id,
                             const StoredContainerProxy& entry) {
  base::DictValue dict;
  dict.Set(kServerKey, entry.server);
  if (!entry.encrypted_credentials.empty()) {
    dict.Set(kCredentialsKey, entry.encrypted_credentials);
  }
  ScopedDictPrefUpdate update(&prefs, kKoContainerProxiesPref);
  update->Set(container_id, std::move(dict));
}

void RemoveStoredContainerProxy(PrefService& prefs,
                                std::string_view container_id) {
  if (!prefs.GetDict(kKoContainerProxiesPref).contains(container_id)) {
    return;
  }
  ScopedDictPrefUpdate update(&prefs, kKoContainerProxiesPref);
  update->Remove(container_id);
}

}  // namespace containers
