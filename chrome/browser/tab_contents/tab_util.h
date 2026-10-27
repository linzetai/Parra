// Copyright 2012 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CHROME_BROWSER_TAB_CONTENTS_TAB_UTIL_H_
#define CHROME_BROWSER_TAB_CONTENTS_TAB_UTIL_H_

#include "content/public/browser/site_instance.h"
#include "content/public/browser/storage_partition_config.h"
#include "url/gurl.h"

class Profile;

namespace tab_util {

// Returns a new SiteInstance for WebUI and app URLs. Returns NULL otherwise.
scoped_refptr<content::SiteInstance> GetSiteInstanceForNewTab(Profile* profile,
                                                            GURL url);

// As above, but when `partition_config` is non-empty and non-default the
// returned SiteInstance is pinned to that StoragePartition for the lifetime of
// its BrowsingInstance (is_fixed_storage_partition). This is what binds a new
// tab to a Parra environment.
//
// Callers that do not pass a config get exactly the upstream behavior.
scoped_refptr<content::SiteInstance> GetSiteInstanceForNewTab(
    Profile* profile,
    GURL url,
    const content::StoragePartitionConfig& partition_config);
}  // namespace tab_util

#endif  // CHROME_BROWSER_TAB_CONTENTS_TAB_UTIL_H_
