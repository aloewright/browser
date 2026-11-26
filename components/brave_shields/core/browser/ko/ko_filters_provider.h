// Copyright (c) 2026 The Brave Authors. All rights reserved.
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this file,
// You can obtain one at https://mozilla.org/MPL/2.0/.

#ifndef BRAVE_COMPONENTS_BRAVE_SHIELDS_CORE_BROWSER_KO_KO_FILTERS_PROVIDER_H_
#define BRAVE_COMPONENTS_BRAVE_SHIELDS_CORE_BROWSER_KO_KO_FILTERS_PROVIDER_H_

#include <string>
#include <vector>

#include "base/functional/callback.h"
#include "base/memory/raw_ptr.h"
#include "base/scoped_observation.h"
#include "brave/components/brave_shields/core/browser/ad_block_filters_provider.h"
#include "brave/components/brave_shields/core/browser/ko/ko_filter_list_manifest.h"
#include "brave/components/brave_shields/core/browser/ko/ko_filter_list_source.h"

namespace brave_shields {

class AdBlockFiltersProviderManager;

// Feeds the K-O baseline lists of one engine. A list that substitutes for a
// Brave catalog entry is loaded only while that entry is enabled and its
// component has not been delivered; component data replaces it once present.
class KoFiltersProvider : public AdBlockFiltersProvider,
                          public KoFilterListSource::Observer {
 public:
  using CatalogQuery = base::RepeatingCallback<bool(const std::string& uuid)>;

  KoFiltersProvider(KoFilterListSource* source,
                    AdBlockFiltersProviderManager* manager,
                    bool is_default_engine,
                    CatalogQuery is_list_enabled,
                    CatalogQuery is_component_ready);
  KoFiltersProvider(const KoFiltersProvider&) = delete;
  KoFiltersProvider& operator=(const KoFiltersProvider&) = delete;
  ~KoFiltersProvider() override;

  // AdBlockFiltersProvider:
  void LoadFilterSet(
      base::OnceCallback<void(
          base::OnceCallback<void(rust::Box<adblock::FilterSet>*)>)>) override;
  std::string GetNameForDebugging() override;
  bool IsInitialized() const override;

  // Re-evaluates which lists apply, e.g. after a catalog toggle.
  void Refresh();

  std::vector<KoFilterListEntry> GetSelectedListsForTesting() const;

 private:
  std::vector<KoFilterListEntry> SelectLists() const;

  // KoFilterListSource::Observer:
  void OnKoFilterListsChanged() override;

  raw_ptr<KoFilterListSource> source_;
  CatalogQuery is_list_enabled_;
  CatalogQuery is_component_ready_;
  base::ScopedObservation<KoFilterListSource, KoFilterListSource::Observer>
      source_observation_{this};
};

}  // namespace brave_shields

#endif  // BRAVE_COMPONENTS_BRAVE_SHIELDS_CORE_BROWSER_KO_KO_FILTERS_PROVIDER_H_
