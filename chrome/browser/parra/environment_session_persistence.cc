// Copyright 2026 The Parra Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chrome/browser/parra/environment_session_persistence.h"

#include <string>

#include "base/logging.h"
#include "base/strings/strcat.h"
#include "chrome/browser/parra/environment_tag.h"
#include "chrome/browser/parra/parra_storage_partition_util.h"
#include "chrome/browser/parra/window_environment_state.h"
#include "chrome/browser/profiles/profile.h"
#include "chrome/browser/sessions/session_service.h"
#include "chrome/browser/sessions/session_service_factory.h"
#include "chrome/browser/ui/browser_window/public/browser_window_interface.h"
#include "chrome/browser/ui/browser_window/public/browser_window_interface_iterator.h"
#include "chrome/browser/ui/tabs/tab_strip_model.h"
#include "components/sessions/content/session_tab_helper.h"
#include "content/public/browser/render_frame_host.h"
#include "content/public/browser/security_principal.h"
#include "content/public/browser/site_instance.h"
#include "content/public/browser/web_contents.h"

namespace parra {

// kEnvironmentSessionExtraDataKey is defined in environment_tag.cc (the low parra
// layer) so that //chrome/browser/sessions can re-emit it during a command rebuild
// without depending on this UI-layer target.

// Key for the window's active environment. Separate from the tab key ON PURPOSE:
// the active environment may fall back to `personal`, a tab's partition may not.
constexpr char kWindowActiveEnvironmentExtraDataKey[] =
    "parra.window_active_environment";

// Value written under kEnvironmentSessionOwnerKey. The string itself carries no
// meaning beyond "Parra owns this tab"; a version number is included so a future
// format change can be detected instead of misread as a valid marker.
constexpr char kEnvironmentSessionOwnerValue[] = "1";

namespace {

// The environment a page's STORAGE actually lives in, derived from its committed
// partition rather than from any recorded claim.
//
// Returns an empty string when the page has no site assigned yet (about:blank):
// in that case there is no partition to observe, and the caller falls back to the
// tag, which is the deliberate claim recorded at creation time.
std::string EnvironmentFromActualPartition(content::WebContents* contents) {
  content::RenderFrameHost* main_frame = contents->GetPrimaryMainFrame();
  if (!main_frame)
    return std::string();

  const scoped_refptr<content::SiteInstance> site_instance =
      main_frame->GetSiteInstance();
  if (!site_instance)
    return std::string();

  return GetEnvironmentIdFromPartitionConfig(
      site_instance->GetSecurityPrincipal().GetStoragePartitionConfig());
}

}  // namespace

// static
std::string EnvironmentSessionPersistence::RecordTabEnvironment(
    Profile* profile,
    BrowserWindowInterface* browser,
    content::WebContents* contents) {
  if (!profile || !browser || !contents)
    return std::string();

  // No explicit tag means nobody ever decided where this tab belongs. Recording
  // `personal` here would be worse than recording nothing: an empty id restores
  // through the upstream path (default partition, no claim made), whereas a
  // written 'personal' would look like a deliberate decision that later code
  // could act on.
  if (!EnvironmentTag::HasTag(contents)) {
    LOG(INFO) << "parra-session: skip tab (no explicit environment tag)";
    return std::string();
  }

  const std::string tagged = EnvironmentTag::Get(contents);
  if (tagged.empty())
    return std::string();

  const SessionID window_id = browser->GetSessionID();
  if (!window_id.is_valid()) {
    LOG(ERROR) << "parra-session: window has no session id; not recording";
    return std::string();
  }

  const SessionID tab_id = sessions::SessionTabHelper::IdForTab(contents);
  if (!tab_id.is_valid()) {
    // Happens for a WebContents that was never adopted by a tab strip (e.g. a
    // provisional pre-render host). Nothing to key the record on yet; the tab will
    // be recorded once it is adopted.
    LOG(INFO) << "parra-session: tab has no session id yet; deferring";
    return std::string();
  }

  SessionService* service = SessionServiceFactory::GetForProfile(profile);
  if (!service) {
    LOG(ERROR) << "parra-session: no SessionService for this profile";
    return std::string();
  }

  // Cross-check the claim against the page's real partition. A disagreement means
  // the tag and the storage have diverged, and the STORAGE is what a restart must
  // reproduce -- restoring the tag would put the tab in a partition its data does
  // not live in, i.e. an empty-looking environment with the old cookies
  // unreachable.
  const std::string actual = EnvironmentFromActualPartition(contents);
  std::string recorded = tagged;
  if (!actual.empty() && actual != tagged) {
    LOG(ERROR) << "parra-session: environment mismatch for tab_id=" << tab_id.id()
               << ": tag='" << tagged << "' but partition says='" << actual
               << "'. Recording the PARTITION (that is where the data is).";
    recorded = actual;
  }

  // The session file keeps the last value written for a key, so calling this again
  // for the same tab is an update, not a duplicate.
  service->AddTabExtraData(window_id, tab_id, kEnvironmentSessionExtraDataKey,
                           recorded);

  // Also mark the tab as Parra-owned. Written on EVERY record (including
  // `personal`), because its purpose is to answer, at restore time, "did Parra ever
  // decide an environment for this tab?". Without it, a tab whose id record went
  // missing is indistinguishable from a plain Chromium history tab, and the only two
  // options would be "restore as Personal" (silent isolation loss) or "refuse every
  // untagged tab" (breaks normal users' sessions). See the header.
  service->AddTabExtraData(window_id, tab_id, kEnvironmentSessionOwnerKey,
                           kEnvironmentSessionOwnerValue);

  LOG(INFO) << "parra-session: recorded tab_id=" << tab_id.id() << " env='"
            << recorded << "'";
  return recorded;
}

// static
int EnvironmentSessionPersistence::RecordAllTabEnvironments(
    BrowserWindowInterface* browser) {
  if (!browser)
    return 0;

  Profile* profile = browser->GetProfile();
  TabStripModel* model = browser->GetTabStripModel();
  if (!profile || !model)
    return 0;

  int recorded = 0;
  for (int i = 0; i < model->count(); ++i) {
    content::WebContents* contents = model->GetWebContentsAt(i);
    if (contents && !RecordTabEnvironment(profile, browser, contents).empty())
      ++recorded;
  }

  // Also record the window's active environment. Called together with the tabs
  // because a window's active environment must be recoverable even if the user
  // never switches environments: with no recorded value, restore cannot tell
  // "the window was on test-a" from "nothing was ever recorded", and would fall
  // back to Personal for a user who was working in test-a.
  if (WindowEnvironmentState* state = WindowEnvironmentState::GetFor(browser)) {
    RecordWindowActiveEnvironment(profile, browser, state->active_environment());
  }

  return recorded;
}

// static
void EnvironmentSessionPersistence::RecordWindowActiveEnvironment(
    Profile* profile,
    BrowserWindowInterface* browser,
    const std::string& environment_id) {
  if (!profile || !browser || environment_id.empty())
    return;

  const SessionID window_id = browser->GetSessionID();
  if (!window_id.is_valid())
    return;

  SessionService* service = SessionServiceFactory::GetForProfile(profile);
  if (!service)
    return;

  service->AddWindowExtraData(window_id, kWindowActiveEnvironmentExtraDataKey,
                              environment_id);
  LOG(INFO) << "parra-session: recorded window active env='" << environment_id
            << "'";
}

// static
std::string EnvironmentSessionPersistence::GetRecordedWindowActiveEnvironment(
    Profile* profile,
    BrowserWindowInterface* browser) {
  // The session service exposes no reader for window extra_data: only the restore
  // path reads it, out of the SessionWindow it just parsed. So the live window's
  // active environment is answered from WindowEnvironmentState (the in-memory
  // authority) and this recorded value is only consulted during restore, where the
  // caller already holds the SessionWindow. Kept as a stub that reports "nothing
  // recorded" rather than inventing a reader that would read the wrong thing.
  return std::string();
}

// static
std::string EnvironmentSessionPersistence::GetRecordedActiveEnvironmentFromExtraData(
    const std::map<std::string, std::string>& window_extra_data) {
  const auto it = window_extra_data.find(kWindowActiveEnvironmentExtraDataKey);
  if (it == window_extra_data.end())
    return std::string();
  return it->second;
}

// static
void EnvironmentSessionPersistence::InstallTagObserver() {
  // Persist at the moment ownership is decided, not at the moment the tab is
  // inserted.
  //
  // Browser::OnTabStripModelChanged(kInserted) also records, and that path is kept
  // because it is the first point where the tab has a session id. But it is NOT
  // sufficient on its own: SessionService::ScheduleResetCommands() clears the
  // pending command queue, so a record queued a few milliseconds earlier is dropped
  // and never re-queued (the rebuild reads status from the live browser, and at that
  // instant the tab may still be an untagged about:blank).
  //
  // Observed in a four-tab run: two records were "queued" and then two
  // pending_reset=1 saves flushed a queue that no longer contained them.
  //
  // Announcing from EnvironmentTag::Set() removes the race: ownership is written as
  // soon as it exists, and SessionService::BuildCommandsForTab() re-emits it on any
  // later rebuild.
  parra::EnvironmentTag::SetObserver(base::BindRepeating(
      [](content::WebContents* contents, const std::string& environment_id) {
        // No window here: the tag is set before the tab is inserted anywhere. The
        // record is therefore keyed by the window that owns the contents' current
        // session id, resolved lazily inside the browser path. If there is no
        // window yet, the kInserted write covers it once the tab lands.
        BrowserWindowInterface* window = nullptr;
        for (BrowserWindowInterface* candidate : GetAllBrowserWindowInterfaces()) {
          if (candidate->GetTabStripModel() &&
              candidate->GetTabStripModel()->GetIndexOfWebContents(contents) !=
                  TabStripModel::kNoTab) {
            window = candidate;
            break;
          }
        }
        if (!window) {
          // Not inserted yet -- the normal case (the tag is applied on the
          // navigation path, before insertion). The kInserted path records it.
          VLOG(1) << "parra-session: tag set before insertion; the insertion path "
                     "records it";
          return;
        }
        RecordTabEnvironment(window->GetProfile(), window, contents);
      }));
}

}  // namespace parra
