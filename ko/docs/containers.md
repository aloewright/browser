# Containers and per-container proxies

K-O builds on Brave Containers (`components/containers`): each container is
its own storage partition (`containers` domain, partition name = container
id), so cookies, storage and caches never mix between containers. K-O adds
an optional **proxy per container** and turns Containers on in every channel,
because workspaces depend on them.

Everything here is guarded by `BUILDFLAG(IS_KO)` (or the `KO_BUILD` define in
`chromium_src`). With `is_ko = false` the code is not compiled and behaviour
is stock Brave.

## Setting a proxy

Settings > Containers > edit a container > **Proxy**:

| Field | Example |
| --- | --- |
| Proxy | `socks5h://100.64.0.10:1080` |
| Username (optional) | `ko-proxy-user` |
| Password | (hidden) |

Accepted schemes: `http`, `https`, `socks5`, `socks5h`. Default ports are
80, 443 and 1080. A full URL with `user:pass@` also works; percent-encode
`@`, `:`, `/`, `#` and `?` in credentials. Paths, queries and fragments are
rejected.

Credentials are refused over a cleartext scheme (`http`, `socks5`,
`socks5h`) unless the host is private: a non-public IP (RFC 1918, CGNAT
`100.64.0.0/10` as used by NetBird, loopback, link-local, ULA) or a name
ending in `.netbird.cloud`, `.netbird.selfhosted`, `.internal`, `.local`,
`.lan`, `.home.arpa` or `.localhost`. Use `https://` for anything public.

`socks5` and `socks5h` behave the same: Chromium's SOCKS5 client always sends
the host name to the proxy, so names are resolved at the exit.

## Storage

- The containers list (`brave.containers.list`) is synced and never holds
  proxy data.
- Proxies live in the local-only pref `ko.containers.proxies`:
  `{ "<container id>": { "server": "socks5h://100.64.0.10:1080",
  "credentials": "<base64 OSCrypt ciphertext>" } }`. On macOS OSCrypt is
  keyed by the Keychain.
- Credentials are encrypted with `os_crypt_async`. If encryption is not
  available, nothing is stored (the dialog shows an error); there is no
  plaintext fallback.
- The UI only ever gets `scheme://host:port` plus "credentials saved". Saving
  without new credentials keeps the stored ones only if the server is
  unchanged.
- `ContainerProxyServer::ToString()`, `ParsedContainerProxy::ToString()` and
  all log lines print the server only; credentials print as `<redacted>`.
- Entries for containers that no longer exist are pruned at startup. A
  deleted container keeps its proxy until then, so its open tabs stay behind
  it.

## How it is enforced

Code: `browser/containers/ko_container_proxy_*`,
`components/containers/core/browser/container_proxy.*`.

- **Network context.** `BraveContentBrowserClient::ConfigureNetworkContextParams`
  puts every container context (`Storage/ext/containers/<hash>`) behind a
  relay between it and the profile-wide `ProxyConfigMonitor`. While the
  container has a proxy, the context gets that one proxy with **no DIRECT
  fallback**, and profile, policy, extension or system proxy changes are
  held back. Without one, the profile's config passes through unchanged.
  Loopback and link-local destinations keep Chromium's implicit bypass, so
  pages cannot reach the proxy host's own local services.
- **Fail closed.**
  - Proxy unreachable: requests fail with `ERR_PROXY_CONNECTION_FAILED`.
  - SOCKS credentials not decryptable yet (OSCrypt still starting): the
    context gets no config, and the network service holds requests until
    one arrives.
  - SOCKS credentials not decryptable at all: the proxy is used without
    them, so an authenticating proxy rejects the connection.
  - Corrupt stored entry: a closed local port (`127.0.0.1:9`) is used.
  - Proxy added while the container already has a context: SOCKS
    credentials still waiting for OSCrypt put it on the closed port first.
  - Proxy removed while tabs are open: the context goes back to the
    profile's proxy settings, like any other container. A deleted
    container's context stays on the closed port until the next start.
- **Private windows.** A container partition opened from a private window
  uses the same proxy as in the regular profile.
- **Live changes.** Adding, editing or removing a proxy applies to open
  contexts immediately, including ones created before the proxy was set.
- **Proxy auth.**
  - SOCKS5: brave's username/password SOCKS5 auth carries the credentials
    in the proxy server sent to the network service.
  - HTTP(S) proxies: K-O answers the 407 challenge itself, once, and only
    when the challenger is exactly the configured proxy. Proxied containers
    never show a proxy login prompt. If the credentials are wrong or the
    challenger differs, the request fails.
- **WebRTC.** Tabs in a proxied container get `disable_non_proxied_udp`, and
  the per-URL overrides are cleared (`chromium_src/chrome/browser/ui/prefs/
  prefs_tab_helper.cc`). Open tabs, also in private windows, are updated
  when a proxy is added.
- **Prefetch.** Speculation-rules prefetch from any container frame is
  blocked. Chromium runs it in the default or an isolated, unproxied network
  context, which would leak cookies and the real IP.
- **DNS prefetch / preconnect.** These run against the container's own
  network context. The preconnect manager asks the proxy resolver first and
  skips local DNS when a proxy applies. "Preload pages" also stays off by
  default in Brave.

## Residential proxies through NetBird

Bright Data credentials never reach the browser. Run the proxy peer from
`ko/infra/proxy/` (see `ko/docs/infra.md` from the agentd area). It is gost on
a NetBird peer that chains to Bright Data. Then give the container the
peer's URL, for example:

```text
socks5h://100.64.0.10:1080      (+ the peer's client username/password)
http://100.64.0.10:8080
```

In NetBird, allow only the Mac's peer group to reach those ports.

## Limits

- Browser-level traffic is not per-container. This covers omnibox
  suggestions, Safe Browsing, extension background pages, the component
  updater and Sync, which use the profile or system network context. Keep
  search suggestions and "Preload pages" off for strict isolation (the
  debloat defaults).
- HTTP(S) proxy auth is only answered for requests that can be tied to a
  container tab or renderer. Browser-initiated requests from a container
  partition without a tab (rare) get no credentials and fail.
- A compromised renderer could ignore the WebRTC policy and open UDP
  sockets directly. The policy is enforced in the renderer, as in Chromium.
- SOCKS credentials are part of the proxy server string inside the network
  stack. A `chrome://net-export` log can therefore contain them, so do not
  share net-export logs from proxied containers.

## Tests

- `components/containers/core/browser/container_proxy_unittest.cc`: parsing,
  validation, cleartext-credential rules, no credentials in `ToString()`,
  config shape (no DIRECT), local-only prefs.
- `components/containers/core/browser/containers_settings_handler_unittest.cc`
  (`ContainersSettingsHandlerProxyTest`): proxies never reach the synced
  list, invalid proxies and storage failures add nothing.
- `browser/containers/ko_container_proxy_service_unittest.cc`: partition
  path, credentials encrypted at rest, HTTP proxy auth matching, live
  updates (including a proxy added to an existing context), profile changes
  held back while proxied, deleted containers staying closed, pending
  without OSCrypt.
