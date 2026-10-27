// Copyright 2026 The Parra Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chrome/browser/parra/parra_startup.h"

#include <string>
#include <utility>
#include <vector>

#include "base/command_line.h"
#include "base/logging.h"
#include "chrome/browser/parra/environment_registry.h"
#include "chrome/browser/parra/parra_storage_partition_util.h"
#include "chrome/browser/parra/parra_switches.h"
#include "chrome/browser/profiles/profile.h"
#include "chrome/browser/ui/browser_window/public/browser_window_interface.h"
#include "chrome/browser/ui/browser_window/public/browser_window_interface_iterator.h"
#include "chrome/browser/ui/navigator/browser_navigator_params.h"
#include "chrome/browser/ui/tabs/tab_enums.h"
#include "chrome/browser/ui/tabs/tab_strip_model.h"
#include "content/public/browser/navigation_controller.h"
#include "content/public/browser/site_instance.h"
#include "content/public/browser/web_contents.h"
#include "ui/base/page_transition_types.h"
#include "url/gurl.h"

namespace parra {

namespace {

// Creates a WebContents whose initial SiteInstance is pinned to `config` (a
// non-default StoragePartition), so that every subsequent navigation in this
// tab stays inside that partition.
//
// `SiteInstance::CreateForFixedStoragePartition()` is the supported upstream
// API for this: it sets is_fixed_storage_partition=true on the BrowsingInstance,
// which makes NavigationRequest and RenderFrameHostManager propagate the
// partition across cross-site navigations, popups and frames.
//
// IMPORTANT: that API CHECKs `!partition_config.is_default()`. An environment
// that maps to the default partition (no dedicated NetworkContext) must go
// through the ordinary CreateParams path instead, otherwise the browser crashes
// at startup. See site_instance_impl.cc:253.
std::unique_ptr<content::WebContents> CreateEnvironmentWebContents(
    Profile* profile,
    const GURL& url,
    const content::StoragePartitionConfig& config) {
  if (config.is_default()) {
    // Default partition: no SiteInstance override, so Chromium picks the
    // regular one for the URL. Behaviour matches a stock tab.
    content::WebContents::CreateParams create_params(profile);
    create_params.initially_hidden = false;
    return content::WebContents::Create(create_params);
  }

  auto site_instance = content::SiteInstance::CreateForFixedStoragePartition(
      profile, url, config);

  content::WebContents::CreateParams create_params(profile,
                                                   std::move(site_instance));
  // Let the tab strip attach tab helpers via AddWebContents().
  create_params.initially_hidden = false;
  return content::WebContents::Create(create_params);
}

}  // namespace

bool InitializePocEnvironmentRegistry() {
  return EnvironmentRegistry::GetInstance()->InitializeFromCommandLine();
}

void MaybeOpenPocEnvironmentTabs() {
  EnvironmentRegistry* registry = EnvironmentRegistry::GetInstance();
  if (!registry->enabled())
    return;

  // The lifecycle PoC creates its own tabs with a controlled count per
  // environment. Letting both paths run would double the tab count and make
  // the switch experiment measure the wrong thing.
  if (base::CommandLine::ForCurrentProcess()->HasSwitch(
          switches::kParraPocLifecycle)) {
    LOG(INFO) << "parra: lifecycle PoC active; skipping default env tabs";
    return;
  }

  // Same for the visibility PoC: it creates a fixed 3x3 layout and would be
  // skewed by extra tabs from this path.
  if (base::CommandLine::ForCurrentProcess()->HasSwitch(
          switches::kParraPocVisibility)) {
    LOG(INFO) << "parra: visibility PoC active; skipping default env tabs";
    return;
  }

  // Session-restore runs must not create tabs here. The registry has to be
  // populated (so restored tabs can resolve their environment), but creating
  // an extra tab per environment on top of the restored ones silently inflates
  // the tab count and makes isolation measurements meaningless.
  if (base::CommandLine::ForCurrentProcess()->HasSwitch(
          switches::kParraPocNoInitialTabs)) {
    LOG(INFO) << "parra: no-initial-tabs set; registry only, no tabs created";
    return;
  }

  BrowserWindowInterface* browser =
      GetLastActiveBrowserWindowInterfaceWithAnyProfile();
  if (!browser) {
    LOG(ERROR) << "parra: no browser window available; skipping PoC tabs";
    return;
  }

  Profile* profile = browser->GetProfile();
  if (!profile) {
    LOG(ERROR) << "parra: browser has no profile; skipping PoC tabs";
    return;
  }

  const std::vector<EnvironmentRegistry::Environment>& environments =
      registry->environments();

  TabStripModel* tab_strip_model = browser->GetTabStripModel();
  if (!tab_strip_model) {
    LOG(ERROR) << "parra: browser has no tab strip; skipping PoC tabs";
    return;
  }

  // The startup tab is an about:blank placeholder. Remember it so it can be
  // closed once the first environment tab exists, keeping the PoC from leaving
  // a stray blank tab behind. Guarded because session restore or a command
  // line URL may have changed what the tab strip holds.
  content::WebContents* placeholder =
      tab_strip_model->count() == 1 ? tab_strip_model->GetWebContentsAt(0)
                                    : nullptr;

  for (size_t i = 0; i < environments.size(); ++i) {
    const EnvironmentRegistry::Environment& environment = environments[i];

    GURL url(environment.url);
    if (!url.is_valid()) {
      LOG(ERROR) << "parra: environment '" << environment.id
                 << "' has an invalid url '" << environment.url << "'";
      continue;
    }

    const content::StoragePartitionConfig config =
        GetStoragePartitionConfigForEnvironment(profile, environment.id);

    std::unique_ptr<content::WebContents> contents =
        CreateEnvironmentWebContents(profile, url, config);
    if (!contents) {
      LOG(ERROR) << "parra: failed to create WebContents for environment '"
                 << environment.id << "'";
      continue;
    }

    content::WebContents* raw_contents = contents.get();

    tab_strip_model->AddWebContents(std::move(contents), -1,
                                    ui::PAGE_TRANSITION_TYPED,
                                    AddTabTypes::ADD_ACTIVE);

    // Navigate explicitly: CreateForFixedStoragePartition() only pins the
    // *partition*, it does not start a load.
    content::NavigationController::LoadURLParams load_params(url);
    load_params.transition_type = ui::PAGE_TRANSITION_TYPED;
    raw_contents->GetController().LoadURLWithParams(load_params);

    LogParraNavigationDiagnostics(environment.id, environment.url);
    LOG(INFO) << "parra: opened environment '" << environment.id
              << "' partition=" << config << " url=" << url;
  }

  if (placeholder) {
    const int index = tab_strip_model->GetIndexOfWebContents(placeholder);
    if (index != TabStripModel::kNoTab)
      tab_strip_model->CloseWebContentsAt(index, TabCloseTypes::CLOSE_NONE);
  }
}

}  // namespace parra
