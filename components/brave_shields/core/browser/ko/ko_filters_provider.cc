// Copyright (c) 2026 The Brave Authors. All rights reserved.
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this file,
// You can obtain one at https://mozilla.org/MPL/2.0/.

#include "brave/components/brave_shields/core/browser/ko/ko_filters_provider.h"

#include <optional>
#include <utility>

#include "base/barrier_callback.h"
#include "base/check.h"
#include "base/functional/bind.h"
#include "brave/components/brave_component_updater/browser/dat_file_util.h"
#include "brave/components/brave_shields/core/browser/ad_block_filters_provider_manager.h"

namespace brave_shields {

namespace {

struct LoadedList {
  uint8_t permission_mask = 0;
  std::optional<std::string> data;
};

using FilterSetCallback =
    base::OnceCallback<void(rust::Box<adblock::FilterSet>*)>;

void AddListsToFilterSet(
    std::vector<std::pair<uint8_t, DATFileDataBuffer>> lists,
    rust::Box<adblock::FilterSet>* filter_set) {
  for (const auto& [permission_mask, buffer] : lists) {
    (*filter_set)->add_filter_list_with_permissions(buffer, permission_mask);
  }
}

void OnAllListsLoaded(base::OnceCallback<void(FilterSetCallback)> cb,
                      std::vector<LoadedList> loaded) {
  std::vector<std::pair<uint8_t, DATFileDataBuffer>> lists;
  for (auto& list : loaded) {
    if (!list.data) {
      continue;  // Logged by KoFilterListSource; other lists still apply.
    }
    lists.emplace_back(list.permission_mask,
                       DATFileDataBuffer(list.data->begin(), list.data->end()));
  }
  std::move(cb).Run(base::BindOnce(&AddListsToFilterSet, std::move(lists)));
}

}  // namespace

KoFiltersProvider::KoFiltersProvider(KoFilterListSource* source,
                                     AdBlockFiltersProviderManager* manager,
                                     bool is_default_engine,
                                     CatalogQuery is_list_enabled,
                                     CatalogQuery is_component_ready)
    : AdBlockFiltersProvider(is_default_engine, manager),
      source_(source),
      is_list_enabled_(std::move(is_list_enabled)),
      is_component_ready_(std::move(is_component_ready)) {
  CHECK(source_);
  source_observation_.Observe(source_);
}

KoFiltersProvider::~KoFiltersProvider() = default;

std::string KoFiltersProvider::GetNameForDebugging() {
  return "KoFiltersProvider";
}

bool KoFiltersProvider::IsInitialized() const {
  return source_->IsReady();
}

void KoFiltersProvider::Refresh() {
  NotifyObservers(engine_is_default_);
}

std::vector<KoFilterListEntry> KoFiltersProvider::GetSelectedListsForTesting()
    const {
  return SelectLists();
}

std::vector<KoFilterListEntry> KoFiltersProvider::SelectLists() const {
  std::vector<KoFilterListEntry> selected;
  for (const auto& entry : source_->manifest().lists) {
    if (!entry.AppliesToEngine(engine_is_default_)) {
      continue;
    }
    if (!entry.replaces.empty() && (!is_list_enabled_.Run(entry.replaces) ||
                                    is_component_ready_.Run(entry.replaces))) {
      continue;
    }
    selected.push_back(entry);
  }
  return selected;
}

void KoFiltersProvider::LoadFilterSet(
    base::OnceCallback<void(FilterSetCallback)> cb) {
  const std::vector<KoFilterListEntry> lists = SelectLists();
  if (lists.empty()) {
    std::move(cb).Run(base::BindOnce([](rust::Box<adblock::FilterSet>*) {}));
    return;
  }
  auto barrier = base::BarrierCallback<LoadedList>(
      lists.size(), base::BindOnce(&OnAllListsLoaded, std::move(cb)));
  for (const auto& entry : lists) {
    source_->LoadList(
        entry, base::BindOnce(
                   [](uint8_t permission_mask,
                      base::RepeatingCallback<void(LoadedList)> done,
                      std::optional<std::string> data) {
                     done.Run(LoadedList{permission_mask, std::move(data)});
                   },
                   entry.permission_mask, barrier));
  }
}

void KoFiltersProvider::OnKoFilterListsChanged() {
  Refresh();
}

}  // namespace brave_shields
