// Copyright 2026 The Parra Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CHROME_BROWSER_PARRA_ENVIRONMENT_TAG_H_
#define CHROME_BROWSER_PARRA_ENVIRONMENT_TAG_H_

#include <string>

#include "base/functional/callback.h"

namespace content {
class WebContents;
}

namespace parra {

// The environment a tab belongs to when it carries no explicit tag. Matches what
// a stock Chromium tab does.
inline constexpr char kDefaultEnvironmentId[] = "personal";

// Environment ownership of a WebContents.
//
// This lives in the low layer (//chrome/browser/parra:parra) rather than next to
// EnvironmentIndexMapper so that code which can only see content/ -- most
// importantly session restore in chrome/browser/ui/browser_tabrestore.cc -- can
// tag a tab without depending on the UI tab-strip layer. EnvironmentIndexMapper
// is a pure delegator over this.
//
// Why the tag lives on the WebContents and not on the tab's position: the
// environment is a property of the page that owns the StoragePartition. Storing
// it on the page means reordering, inserting or closing other environments' tabs
// can never silently reassign a tab to a different environment.

// The sessions::SessionTab::extra_data key under which a tab's environment id is
// persisted.
//
// Defined HERE (the low parra layer, which has no UI dependencies) rather than next
// to the persistence helper, because //chrome/browser/sessions also needs it: it
// re-emits this key during a session-command REBUILD (see
// SessionService::BuildCommandsForTab). //chrome/browser/sessions cannot depend on
// //chrome/browser/parra:parra_tabs (that target depends on the UI layer and on
// sessions itself, so the edge would be a cycle), but it can depend on :parra.
//
// This is a PUBLIC on-disk contract: it is the string written into the session file
// and read back after a restart. Changing it silently orphans every tab recorded by
// an older build, and those tabs would restore as "no environment recorded" -- i.e.
// the default partition.
extern const char kEnvironmentSessionExtraDataKey[];

// A second extra_data key that marks a tab as PARRA-OWNED, independent of the
// environment id.
//
// WHY IT IS NEEDED -- docs/parra/68 task 3. On restore, an ABSENT
// kEnvironmentSessionExtraDataKey is ambiguous, and the four cases must not be
// conflated:
//
//   (a) a plain Chromium history tab that never had an environment
//       -> restore on the default path, unchanged. This is the overwhelming
//          majority of real users' sessions, so it must NOT be refused;
//   (b) a tab that WAS in a Parra environment but whose id record is missing
//       -> its partition is unknown. Restoring it into Personal would put the page
//          where its cookies do not live, which is the leak this project prevents;
//   (c) a tab explicitly recorded as `personal`
//       -> known to be Personal, restore normally;
//   (d) a tab whose environment was deleted / disabled / corrupt
//       -> fail closed as today (deleted => refuse, disabled => restore but not
//          activatable).
//
// Cases (a) and (b) are indistinguishable from the id alone. This marker resolves
// that: it is written whenever Parra decided an environment for the tab (including
// `personal`), so an id that is missing WHILE the marker is present is case (b) and
// can be refused, while a missing id with no marker is case (a) and keeps working.
//
// Deliberately a separate key rather than a sentinel value in the id: keeping the
// id space clean ("a real environment id, or nothing") means the reader does not
// have to special-case magic strings.
extern const char kEnvironmentSessionOwnerKey[];

class EnvironmentTag {
 public:
  EnvironmentTag() = delete;

  // Called whenever a tab's environment is settled (EnvironmentTag::Set).
  //
  // WHY A HOOK: the tag is written on the navigation path, which runs in
  // //chrome/browser/ui/navigator, before the tab is inserted into the strip. The
  // session record for the tab, however, is written from
  // Browser::OnTabStripModelChanged() -- and that record can be discarded if the
  // session service rebuilds its command queue in between (see
  // SessionService::ScheduleResetCommands(), which calls ClearPendingCommands()).
  //
  // Rather than depend on the ordering between those two events, this layer simply
  // announces "ownership for this tab is now X". The persistence side installs a
  // callback and writes the record at that point, so a later rebuild cannot lose it:
  // a rebuild READS the tag (SessionService::BuildCommandsForTab re-emits it) and a
  // fresh Set re-announces it.
  //
  // Still no UI dependency here: the callback takes a WebContents, and installing it
  // is the persistence layer's job.
  using Observer = base::RepeatingCallback<void(content::WebContents* contents,
                                                const std::string& environment_id)>;

  // Installs the observer (last one wins). Passing an empty callback removes it.
  static void SetObserver(Observer observer);

  // The only writer. `environment_id` must not be empty.
  static void Set(content::WebContents* contents,
                  const std::string& environment_id);

  // Reads the tag without needing a TabStripModel. Returns
  // kDefaultEnvironmentId for untagged or null contents, which is the correct
  // answer for a restored upstream tab.
  static std::string Get(const content::WebContents* contents);

  // True when somebody explicitly wrote a tag. This exists because Get()
  // deliberately conflates two very different situations:
  //
  //   * an upstream Chromium tab that was never meant to carry an environment
  //     (a native default tab -- "Personal" is the right answer), and
  //   * a tab created through a Parra path that forgot to bind its environment.
  //
  // The first is legitimate, the second is a bug that would silently drop a page
  // into Personal while its StoragePartition says otherwise. Callers that need to
  // tell them apart (see EnvironmentTabRouter) must ask this, not compare Get()
  // against kDefaultEnvironmentId.
  static bool HasTag(const content::WebContents* contents);
};

}  // namespace parra

#endif  // CHROME_BROWSER_PARRA_ENVIRONMENT_TAG_H_
