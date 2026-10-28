// Copyright 2026 The Parra Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chrome/browser/parra/environment_store_factory.h"

#include <memory>

#include "chrome/browser/parra/environment_store.h"
#include "chrome/browser/profiles/profile.h"
#include "chrome/browser/profiles/profile_selections.h"

namespace parra {

// static
EnvironmentStoreFactory* EnvironmentStoreFactory::GetInstance() {
  static base::NoDestructor<EnvironmentStoreFactory> instance;
  return instance.get();
}

// static
EnvironmentStore* EnvironmentStoreFactory::GetForProfile(Profile* profile) {
  if (!profile) {
    return nullptr;
  }
  return static_cast<EnvironmentStore*>(
      GetInstance()->GetServiceForBrowserContext(profile, /*create=*/true));
}

EnvironmentStoreFactory::EnvironmentStoreFactory()
    : ProfileKeyedServiceFactory(
          "ParraEnvironmentStore",
          // kOwnInstance: incognito gets its OWN store bound to the OTR profile,
          // so its partition configs are in-memory. A redirected store would hold
          // the regular profile and hand out persistent configs pointing at the
          // regular profile's directory. See the header for the full reasoning.
          ProfileSelections::Builder()
              .WithRegular(ProfileSelection::kOwnInstance)
              .WithGuest(ProfileSelection::kOwnInstance)
              .WithSystem(ProfileSelection::kNone)
              .WithAshInternals(ProfileSelection::kNone)
              .Build()) {}

EnvironmentStoreFactory::~EnvironmentStoreFactory() = default;

std::unique_ptr<KeyedService>
EnvironmentStoreFactory::BuildServiceInstanceForBrowserContext(
    content::BrowserContext* context) const {
  Profile* profile = Profile::FromBrowserContext(context);
  if (!profile) {
    return nullptr;
  }
  return std::make_unique<EnvironmentStore>(profile);
}

bool EnvironmentStoreFactory::ServiceIsCreatedWithBrowserContext() const {
  // Eager. The first StoragePartition of a profile asks for host mapping rules
  // from ContentBrowserClient::ConfigureNetworkContextParams(), which is a
  // synchronous call with no way to wait for a lazily-created service. A store
  // that did not exist yet would answer "no rules", and the page would resolve to
  // real DNS instead of the intended backend -- a silent routing failure with no
  // error anywhere.
  return true;
}

}  // namespace parra
