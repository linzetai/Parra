// Copyright 2026 The Parra Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CHROME_BROWSER_PARRA_WINDOW_ENVIRONMENT_STATE_H_
#define CHROME_BROWSER_PARRA_WINDOW_ENVIRONMENT_STATE_H_

#include <optional>
#include <string>
#include <vector>

#include "base/memory/raw_ptr.h"
#include "base/callback_list.h"
#include "base/types/pass_key.h"
#include "chrome/browser/parra/environment_tag.h"
#include "components/tabs/public/tab_interface.h"
#include "ui/base/unowned_user_data/scoped_unowned_user_data.h"

namespace content {
class WebContents;
}

class BrowserWindowInterface;
class TabStripModel;

namespace parra {

// Per-window environment state: which environment this window is showing, and
// which tab was last active in each environment.
//
// WHY PER-WINDOW AND NOT PER-PROCESS
//
// Parra is meant to be a daily-driver browser, so two windows must be able to sit
// on different environments at the same time (window A on Personal, window B on
// Test-A) without either affecting the other. A process-global value cannot
// express that: the moment window B switches, window A's tab strip would repaint
// with B's environment. This class replaces the earlier process-global
// ActiveEnvironment for exactly that reason.
//
// The interesting part is not the storage, it is finding the right SCOPE for
// each piece of state:
//
//   Environment definitions   -> Profile (EnvironmentRegistry). Same environments
//                                in every window of a profile; they are a
//                                property of the profile's configuration.
//   StoragePartition          -> Profile. Owned by the SecurityPrincipal of the
//                                committed document; a window cannot have its own.
//   Active environment        -> THIS window. It is a view state: "what is this
//                                window looking at right now".
//   Last active tab per env   -> THIS window, because it names a TabHandle that
//                                only exists in this window's TabStripModel.
//
// STORAGE: BrowserWindowInterface::GetUnownedUserDataHost()
//
// BrowserWindowInterface does not inherit base::SupportsUserData, so there is no
// SetUserData() to use. It does expose ui::UnownedUserDataHost, and its own header
// says feature-specific state belongs there, so that is what this uses for LOOKUP:
//
//   - the host is keyed by type, so there is exactly one instance per window -- a
//     second writer overwrites rather than creating a second truth;
//   - lookup works from the window, and from a TabStripModel via its delegate.
//
// OWNERSHIP: the BrowserTabStripController, NOT this object
//
// UnownedUserData does not own (the name is literal -- ScopedUnownedUserData only
// Erases on destruction). An earlier version of this class therefore made itself
// self-deleting from the BrowserDidClose notification. That ordering is unsafe:
//
//   Browser::~Browser()
//     -> !IsDeleteScheduled() -> UnloadController::OnWindowClosing()
//          -> TabStripModel::CloseAllTabs()
//               -> Parra observers still running
//          -> OnWindowCloseComplete() -> BrowserDidClose -> delete state
//     -> window_.reset()
//     -> features_.reset()
//
// The notification can therefore fire while tab closing is still in progress, and
// any Parra callback that runs afterwards would touch a deleted object. It also
// paints a picture where the state outlives nothing in particular: whether it is
// freed at all depends on whether a close notification was ever emitted.
//
// So ownership is explicit and local instead: BrowserTabStripController owns the
// object (one controller per window, destroyed with the window's tab strip) and
// installs it into the host purely so other components can find it. This is the
// same shape Chromium's own UnloadController uses: it registers itself in the
// window's UnownedUserDataHost via a ScopedUnownedUserData member but is owned by
// BrowserWindowFeatures, not by the host.
//
// The lifetime consequence: the state lives exactly as long as the window's tab
// strip controller. It cannot be resurrected after teardown (GetOrCreate() only
// runs from InitFromModel()), and it cannot be freed out from under a running tab
// close.
class WindowEnvironmentState {
 public:
  DECLARE_USER_DATA(WindowEnvironmentState);

  // Creates the state for `window` and installs it for lookup. The CALLER owns
  // the result. Returns nullptr when `window` is null.
  //
  // Deliberately not called GetOrCreate(): this always creates, and ownership is
  // the caller's, so a name implying a cache lookup would mislead.
  static std::unique_ptr<WindowEnvironmentState> CreateFor(
      BrowserWindowInterface* window);

  // Returns this window's state, or nullptr if Parra never initialised it. Never
  // creates. Does not transfer ownership.
  static WindowEnvironmentState* GetFor(BrowserWindowInterface* window);

  // Resolves the owning window of `model` and returns its state.
  //
  // This is the bridge that lets TabStripModel-based code (the index mapper, the
  // tab strip controller) reach window-scoped state without being handed the
  // window explicitly. Implemented through
  // TabStripModel::delegate()->GetBrowserWindowInterface(), which is the supported
  // way to go from a model back to its window.
  static WindowEnvironmentState* GetForModel(TabStripModel* model);

  explicit WindowEnvironmentState(BrowserWindowInterface* window);
 public:
  // Public so std::unique_ptr can delete it; construction stays through
  // CreateFor().
  ~WindowEnvironmentState();

  WindowEnvironmentState(const WindowEnvironmentState&) = delete;
  WindowEnvironmentState& operator=(const WindowEnvironmentState&) = delete;

  // ---- active environment (this window only) -----------------------------

  // Never empty. Starts as kDefaultEnvironmentId.
  const std::string& active_environment() const { return active_environment_; }

  // Sets this window's active environment. Rejects an empty id.
  //
  // Returns true when the value actually changed, so callers can skip a re-layout
  // on a redundant set without comparing strings themselves.
  bool SetActiveEnvironment(const std::string& environment_id);

  // Restores the default environment. Used when a window is reset.
  void ResetActiveEnvironment();

  // ---- last active tab per environment (this window only) ----------------

  // Global index to activate when switching to `environment_id`: the remembered
  // tab if it still exists and still belongs there, otherwise that environment's
  // first tab. nullopt when the environment has no tabs in this window.
  std::optional<int> GetLastActiveIndex(const std::string& environment_id) const;

  // Records the tab at `global_index` as `environment_id`'s last active tab.
  // Silently ignored when the tab does not belong to that environment, and
  // logged, because that combination indicates a caller bug.
  void RememberActiveTab(const std::string& environment_id, int global_index);

  // Records whatever is currently active, under its own environment. No-op when
  // there is no active tab.
  void RememberCurrentActiveTab();

 private:
  // Handle-based memory: stable across reordering, and it fails closed after the
  // tab is destroyed instead of pointing at a stale index.
  struct LastActiveEntry {
    std::string environment_id;
    tabs::TabHandle handle;
  };

  // The tab at `global_index` belongs to `environment_id`?
  bool IsTabInEnvironment(const std::string& environment_id,
                          int global_index) const;

  // This window's tab strip. Never null while the window is alive; looked up on
  // demand so a model replacement would be picked up instead of dangling.
  TabStripModel* GetModel() const;

  raw_ptr<BrowserWindowInterface> window_;

  std::string active_environment_ = kDefaultEnvironmentId;

  std::vector<LastActiveEntry> last_active_;

  // Installs this object into the window's UnownedUserDataHost so any Parra
  // component can find it from the window. Purely a lookup registry: it does NOT
  // own this object. Ownership is the caller's (see CreateFor()).
  ui::ScopedUnownedUserData<WindowEnvironmentState> scoped_data_holder_;

  // Bumped on every environment change. Lets a caller tell "the environment
  // changed" from "someone re-entered the same environment" without keeping a
  // second copy of the active environment.
  int generation_ = 0;

 public:
  int generation() const { return generation_; }
};

}  // namespace parra

#endif  // CHROME_BROWSER_PARRA_WINDOW_ENVIRONMENT_STATE_H_
