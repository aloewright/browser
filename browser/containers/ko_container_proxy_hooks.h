// Copyright (c) 2026 The Brave Authors. All rights reserved.
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this file,
// You can obtain one at https://mozilla.org/MPL/2.0/.

#ifndef BRAVE_BROWSER_CONTAINERS_KO_CONTAINER_PROXY_HOOKS_H_
#define BRAVE_BROWSER_CONTAINERS_KO_CONTAINER_PROXY_HOOKS_H_

#include <memory>

#include "content/public/browser/login_delegate.h"
#include "services/network/public/mojom/network_context.mojom-forward.h"

namespace base {
class FilePath;
}  // namespace base

namespace content {
class BrowserContext;
class RenderFrameHost;
class WebContents;
struct GlobalRequestID;
}  // namespace content

namespace net {
class AuthChallengeInfo;
}  // namespace net

namespace network {
class URLLoaderFactoryBuilder;
}  // namespace network

// K-O entry points used by BraveContentBrowserClient.
namespace containers {

// Puts a container's network context behind its container's proxy, if any.
void ConfigureKoContainerNetworkContext(
    content::BrowserContext* context,
    const base::FilePath& relative_partition_path,
    network::mojom::NetworkContextParams* params);

// Answers proxy auth for proxied containers from stored credentials and never
// prompts. Returns nullptr, leaving `callback` untouched, for other requests.
std::unique_ptr<content::LoginDelegate>
MaybeCreateKoContainerProxyLoginDelegate(
    const net::AuthChallengeInfo& auth_info,
    content::WebContents* web_contents,
    const content::GlobalRequestID& request_id,
    bool first_auth_attempt,
    content::LoginDelegate::LoginAuthRequiredCallback& callback);

// Speculative prefetches use the default (or an isolated, unproxied) network
// context rather than the container's, so they would leak cookies and the
// real IP. Returns true and fails the factory for container frames.
bool MaybeBlockKoContainerPrefetch(
    content::RenderFrameHost* frame,
    network::URLLoaderFactoryBuilder& factory_builder);

}  // namespace containers

#endif  // BRAVE_BROWSER_CONTAINERS_KO_CONTAINER_PROXY_HOOKS_H_
