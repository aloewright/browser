// Copyright (c) 2026 The Brave Authors. All rights reserved.
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this file,
// You can obtain one at https://mozilla.org/MPL/2.0/.

#include "brave/browser/containers/ko_container_proxy_hooks.h"

#include <optional>
#include <string>
#include <tuple>
#include <utility>

#include "base/feature_list.h"
#include "base/files/file_path.h"
#include "base/functional/bind.h"
#include "base/location.h"
#include "base/memory/self_deleting.h"
#include "base/memory/weak_ptr.h"
#include "base/task/sequenced_task_runner.h"
#include "brave/browser/containers/ko_container_proxy_service.h"
#include "brave/browser/containers/ko_container_proxy_service_factory.h"
#include "brave/components/containers/content/browser/storage_partition_utils.h"
#include "brave/components/containers/core/common/features.h"
#include "chrome/browser/profiles/profile.h"
#include "content/public/browser/global_request_id.h"
#include "content/public/browser/render_frame_host.h"
#include "content/public/browser/render_process_host.h"
#include "content/public/browser/storage_partition.h"
#include "content/public/browser/web_contents.h"
#include "mojo/public/cpp/bindings/remote.h"
#include "net/base/auth.h"
#include "net/base/net_errors.h"
#include "services/network/public/cpp/self_deleting_url_loader_factory.h"
#include "services/network/public/cpp/url_loader_completion_status.h"
#include "services/network/public/cpp/url_loader_factory_builder.h"
#include "services/network/public/mojom/network_context.mojom.h"
#include "services/network/public/mojom/url_loader.mojom.h"

namespace containers {

namespace {

// Fails every request with ERR_BLOCKED_BY_CLIENT.
class BlockedURLLoaderFactory final
    : public network::SelfDeletingURLLoaderFactory {
 public:
  BlockedURLLoaderFactory(
      mojo::PendingReceiver<network::mojom::URLLoaderFactory> receiver,
      base::SelfDeletingPassKey key)
      : SelfDeletingURLLoaderFactory(std::move(receiver), key) {}

  BlockedURLLoaderFactory(const BlockedURLLoaderFactory&) = delete;
  BlockedURLLoaderFactory& operator=(const BlockedURLLoaderFactory&) = delete;

 private:
  ~BlockedURLLoaderFactory() override = default;

  // network::mojom::URLLoaderFactory:
  void CreateLoaderAndStart(
      mojo::PendingReceiver<network::mojom::URLLoader> loader,
      int32_t request_id,
      uint32_t options,
      const network::ResourceRequest& request,
      mojo::PendingRemote<network::mojom::URLLoaderClient> client,
      const net::MutableNetworkTrafficAnnotationTag& traffic_annotation)
      override {
    mojo::Remote<network::mojom::URLLoaderClient>(std::move(client))
        ->OnComplete(
            network::URLLoaderCompletionStatus(net::ERR_BLOCKED_BY_CLIENT));
  }
};

// Never shows UI. Replies asynchronously, as LoginDelegate requires.
class ContainerProxyLoginDelegate : public content::LoginDelegate {
 public:
  ContainerProxyLoginDelegate(
      std::optional<net::AuthCredentials> credentials,
      content::LoginDelegate::LoginAuthRequiredCallback callback) {
    base::SequencedTaskRunner::GetCurrentDefault()->PostTask(
        FROM_HERE,
        base::BindOnce(&ContainerProxyLoginDelegate::Reply,
                       weak_factory_.GetWeakPtr(), std::move(credentials),
                       std::move(callback)));
  }
  ~ContainerProxyLoginDelegate() override = default;

 private:
  void Reply(std::optional<net::AuthCredentials> credentials,
             content::LoginDelegate::LoginAuthRequiredCallback callback) {
    // nullopt cancels auth, so the request fails instead of prompting.
    std::move(callback).Run(credentials);
  }

  base::WeakPtrFactory<ContainerProxyLoginDelegate> weak_factory_{this};
};

std::string GetContainerIdForRequest(
    content::WebContents* web_contents,
    const content::GlobalRequestID& request_id) {
  if (web_contents) {
    return GetContainerIdForWebContents(web_contents);
  }
  if (request_id.child_id.is_browser() || request_id.child_id.is_null()) {
    return std::string();
  }
  auto* process =
      content::RenderProcessHost::FromID(request_id.child_id.GetUnsafeValue());
  if (!process || !process->GetStoragePartition()) {
    return std::string();
  }
  return std::string(GetContainerIdFromStoragePartitionConfig(
      process->GetStoragePartition()->GetConfig()));
}

}  // namespace

void ConfigureKoContainerNetworkContext(
    content::BrowserContext* context,
    const base::FilePath& relative_partition_path,
    network::mojom::NetworkContextParams* params) {
  if (!base::FeatureList::IsEnabled(features::kContainers)) {
    return;
  }
  // Skip every partition outside Storage/ext/containers/.
  const base::FilePath containers_root =
      KoContainerProxyService::GetRelativePartitionPath("any").DirName();
  if (!containers_root.IsParent(relative_partition_path)) {
    return;
  }
  auto* service = KoContainerProxyServiceFactory::GetForProfile(
      Profile::FromBrowserContext(context));
  if (service) {
    service->ConfigureNetworkContextParams(relative_partition_path, params);
  }
}

std::unique_ptr<content::LoginDelegate> MaybeCreateKoContainerProxyLoginDelegate(
    const net::AuthChallengeInfo& auth_info,
    content::WebContents* web_contents,
    const content::GlobalRequestID& request_id,
    bool first_auth_attempt,
    content::LoginDelegate::LoginAuthRequiredCallback& callback) {
  if (!auth_info.is_proxy ||
      !base::FeatureList::IsEnabled(features::kContainers)) {
    return nullptr;
  }
  const std::string container_id =
      GetContainerIdForRequest(web_contents, request_id);
  if (container_id.empty()) {
    return nullptr;
  }
  content::BrowserContext* browser_context =
      web_contents ? web_contents->GetBrowserContext() : nullptr;
  if (!browser_context && !request_id.child_id.is_browser() &&
      !request_id.child_id.is_null()) {
    if (auto* process = content::RenderProcessHost::FromID(
            request_id.child_id.GetUnsafeValue())) {
      browser_context = process->GetBrowserContext();
    }
  }
  if (!browser_context) {
    return nullptr;
  }
  auto* service = KoContainerProxyServiceFactory::GetForProfile(
      Profile::FromBrowserContext(browser_context));
  if (!service || !service->HasProxy(container_id)) {
    return nullptr;
  }
  // One attempt only: a rejected answer must not loop or fall back to a prompt.
  std::optional<net::AuthCredentials> credentials;
  if (first_auth_attempt) {
    credentials =
        service->GetHttpProxyCredentials(container_id, auth_info.challenger);
  }
  return std::make_unique<ContainerProxyLoginDelegate>(std::move(credentials),
                                                       std::move(callback));
}

bool MaybeBlockKoContainerPrefetch(
    content::RenderFrameHost* frame,
    network::URLLoaderFactoryBuilder& factory_builder) {
  if (!frame || !base::FeatureList::IsEnabled(features::kContainers) ||
      !IsContainersStoragePartition(frame->GetStoragePartition()->GetConfig())) {
    return false;
  }
  // The downstream remote is dropped: nothing reaches the network.
  auto pipes = factory_builder.Append();
  base::MakeSelfDeleting<BlockedURLLoaderFactory>(
      std::move(std::get<0>(pipes)));
  return true;
}

}  // namespace containers
