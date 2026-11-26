// Copyright (c) 2026 The Brave Authors. All rights reserved.
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this file,
// You can obtain one at https://mozilla.org/MPL/2.0/.

#ifndef BRAVE_BROWSER_CONTAINERS_KO_CONTAINER_PROXY_SERVICE_FACTORY_H_
#define BRAVE_BROWSER_CONTAINERS_KO_CONTAINER_PROXY_SERVICE_FACTORY_H_

#include <memory>

#include "base/no_destructor.h"
#include "chrome/browser/profiles/profile_keyed_service_factory.h"

class Profile;

namespace containers {
class KoContainerProxyService;
}  // namespace containers

// K-O: per-profile owner of container proxy state.
class KoContainerProxyServiceFactory : public ProfileKeyedServiceFactory {
 public:
  static KoContainerProxyServiceFactory* GetInstance();
  static containers::KoContainerProxyService* GetForProfile(Profile* profile);

  KoContainerProxyServiceFactory(const KoContainerProxyServiceFactory&) =
      delete;
  KoContainerProxyServiceFactory& operator=(
      const KoContainerProxyServiceFactory&) = delete;

 private:
  friend base::NoDestructor<KoContainerProxyServiceFactory>;

  KoContainerProxyServiceFactory();
  ~KoContainerProxyServiceFactory() override;

  // ProfileKeyedServiceFactory:
  std::unique_ptr<KeyedService> BuildServiceInstanceForBrowserContext(
      content::BrowserContext* context) const override;
  bool ServiceIsCreatedWithBrowserContext() const override;
};

#endif  // BRAVE_BROWSER_CONTAINERS_KO_CONTAINER_PROXY_SERVICE_FACTORY_H_
