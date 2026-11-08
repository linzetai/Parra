// Copyright 2026 The Parra Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chrome/browser/parra/environment_session_persistence.h"

#include <string>

#include "base/logging.h"
#include "chrome/browser/parra/environment_tag.h"
#include "chrome/browser/parra/parra_storage_partition_util.h"
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

// kEnvironmentSessionExtraDataKey is defined in environment_tag.cc (the low
// parra layer) so that //chrome/browser/sessions can re-emit it during a
// command rebuild without depending on this UI-layer target.

// Key for the window's active environment. Separate from the tab key ON
// PURPOSE: the active environment may fall back to `personal`, a tab's
// partition may not.
constexpr char kWindowActiveEnvironmentExtraDataKey[] =
    "parra.window_active_environment";

// Value written under kEnvironmentSessionOwnerKey. The string itself carries no
// meaning beyond "Parra owns this tab"; a version number is included so a
// future format change can be detected instead of misread as a valid marker.
constexpr char kEnvironmentSessionOwnerValue[] = "1";

namespace {

// The environment a page's STORAGE actually lives in, derived from its
// committed partition rather than from any recorded claim.
//
// Returns an empty string when the page has no site assigned yet (about:blank):
// in that case there is no partition to observe, and the caller falls back to
// the tag, which is the deliberate claim recorded at creation time.
std::string EnvironmentFromActualPartition(content::WebContents* contents) {
  content::RenderFrameHost* main_frame = contents->GetPrimaryMainFrame();
  if (!main_frame) {
    return std::string();
  }

  const scoped_refptr<content::SiteInstance> site_instance =
      main_frame->GetSiteInstance();
  if (!site_instance) {
    return std::string();
  }

  return GetEnvironmentIdFromPartitionConfig(
      site_instance->GetSecurityPrincipal().GetStoragePartitionConfig());
}

}  // namespace

// static
std::string EnvironmentSessionPersistence::RecordTabEnvironment(
    Profile* profile,
    BrowserWindowInterface* browser,
    content::WebContents* contents) {
  if (!profile || !browser || !contents) {
    return std::string();
  }

  // Preview policy: the management Browser-level tab is reopened from the menu,
  // not restored. Mark its existing session entry as Parra-owned without an
  // environment id. The restore guard refuses this shape before creating a
  // WebContents, so it can never reappear as a Personal ordinary tab.
  if (BrowserLevelTab::Is(contents)) {
    const SessionID window_id = browser->GetSessionID();
    const SessionID tab_id = sessions::SessionTabHelper::IdForTab(contents);
    SessionService* service = SessionServiceFactory::GetForProfile(profile);
    if (window_id.is_valid() && tab_id.is_valid() && service) {
      service->AddTabExtraData(window_id, tab_id, kEnvironmentSessionOwnerKey,
                               kEnvironmentSessionOwnerValue);
      LOG(INFO) << "parra-session: marked Browser-level tab " << tab_id.id()
                << " non-restorable";
    }
    return std::string();
  }

  // Prefer the explicit tag; fall back to the StoragePartition.
  //
  // The tag is the intended record, but it is not the only truth: a tab created
  // directly inside `parra/<id>` without a tag has no tag yet is unambiguously
  // in that environment. Recording NOTHING for it (the old behaviour) meant the
  // tab's environment was not persisted at all, so a restart dropped the tab:
  // the restore path treats an untagged/unknown-environment tab as foreign and
  // skips it. Observed with the M1 build, where the startup path adds tabs
  // through TabStripModel::AddWebContents() and never writes a tag:
  //
  //   parra-session: skip tab (no explicit environment tag)   <-- three times
  //   ... restart ...
  //   parra-ui: SNAPSHOT (init-from-model) model_count=0      <-- every tab
  //   gone
  //
  // The partition IS the isolation decision, so it is a valid source for "which
  // environment is this tab in". Personal keeps its old behaviour: an untagged
  // tab in the DEFAULT partition records nothing, because upstream restore
  // handles that case correctly and writing 'personal' would fabricate a
  // decision.
  std::string environment_id;
  if (EnvironmentTag::HasTag(contents)) {
    environment_id = EnvironmentTag::Get(contents);
  } else {
    environment_id = EnvironmentFromActualPartition(contents);
    if (environment_id == kDefaultEnvironmentId) {
      environment_id
          .clear();  // untagged + default partition: upstream behaviour.
    }
  }

  if (environment_id.empty()) {
    LOG(INFO) << "parra-session: skip tab (no tag and no named partition)";
    return std::string();
  }

  const SessionID window_id = browser->GetSessionID();
  if (!window_id.is_valid()) {
    LOG(ERROR) << "parra-session: window has no session id; not recording";
    return std::string();
  }

  const SessionID tab_id = sessions::SessionTabHelper::IdForTab(contents);
  if (!tab_id.is_valid()) {
    // Happens for a WebContents that was never adopted by a tab strip (e.g. a
    // provisional pre-render host). Nothing to key the record on yet; the tab
    // will be recorded once it is adopted.
    LOG(INFO) << "parra-session: tab has no session id yet; deferring";
    return std::string();
  }

  SessionService* service = SessionServiceFactory::GetForProfile(profile);
  if (!service) {
    LOG(ERROR) << "parra-session: no SessionService for this profile";
    return std::string();
  }

  // Cross-check the claim against the page's real partition. A disagreement
  // means the record and the storage have diverged, and the STORAGE is what a
  // restart must reproduce -- restoring the record would put the tab in a
  // partition its data does not live in, i.e. an empty-looking environment with
  // the old cookies unreachable.
  //
  // Reachable for a PARTITION-derived id too (the page committed into a
  // different partition than the one its SiteInstance was pinned to), so this
  // check runs for both sources rather than only for tagged tabs.
  const std::string actual = EnvironmentFromActualPartition(contents);
  std::string recorded = environment_id;
  if (!actual.empty() && actual != environment_id) {
    LOG(ERROR) << "parra-session: environment mismatch for tab_id="
               << tab_id.id() << ": recorded='" << environment_id
               << "' but partition says='" << actual
               << "'. Recording the PARTITION (that is where the data is).";
    recorded = actual;
  }

  // The session file keeps the last value written for a key, so calling this
  // again for the same tab is an update, not a duplicate.
  service->AddTabExtraData(window_id, tab_id, kEnvironmentSessionExtraDataKey,
                           recorded);

  // Also mark the tab as Parra-owned. Written on EVERY record (including
  // `personal`), because its purpose is to answer, at restore time, "did Parra
  // ever decide an environment for this tab?". Without it, a tab whose id
  // record went missing is indistinguishable from a plain Chromium history tab,
  // and the only two options would be "restore as Personal" (silent isolation
  // loss) or "refuse every untagged tab" (breaks normal users' sessions). See
  // the header.
  service->AddTabExtraData(window_id, tab_id, kEnvironmentSessionOwnerKey,
                           kEnvironmentSessionOwnerValue);

  LOG(INFO) << "parra-session: recorded tab_id=" << tab_id.id() << " env='"
            << recorded << "'";
  return recorded;
}

// static
void EnvironmentSessionPersistence::RecordWindowActiveEnvironment(
    Profile* profile,
    BrowserWindowInterface* browser,
    const std::string& environment_id) {
  if (!profile || !browser || environment_id.empty()) {
    return;
  }

  const SessionID window_id = browser->GetSessionID();
  if (!window_id.is_valid()) {
    return;
  }

  SessionService* service = SessionServiceFactory::GetForProfile(profile);
  if (!service) {
    return;
  }

  service->AddWindowExtraData(window_id, kWindowActiveEnvironmentExtraDataKey,
                              environment_id);
  LOG(INFO) << "parra-session: recorded window active env='" << environment_id
            << "'";
}

// static
std::string
EnvironmentSessionPersistence::GetRecordedActiveEnvironmentFromExtraData(
    const std::map<std::string, std::string>& window_extra_data) {
  const auto it = window_extra_data.find(kWindowActiveEnvironmentExtraDataKey);
  if (it == window_extra_data.end()) {
    return std::string();
  }
  return it->second;
}

// static
void EnvironmentSessionPersistence::InstallTagObserver() {
  // Persist at the moment ownership is decided, not at the moment the tab is
  // inserted.
  //
  // Browser::OnTabStripModelChanged(kInserted) also records, and that path is
  // kept because it is the first point where the tab has a session id. But it
  // is NOT sufficient on its own: SessionService::ScheduleResetCommands()
  // clears the pending command queue, so a record queued a few milliseconds
  // earlier is dropped and never re-queued (the rebuild reads status from the
  // live browser, and at that instant the tab may still be an untagged
  // about:blank).
  //
  // Observed in a four-tab run: two records were "queued" and then two
  // pending_reset=1 saves flushed a queue that no longer contained them.
  //
  // Announcing from EnvironmentTag::Set() removes the race: ownership is
  // written as soon as it exists, and SessionService::BuildCommandsForTab()
  // re-emits it on any later rebuild.
  parra::EnvironmentTag::SetObserver(base::BindRepeating(
      [](content::WebContents* contents, const std::string& environment_id) {
        // No window here: the tag is set before the tab is inserted anywhere.
        // The record is therefore keyed by the window that owns the contents'
        // current session id, resolved lazily inside the browser path. If there
        // is no window yet, the kInserted write covers it once the tab lands.
        BrowserWindowInterface* window = nullptr;
        for (BrowserWindowInterface* candidate :
             GetAllBrowserWindowInterfaces()) {
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
          VLOG(1)
              << "parra-session: tag set before insertion; the insertion path "
                 "records it";
          return;
        }
        RecordTabEnvironment(window->GetProfile(), window, contents);
      }));
}

}  // namespace parra
