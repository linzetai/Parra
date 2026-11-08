// Copyright 2026 The Parra Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CHROME_BROWSER_PARRA_WINDOW_ENVIRONMENT_STATE_H_
#define CHROME_BROWSER_PARRA_WINDOW_ENVIRONMENT_STATE_H_

#include <optional>
#include <string>
#include <vector>

#include "base/callback_list.h"
#include "base/functional/callback_forward.h"
#include "base/memory/raw_ptr.h"
#include "base/memory/weak_ptr.h"
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
// Parra is meant to be a daily-driver browser, so two windows must be able to
// sit on different environments at the same time (window A on Personal, window
// B on Test-A) without either affecting the other. A process-global value
// cannot express that: the moment window B switches, window A's tab strip would
// repaint with B's environment. This class replaces the earlier process-global
// ActiveEnvironment for exactly that reason.
//
// The interesting part is not the storage, it is finding the right SCOPE for
// each piece of state:
//
//   Environment definitions   -> Profile (EnvironmentRegistry). Same
//   environments
//                                in every window of a profile; they are a
//                                property of the profile's configuration.
//   StoragePartition          -> Profile. Owned by the SecurityPrincipal of the
//                                committed document; a window cannot have its
//                                own.
//   Active environment        -> THIS window. It is a view state: "what is this
//                                window looking at right now".
//   Last active tab per env   -> THIS window, because it names a TabHandle that
//                                only exists in this window's TabStripModel.
//
// STORAGE: BrowserWindowInterface::GetUnownedUserDataHost()
//
// BrowserWindowInterface does not inherit base::SupportsUserData, so there is
// no SetUserData() to use. It does expose ui::UnownedUserDataHost, and its own
// header says feature-specific state belongs there, so that is what this uses
// for LOOKUP:
//
//   - the host is keyed by type, so there is exactly one instance per window --
//   a
//     second writer overwrites rather than creating a second truth;
//   - lookup works from the window, and from a TabStripModel via its delegate.
//
// OWNERSHIP: the BrowserTabStripController, NOT this object
//
// UnownedUserData does not own (the name is literal -- ScopedUnownedUserData
// only Erases on destruction). An earlier version of this class therefore made
// itself self-deleting from the BrowserDidClose notification. That ordering is
// unsafe:
//
//   Browser::~Browser()
//     -> !IsDeleteScheduled() -> UnloadController::OnWindowClosing()
//          -> TabStripModel::CloseAllTabs()
//               -> Parra observers still running
//          -> OnWindowCloseComplete() -> BrowserDidClose -> delete state
//     -> window_.reset()
//     -> features_.reset()
//
// The notification can therefore fire while tab closing is still in progress,
// and any Parra callback that runs afterwards would touch a deleted object. It
// also paints a picture where the state outlives nothing in particular: whether
// it is freed at all depends on whether a close notification was ever emitted.
//
// So ownership is explicit and local instead: BrowserTabStripController owns
// the object (one controller per window, destroyed with the window's tab strip)
// and installs it into the host purely so other components can find it. This is
// the same shape Chromium's own UnloadController uses: it registers itself in
// the window's UnownedUserDataHost via a ScopedUnownedUserData member but is
// owned by BrowserWindowFeatures, not by the host.
//
// The lifetime consequence: the state lives exactly as long as the window's tab
// strip controller. It cannot be resurrected after teardown (GetOrCreate() only
// runs from InitFromModel()), and it cannot be freed out from under a running
// tab close.
class WindowEnvironmentState {
 public:
  DECLARE_USER_DATA(WindowEnvironmentState);

  // Creates the state for `window` and installs it for lookup. The CALLER owns
  // the result. Returns nullptr when `window` is null.
  //
  // Deliberately not called GetOrCreate(): this always creates, and ownership
  // is the caller's, so a name implying a cache lookup would mislead.
  static std::unique_ptr<WindowEnvironmentState> CreateFor(
      BrowserWindowInterface* window);

  // Returns this window's state, or nullptr if Parra never initialised it.
  // Never creates. Does not transfer ownership.
  static WindowEnvironmentState* GetFor(BrowserWindowInterface* window);

  // Resolves the owning window of `model` and returns its state.
  //
  // This is the bridge that lets TabStripModel-based code (the index mapper,
  // the tab strip controller) reach window-scoped state without being handed
  // the window explicitly. Implemented through
  // TabStripModel::delegate()->GetBrowserWindowInterface(), which is the
  // supported way to go from a model back to its window.
  static WindowEnvironmentState* GetForModel(TabStripModel* model);

  // Live instance count, for the lifetime tests (docs/parra/64, 67).
  //
  // A leaked state has no visible symptom, so the only way to assert "closing a
  // window releases its state" is to expose the count. Test-only in spirit; not
  // used by any product path.
  static int live_instance_count();
  static int total_created_count();

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
  // Returns true when the value actually changed, so callers can skip a
  // re-layout on a redundant set without comparing strings themselves.
  bool SetActiveEnvironment(const std::string& environment_id);

  // Restores the default environment. Used when a window is reset.
  void ResetActiveEnvironment();

  // ---- session-restore protection ----------------------------------------
  //
  // Marks the environment as AUTHORITATIVE for the current restore, so the tab
  // strip's "native activation adopts the active tab's environment" step does
  // not undo it.
  //
  // WHY THIS IS NEEDED
  //
  // Session restore sets the window's environment from the session file, then
  // restores the tabs and activates the recorded selection. Every activation
  // passes through BrowserTabStripController::AdoptActiveTabEnvironment(),
  // which assumes a native activation is the user moving to that tab and
  // rewrites the window's environment to match. During restore that assumption
  // is exactly backwards: the activation is bookkeeping, and the environment is
  // what the user chose before closing the window. Without this guard the
  // restore was silently discarded within ~14 ms:
  //
  //   parra-env: active environment 'personal' -> 'test-b'
  //   parra-restore: window active environment restored to 'test-b'
  //   parra-env: active environment 'test-b' -> 'personal'     <-- adopt undid
  //   it
  //
  // A PASS, NOT A ONE-SHOT. Restoring N tabs performs N activations (one per
  // restored tab, plus the final selection), and each one reaches the adopt
  // step. Consuming the guard on the first activation let the second one undo
  // the restore anyway:
  //
  //   parra-ui: not adopting environment 'personal' for the activation
  //   performed by
  //             session restore; the restored environment 'test-b' stays
  //             authoritative
  //   parra-env: active environment 'test-b' -> 'personal'      <-- the next
  //   activation
  //
  // So the suppression lasts until EndEnvironmentAdoptionSuppression(), which
  // session restore calls once the window is up. After that, a user clicking
  // another environment's tab moves the window as usual.
  void BeginEnvironmentAdoptionSuppression();
  void EndEnvironmentAdoptionSuppression();
  bool EnvironmentAdoptionSuppressed() const {
    return adoption_suppression_depth_ > 0;
  }

  // A provisional browser-level transfer reserves the original window's slot.
  bool ReserveBrowserLevelTab(content::WebContents* contents);
  void ReleaseBrowserLevelTabReservation(content::WebContents* contents);
  bool BrowserLevelTabSlotAvailable(content::WebContents* contents) const;

  // Scoped form of the pair above.
  //
  // PREFER THIS OVER CALLING Begin/End BY HAND. The bug this exists to prevent:
  // session restore called Begin() from one function and End() from another,
  // and the second function is only reachable for a window that restores a
  // selected tab. A window that finished restoring without reaching it kept the
  // depth at 1 for its whole life, so it silently stopped adopting environments
  // -- a failure that only showed up after a restart, and looked like "the
  // environment switcher stopped working" rather than like a missing decrement.
  //
  // With this, the release is tied to a scope instead of to a code path, so a
  // new early return cannot reintroduce the leak.
  class ScopedAdoptionSuppression {
   public:
    explicit ScopedAdoptionSuppression(WindowEnvironmentState* state)
        : state_(state) {
      if (state_) {
        state_->BeginEnvironmentAdoptionSuppression();
      }
    }

    ~ScopedAdoptionSuppression() {
      if (state_) {
        state_->EndEnvironmentAdoptionSuppression();
      }
    }

    ScopedAdoptionSuppression(const ScopedAdoptionSuppression&) = delete;
    ScopedAdoptionSuppression& operator=(const ScopedAdoptionSuppression&) =
        delete;

   private:
    raw_ptr<WindowEnvironmentState> state_;
  };

  // ---- change notification -----------------------------------------------
  //
  // WHY THIS LIVES HERE AND NOT ON EnvironmentVisibilityController
  //
  // EnvironmentVisibilityController is a VALUE type: callers construct one per
  // operation and let it die (EnvironmentTabOperations creates them on the
  // stack, the visibility PoC owns one per run). An observer registered on such
  // an object would die with it, so a switch performed by one short-lived
  // controller would silently fail to notify a tab strip that outlives it --
  // which is exactly the bug that left the tab strip showing one environment
  // while the content area showed another (docs/parra/93 §2.2).
  //
  // This object, by contrast, has the lifetime that the notification needs: one
  // per window, created by BrowserTabStripController::InitFromModel() and
  // destroyed with that window's tab strip. Registering here means the
  // notification outlives any individual switch, and cannot outlive the window
  // it refers to.
  //
  // LIFETIME SAFETY: subscribers hold a base::CallbackListSubscription, which
  // unregisters on destruction and is safe against a destroyed CallbackList. So
  // a tab strip that is torn down before this object cannot be called back, and
  // this object cannot leak a pointer into a destroyed view. Callbacks are
  // invoked synchronously on the UI thread.
  using ActiveEnvironmentChangedCallback = base::RepeatingClosure;
  base::CallbackListSubscription AddActiveEnvironmentChangedCallback(
      ActiveEnvironmentChangedCallback callback);

  // Defers change notifications until this object is destroyed.
  //
  // WHY A SWITCH NEEDS THIS
  //
  // Switching environments is a two-step operation: change the active
  // environment, then activate that environment's tab. Notifying between the
  // two steps would re-lay out the tab strip at a moment when the active
  // environment is already the new one but the active TAB still belongs to the
  // old one. The filter would then report the old environment's active tab as
  // hidden, and the strip would paint one frame with no visible selection
  // before the activation arrives.
  //
  // Deferring lets the whole two-step operation complete and then notify
  // exactly once, so the only layout the user can observe is the finished
  // state.
  //
  // Nestable and safe: the counter is per instance, and the destructor notifies
  // only when the last deferral ends and something actually changed. Must not
  // outlive the state it defers.
  class ScopedNotificationDeferral {
   public:
    explicit ScopedNotificationDeferral(WindowEnvironmentState* state);
    ~ScopedNotificationDeferral();

    ScopedNotificationDeferral(const ScopedNotificationDeferral&) = delete;
    ScopedNotificationDeferral& operator=(const ScopedNotificationDeferral&) =
        delete;

   private:
    raw_ptr<WindowEnvironmentState> state_;
  };

  // ---- last active tab per environment (this window only) ----------------

  // Global index to activate when switching to `environment_id`: the remembered
  // tab if it still exists and still belongs there, otherwise that
  // environment's first tab. nullopt when the environment has no tabs in this
  // window.
  std::optional<int> GetLastActiveIndex(
      const std::string& environment_id) const;

  // Records the tab at `global_index` as `environment_id`'s last active tab.
  // Silently ignored when the tab does not belong to that environment, and
  // logged, because that combination indicates a caller bug.
  void RememberActiveTab(const std::string& environment_id, int global_index);

  // Records whatever is currently active, under its own environment. No-op when
  // there is no active tab.
  void RememberCurrentActiveTab();

 private:
  // Handle-based memory: stable across reordering, and it fails closed after
  // the tab is destroyed instead of pointing at a stale index.
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
  // component can find it from the window. Purely a lookup registry: it does
  // NOT own this object. Ownership is the caller's (see CreateFor()).
  ui::ScopedUnownedUserData<WindowEnvironmentState> scoped_data_holder_;

  // Bumped on every environment change. Lets a caller tell "the environment
  // changed" from "someone re-entered the same environment" without keeping a
  // second copy of the active environment.
  int generation_ = 0;

  // Notified after active_environment_ changes and after generation_ is bumped,
  // so a listener that re-reads the environment sees the new value.
  //
  // Repeating (not one-shot): a window switches environments many times over
  // its life, and each switch must reach the same listener.
  base::RepeatingClosureList active_environment_changed_;

  // Nesting depth of ScopedNotificationDeferral. While non-zero, a change is
  // recorded in `notification_pending_` instead of being delivered, so the
  // whole multi-step operation produces exactly one notification at the end.
  int notification_deferral_depth_ = 0;
  bool notification_pending_ = false;

  // Greater than zero while a session restore owns the environment. See the
  // accessor block above. A depth rather than a bool so nested restores (a
  // second window restored while the first is still coming up) cannot clear it
  // early.
  int adoption_suppression_depth_ = 0;
  base::WeakPtr<content::WebContents> browser_level_reservation_;

 public:
  int generation() const { return generation_; }
};

}  // namespace parra

#endif  // CHROME_BROWSER_PARRA_WINDOW_ENVIRONMENT_STATE_H_
