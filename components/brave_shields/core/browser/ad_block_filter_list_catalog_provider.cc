// Copyright (c) 2021 The Brave Authors. All rights reserved.
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this file,
// You can obtain one at https://mozilla.org/MPL/2.0/.

#include "brave/components/brave_shields/core/browser/ad_block_filter_list_catalog_provider.h"

#include <string>
#include <utility>

#include "base/files/file_path.h"
#include "base/functional/bind.h"
#include "base/memory/weak_ptr.h"
#include "base/task/thread_pool.h"
#include "base/trace_event/trace_event.h"
#include "brave/components/brave_shields/core/browser/ad_block_component_installer.h"
#include "brave/components/ko/buildflags/buildflags.h"

#if BUILDFLAG(IS_KO)
#include "brave/components/brave_shields/core/browser/ko/ko_filter_list_source.h"
#endif

constexpr char kListCatalogFile[] = "list_catalog.json";

namespace brave_shields {

AdBlockFilterListCatalogProvider::AdBlockFilterListCatalogProvider(
    component_updater::ComponentUpdateService* cus) {
  TRACE_EVENT("brave.adblock", "RegisterAdBlockFilterListCatalogComponent",
              perfetto::Flow::FromPointer(this));
  // Can be nullptr in unit tests
  if (cus) {
    RegisterAdBlockFilterListCatalogComponent(
        cus,
        base::BindRepeating(&AdBlockFilterListCatalogProvider::OnComponentReady,
                            weak_factory_.GetWeakPtr()));
#if BUILDFLAG(IS_KO)
    // The catalog component may never arrive; start from the embedded copy.
    // Observers get it like a component update.
    base::ThreadPool::PostTaskAndReplyWithResult(
        FROM_HERE, {base::MayBlock()}, base::BindOnce([] {
          return LoadKoEmbeddedList("brave-list-catalog").value_or("");
        }),
        base::BindOnce(
            [](base::WeakPtr<AdBlockFilterListCatalogProvider> provider,
               const std::string& catalog_json) {
              // The component wins if it showed up meanwhile.
              if (!provider || !provider->component_path_.empty() ||
                  catalog_json.empty()) {
                return;
              }
              provider->OnFilterListCatalogLoaded(catalog_json);
            },
            weak_factory_.GetWeakPtr()));
#endif
  }
}

AdBlockFilterListCatalogProvider::~AdBlockFilterListCatalogProvider() = default;

void AdBlockFilterListCatalogProvider::AddObserver(
    AdBlockFilterListCatalogProvider::Observer* observer) {
  observers_.AddObserver(observer);
}

void AdBlockFilterListCatalogProvider::RemoveObserver(
    AdBlockFilterListCatalogProvider::Observer* observer) {
  observers_.RemoveObserver(observer);
}

void AdBlockFilterListCatalogProvider::OnFilterListCatalogLoaded(
    const std::string& catalog_json) {
  TRACE_EVENT("brave.adblock",
              "AdBlockFilterListCatalogProvider::OnFilterListCatalogLoaded",
              perfetto::TerminatingFlow::FromPointer(this), "catalog_json_size",
              catalog_json.size());
  for (auto& observer : observers_) {
    observer.OnFilterListCatalogLoaded(catalog_json);
  }
}

void AdBlockFilterListCatalogProvider::OnComponentReady(
    const base::FilePath& path) {
  TRACE_EVENT("brave.adblock",
              "AdBlockFilterListCatalogProvider::OnComponentReady",
              perfetto::Flow::FromPointer(this), "path", path.value());
  component_path_ = path;

  // Load the filter list catalog (as a string)
  base::ThreadPool::PostTaskAndReplyWithResult(
      FROM_HERE, {base::MayBlock()},
      base::BindOnce(&brave_component_updater::GetDATFileAsString,
                     component_path_.AppendASCII(kListCatalogFile)),
      base::BindOnce(
          &AdBlockFilterListCatalogProvider::OnFilterListCatalogLoaded,
          weak_factory_.GetWeakPtr()));
}

void AdBlockFilterListCatalogProvider::LoadFilterListCatalog(
    base::OnceCallback<void(const std::string& catalog_json)> cb) {
  if (component_path_.empty()) {
    // If the path is not ready yet, don't run the callback. An update should be
    // pushed soon.
    return;
  }

  base::ThreadPool::PostTaskAndReplyWithResult(
      FROM_HERE, {base::MayBlock()},
      base::BindOnce(&brave_component_updater::GetDATFileAsString,
                     component_path_.AppendASCII(kListCatalogFile)),
      std::move(cb));
}

}  // namespace brave_shields
