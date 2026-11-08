// Copyright 2026 The Parra Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CHROME_BROWSER_PARRA_ENVIRONMENT_SESSION_PERSISTENCE_H_
#define CHROME_BROWSER_PARRA_ENVIRONMENT_SESSION_PERSISTENCE_H_

#include <map>
#include <string>

#include "base/memory/weak_ptr.h"
#include "base/observer_list_types.h"
#include "chrome/browser/parra/environment_tag.h"

class BrowserWindowInterface;
class Profile;
namespace content {
class WebContents;
}

namespace parra {

// kEnvironmentSessionExtraDataKey -- the sessions::SessionTab::extra_data key
// under which a tab's environment id is stored -- is declared in
// chrome/browser/parra/environment_tag.h and defined in environment_tag.cc.
//
// It lives there rather than here because //chrome/browser/sessions must
// re-emit the key during a session-command rebuild
// (SessionService::BuildCommandsForTab), and sessions cannot depend on this
// UI-layer target without a GN cycle. The tag header is in the low :parra
// target, which both sides may depend on.
//
// It remains a PUBLIC on-disk contract; see the comment in environment_tag.h.

// Records tab -> environment ownership into the session file.
//
// WHY THIS EXISTS
//
// EnvironmentTag (environment_tag.h) holds ownership in memory only, on the
// WebContents. Nothing writes it to disk, so before this class the only code
// that persisted ownership at all was the visibility PoC, gated behind
// --parra-poc-visibility and run once at startup. A tab created the normal way
// (Ctrl+T, a link click, a restored session) therefore never recorded where it
// belonged: after a restart the id was absent, browser_tabrestore.cc took its
// "no environment recorded" path, and the tab came back in the DEFAULT
// partition.
//
// WHAT THIS IS NOT
//
// It is not a second list of tabs. The session service is already the single
// source of truth for which tabs exist and in what order; this class only adds
// one extra_data key to a tab that the session service already knows about. A
// tab that closes simply stops existing in both places, and there is nothing to
// keep in sync.
//
// It also never GUESSES. An explicit EnvironmentTag takes precedence; when it
// is absent, ownership is derived from the page's real StoragePartitionConfig.
// The result is cross-checked against that partition before it is persisted.
// Ownership is never inferred from the window's active environment or tab
// visibility: both can change independently of where the page's data lives.
class EnvironmentSessionPersistence {
 public:
  // Records `contents`' environment for `window_id`.
  //
  // Safe to call repeatedly for the same tab: the session file keeps the last
  // value written for a key. Returns the id that was recorded (empty when
  // nothing was recorded, e.g. the tab has no explicit tag or has no session
  // id).
  static std::string RecordTabEnvironment(Profile* profile,
                                          BrowserWindowInterface* browser,
                                          content::WebContents* contents);

  // Records the window's active environment so a restart can put the user back
  // in the environment they were looking at.
  //
  // A SEPARATE concern from tab ownership, and deliberately a separate key: the
  // active environment is a view preference that MAY fall back to `personal`,
  // while a tab's partition may NEVER be changed by that fallback. Sharing one
  // key would make it possible to confuse the two.
  static void RecordWindowActiveEnvironment(Profile* profile,
                                            BrowserWindowInterface* browser,
                                            const std::string& environment_id);

  // Reads the recorded active environment straight out of a restored window's
  // SessionWindow::extra_data.
  //
  // This is the restore-side reader: during restore there is no live window to
  // ask yet (the value is needed WHILE building it), and SessionService exposes
  // no reader for window extra_data. Called with the map that
  // CreateRestoredBrowser already receives.
  static std::string GetRecordedActiveEnvironmentFromExtraData(
      const std::map<std::string, std::string>& window_extra_data);

  // Installs the EnvironmentTag::Set hook so ownership is persisted the moment
  // it is decided, instead of only from the tab-insertion path.
  //
  // Called once at browser startup. See the implementation for why insertion
  // alone is not enough (a session rebuild clears the pending command queue).
  static void InstallTagObserver();
};

}  // namespace parra

#endif  // CHROME_BROWSER_PARRA_ENVIRONMENT_SESSION_PERSISTENCE_H_
