// Copyright 2026 The Parra Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CHROME_BROWSER_PARRA_ENVIRONMENT_STORE_FACTORY_H_
#define CHROME_BROWSER_PARRA_ENVIRONMENT_STORE_FACTORY_H_

#include "base/no_destructor.h"
#include "chrome/browser/profiles/profile_keyed_service_factory.h"

class Profile;

namespace parra {

class EnvironmentStore;

// Creates one EnvironmentStore per Profile.
//
// WHY A KEYED SERVICE AND NOT A SINGLETON
//
// The definitions are a property of the profile's configuration, not of the
// process: two profiles may hold different definitions for the SAME id, and the
// StoragePartition a page lands in is already profile-scoped
// (BrowserContext::GetStoragePartition). A process-wide singleton can only hold
// one answer, so with two profiles it would either serve B's definitions to A or
// need an internal per-profile map -- which is the same thing as this factory but
// without Chromium's lifetime handling.
//
// OTR SELECTION: kOwnInstance, NOT kRedirectedToOriginal.
//
// The tempting choice is kRedirectedToOriginal (one shared store). It is WRONG
// here, and dangerously so:
//
//   The store derives the StoragePartitionConfig from its own `profile_`, and the
//   in_memory flag comes from that profile's IsOffTheRecord(). A redirected store
//   would hold the REGULAR profile, so a tab in an incognito window would receive
//   a PERSISTENT config -- and because OffTheRecordProfileImpl::GetPath() returns
//   the PARENT's path (off_the_record_profile_impl.cc:354-356) while the partition
//   path is a pure function of (domain, name), that config points at the very same
//   directory the regular profile uses. Incognito cookies would then be written
//   into the regular profile's Storage/ext/parra.
//
// kOwnInstance gives incognito its own store bound to the OTR profile, so
// IsOffTheRecord() is true and every partition it hands out is in-memory.
//
// The definitions still come from the same place: an OTR profile's PrefService is
// built by CreateIncognitoPrefServiceSyncable() over the original's pref service
// (off_the_record_profile_impl.cc:185-187), so reading prefs::kParraEnvironments
// returns the original profile's value while writes land in a session-only
// overlay. That is exactly the wanted behaviour: same environments, no disk
// effect.
//
// Writes are nevertheless REFUSED for OTR profiles (StoreResult::kReadOnly).
// A session-only write would look successful and vanish when the window closes,
// and the user's next regular window would disagree with what they just did.
class EnvironmentStoreFactory : public ProfileKeyedServiceFactory {
 public:
  static EnvironmentStoreFactory* GetInstance();

  static EnvironmentStore* GetForProfile(Profile* profile);

  EnvironmentStoreFactory(const EnvironmentStoreFactory&) = delete;
  EnvironmentStoreFactory& operator=(const EnvironmentStoreFactory&) = delete;

 private:
  friend base::NoDestructor<EnvironmentStoreFactory>;

  EnvironmentStoreFactory();
  ~EnvironmentStoreFactory() override;

  // BrowserContextKeyedServiceFactory:
  std::unique_ptr<KeyedService> BuildServiceInstanceForBrowserContext(
      content::BrowserContext* context) const override;
  bool ServiceIsCreatedWithBrowserContext() const override;
};

}  // namespace parra

#endif  // CHROME_BROWSER_PARRA_ENVIRONMENT_STORE_FACTORY_H_
