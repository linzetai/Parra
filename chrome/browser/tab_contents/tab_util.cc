// Copyright 2012 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chrome/browser/tab_contents/tab_util.h"

#include "chrome/browser/profiles/profile.h"
#include "chrome/browser/ui/webui/chrome_web_ui_controller_factory.h"
#include "content/public/browser/browser_url_handler.h"
#include "content/public/browser/site_instance.h"
#include "extensions/buildflags/buildflags.h"
#include "url/gurl.h"

#if BUILDFLAG(ENABLE_HOSTED_APPS)
#include "extensions/browser/extension_registry.h"
#endif

using content::SiteInstance;

namespace tab_util {

scoped_refptr<SiteInstance> GetSiteInstanceForNewTab(
    Profile* profile,
    GURL url,
    const content::StoragePartitionConfig& partition_config) {
  // Rewrite the |url| if necessary, to ensure that the SiteInstance is
  // associated with a |url| that will actually be loaded.  For example,
  // |url| set to chrome://newtab/ might actually result in a navigation to a
  // different URL like chrome://new-tab-page.
  content::BrowserURLHandler::GetInstance()->RewriteURLIfNecessary(&url,
                                                                   profile);

  // Parra PoC: when a non-default partition is requested, pin the SiteInstance
  // to it. CreateForFixedStoragePartition() also sets
  // is_fixed_storage_partition=true, so every later navigation in this tab
  // (including cross-site ones) stays in the same partition.
  const bool wants_custom_partition = !partition_config.is_default();

  // If |url| is a WebUI or extension, we set the SiteInstance up front so that
  // we don't end up with an extra process swap on the first navigation.
  if (ChromeWebUIControllerFactory::GetInstance()->UseWebUIForURL(profile, url))
    return wants_custom_partition
               ? SiteInstance::CreateForFixedStoragePartition(profile, url,
                                                             partition_config)
               : SiteInstance::CreateForURL(profile, url);

#if BUILDFLAG(ENABLE_HOSTED_APPS)
  if (extensions::ExtensionRegistry::Get(profile)
          ->enabled_extensions()
          .GetHostedAppByURL(url))
    return wants_custom_partition
               ? SiteInstance::CreateForFixedStoragePartition(profile, url,
                                                             partition_config)
               : SiteInstance::CreateForURL(profile, url);
#endif

  // We used to share the SiteInstance for same-site links opened in new tabs,
  // to leverage the in-memory cache and reduce process creation.  It now
  // appears that it is more useful to have such links open in a new process,
  // so we create new tabs in a new BrowsingInstance.
  // Create a new SiteInstance for the |url| unless it is not desirable.
  if (!SiteInstance::ShouldAssignSiteForURL(url))
    return nullptr;

  return wants_custom_partition
             ? SiteInstance::CreateForFixedStoragePartition(profile, url,
                                                           partition_config)
             : SiteInstance::CreateForURL(profile, url);
}

scoped_refptr<SiteInstance> GetSiteInstanceForNewTab(Profile* profile,
                                                    GURL url) {
  return GetSiteInstanceForNewTab(
      profile, std::move(url),
      content::StoragePartitionConfig::CreateDefault(profile));
}

}  // namespace tab_util
