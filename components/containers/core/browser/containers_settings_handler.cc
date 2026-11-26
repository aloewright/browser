// Copyright (c) 2025 The Brave Authors. All rights reserved.
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this file,
// You can obtain one at https://mozilla.org/MPL/2.0/.

#include "brave/components/containers/core/browser/containers_settings_handler.h"

#include <algorithm>
#include <utility>

#include "base/uuid.h"
#include "brave/components/containers/core/browser/pref_names.h"
#include "brave/components/containers/core/browser/prefs.h"
#include "components/prefs/pref_service.h"
#include "third_party/abseil-cpp/absl/container/flat_hash_map.h"
#include "third_party/re2/src/re2/re2.h"

#if BUILDFLAG(IS_KO)
#include "base/functional/bind.h"
#include "base/logging.h"
#include "base/strings/string_util.h"
#include "base/types/expected.h"
#endif

namespace containers {

namespace {

// Returns true if the container name is valid.
bool IsContainerNameValid(std::string_view name) {
  // A string that is not empty and does not contain only whitespace.
  return re2::RE2::FullMatch(name, re2::RE2("^.*\\S.*$"));
}

bool IsIconValid(mojom::Icon icon) {
  // Valid icons are in the range of defined mojom::Icon values.
  return icon >= mojom::Icon::kMinValue && icon <= mojom::Icon::kMaxValue;
}

bool IsBackgroundColorValid(SkColor color) {
  return SkColorGetA(color) == SK_AlphaOPAQUE;
}

#if BUILDFLAG(IS_KO)
struct ProxyUpdate {
  std::optional<ParsedContainerProxy> proxy;
  bool keep_credentials = false;
};

base::expected<ProxyUpdate, mojom::ContainerOperationError> ParseProxyUpdate(
    const mojom::ContainerProxyPtr& proxy) {
  ProxyUpdate update;
  if (!proxy || base::TrimWhitespaceASCII(proxy->url, base::TRIM_ALL).empty()) {
    return update;
  }
  auto parsed = ParseContainerProxyUrl(proxy->url);
  if (!parsed.has_value()) {
    // Never log the URL itself: it may carry credentials.
    VLOG(1) << "Rejected container proxy: "
            << ContainerProxyErrorToString(parsed.error());
    return base::unexpected(mojom::ContainerOperationError::kInvalidProxy);
  }
  update.keep_credentials = !parsed->credentials && proxy->has_credentials;
  update.proxy = std::move(*parsed);
  return update;
}
#endif  // BUILDFLAG(IS_KO)

}  // namespace

ContainersSettingsHandler::ContainersSettingsHandler(PrefService* prefs)
    : prefs_(prefs) {
  DCHECK(prefs_);
  pref_change_registrar_.Init(prefs);
  // Watch for external changes to containers list (e.g. sync, other windows)
  pref_change_registrar_.Add(
      prefs::kContainersList,
      base::BindRepeating(&ContainersSettingsHandler::OnContainersChanged,
                          base::Unretained(this)));
}

#if BUILDFLAG(IS_KO)
ContainersSettingsHandler::ContainersSettingsHandler(
    PrefService* prefs,
    ProxyDelegate* proxy_delegate)
    : ContainersSettingsHandler(prefs) {
  proxy_delegate_ = proxy_delegate;
  if (proxy_delegate_) {
    pref_change_registrar_.Add(
        kKoContainerProxiesPref,
        base::BindRepeating(&ContainersSettingsHandler::OnContainersChanged,
                            base::Unretained(this)));
  }
}
#endif  // BUILDFLAG(IS_KO)

ContainersSettingsHandler::~ContainersSettingsHandler() {}

void ContainersSettingsHandler::BindUI(
    mojo::PendingRemote<mojom::ContainersSettingsUI> ui) {
  DCHECK(!ui_);
  ui_.Bind(std::move(ui));
}

void ContainersSettingsHandler::GetContainers(GetContainersCallback callback) {
  std::move(callback).Run(GetContainersForUI());
}

void ContainersSettingsHandler::AddContainer(mojom::ContainerPtr container,
                                             AddContainerCallback callback) {
  if (!container->id.empty()) {
    std::move(callback).Run(mojom::ContainerOperationError::kIdShouldBeEmpty);
    return;
  }

  if (auto error = ValidateEditableContainerProperties(container)) {
    std::move(callback).Run(*error);
    return;
  }

#if BUILDFLAG(IS_KO)
  if (proxy_delegate_) {
    auto update = ParseProxyUpdate(container->proxy);
    if (!update.has_value()) {
      std::move(callback).Run(update.error());
      return;
    }
    container->proxy = nullptr;
    container->id = base::Uuid::GenerateRandomV4().AsLowercaseString();
    const std::string id = container->id;
    proxy_delegate_->SetProxy(
        id, std::move(update->proxy), /*keep_credentials=*/false,
        base::BindOnce(&ContainersSettingsHandler::OnProxyStored,
                       weak_ptr_factory_.GetWeakPtr(), std::move(container),
                       /*is_new=*/true, std::move(callback)));
    return;
  }
#endif  // BUILDFLAG(IS_KO)

  container->id = base::Uuid::GenerateRandomV4().AsLowercaseString();
  AddContainerToPrefs(std::move(container));
  std::move(callback).Run(std::nullopt);
}

void ContainersSettingsHandler::UpdateContainer(
    mojom::ContainerPtr container,
    UpdateContainerCallback callback) {
  if (container->id.empty()) {
    std::move(callback).Run(mojom::ContainerOperationError::kIdShouldBeSet);
    return;
  }

  if (auto error = ValidateEditableContainerProperties(container)) {
    std::move(callback).Run(*error);
    return;
  }

#if BUILDFLAG(IS_KO)
  if (proxy_delegate_) {
    if (!GetContainerFromPrefs(*prefs_, container->id)) {
      std::move(callback).Run(mojom::ContainerOperationError::kNotFound);
      return;
    }
    auto update = ParseProxyUpdate(container->proxy);
    if (!update.has_value()) {
      std::move(callback).Run(update.error());
      return;
    }
    container->proxy = nullptr;
    const std::string id = container->id;
    proxy_delegate_->SetProxy(
        id, std::move(update->proxy), update->keep_credentials,
        base::BindOnce(&ContainersSettingsHandler::OnProxyStored,
                       weak_ptr_factory_.GetWeakPtr(), std::move(container),
                       /*is_new=*/false, std::move(callback)));
    return;
  }
#endif  // BUILDFLAG(IS_KO)

  std::move(callback).Run(UpdateContainerInPrefs(std::move(container)));
}

void ContainersSettingsHandler::RemoveContainer(
    const std::string& id,
    RemoveContainerCallback callback) {
  if (id.empty()) {
    std::move(callback).Run(mojom::ContainerOperationError::kIdShouldBeSet);
    return;
  }

  auto containers = GetContainersFromPrefs(*prefs_);
  auto it = std::ranges::find(containers, id, &mojom::Container::id);
  if (it == containers.end()) {
    std::move(callback).Run(mojom::ContainerOperationError::kNotFound);
    return;
  }

  containers.erase(it);
  SetContainersToPrefs(std::move(containers), *prefs_);

  std::move(callback).Run(std::nullopt);
}

void ContainersSettingsHandler::ReorderContainers(
    const std::vector<std::string>& ordered_ids,
    ReorderContainersCallback callback) {
  auto containers = GetContainersFromPrefs(*prefs_);
  if (ordered_ids.size() != containers.size()) {
    std::move(callback).Run(mojom::ContainerOperationError::kNotFound);
    return;
  }

  absl::flat_hash_map<std::string, size_t> order_map;
  order_map.reserve(ordered_ids.size());
  for (size_t i = 0; i < ordered_ids.size(); ++i) {
    order_map.emplace(ordered_ids[i], i);
  }

  if (order_map.size() != ordered_ids.size()) {
    std::move(callback).Run(mojom::ContainerOperationError::kNotFound);
    return;
  }

  for (const auto& container : containers) {
    if (!order_map.contains(container->id)) {
      std::move(callback).Run(mojom::ContainerOperationError::kNotFound);
      return;
    }
  }

  std::ranges::sort(containers, [&order_map](const auto& lhs, const auto& rhs) {
    return order_map.at(lhs->id) < order_map.at(rhs->id);
  });

  SetContainersToPrefs(std::move(containers), *prefs_);
  std::move(callback).Run(std::nullopt);
}

// static
std::optional<mojom::ContainerOperationError>
ContainersSettingsHandler::ValidateEditableContainerProperties(
    const mojom::ContainerPtr& container) {
  if (!IsContainerNameValid(container->name)) {
    return mojom::ContainerOperationError::kInvalidName;
  }

  if (!IsIconValid(container->icon)) {
    return mojom::ContainerOperationError::kInvalidIcon;
  }

  if (!IsBackgroundColorValid(container->background_color)) {
    return mojom::ContainerOperationError::kInvalidBackgroundColor;
  }

  return std::nullopt;
}

void ContainersSettingsHandler::OnContainersChanged() {
  // Notify UI about container list changes (from this window or others).
  if (ui_) {
    ui_->OnContainersChanged(GetContainersForUI());
  }
}

std::vector<mojom::ContainerPtr> ContainersSettingsHandler::GetContainersForUI()
    const {
  auto containers = GetContainersFromPrefs(*prefs_);
#if BUILDFLAG(IS_KO)
  if (proxy_delegate_) {
    for (auto& container : containers) {
      container->proxy = proxy_delegate_->GetProxyForUI(container->id);
    }
  }
#endif  // BUILDFLAG(IS_KO)
  return containers;
}

void ContainersSettingsHandler::AddContainerToPrefs(
    mojom::ContainerPtr container) {
  auto containers = GetContainersFromPrefs(*prefs_);
  containers.push_back(std::move(container));
  SetContainersToPrefs(std::move(containers), *prefs_);
}

std::optional<mojom::ContainerOperationError>
ContainersSettingsHandler::UpdateContainerInPrefs(
    mojom::ContainerPtr container) {
  auto containers = GetContainersFromPrefs(*prefs_);
  auto it = std::ranges::find(containers, container->id, &mojom::Container::id);
  if (it == containers.end()) {
    return mojom::ContainerOperationError::kNotFound;
  }
  *it = std::move(container);
  SetContainersToPrefs(std::move(containers), *prefs_);
  return std::nullopt;
}

#if BUILDFLAG(IS_KO)
void ContainersSettingsHandler::OnProxyStored(mojom::ContainerPtr container,
                                              bool is_new,
                                              OperationCallback callback,
                                              bool success) {
  if (!success) {
    std::move(callback).Run(
        mojom::ContainerOperationError::kProxyCredentialsUnavailable);
    return;
  }
  if (is_new) {
    AddContainerToPrefs(std::move(container));
    std::move(callback).Run(std::nullopt);
    return;
  }
  std::move(callback).Run(UpdateContainerInPrefs(std::move(container)));
}
#endif  // BUILDFLAG(IS_KO)

}  // namespace containers
