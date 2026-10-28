// Copyright 2026 The Parra Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CHROME_BROWSER_PARRA_ENVIRONMENT_TAB_OPERATIONS_H_
#define CHROME_BROWSER_PARRA_ENVIRONMENT_TAB_OPERATIONS_H_

#include <string>
#include <vector>

#include "base/memory/raw_ptr.h"
#include "chrome/browser/ui/tabs/tab_strip_model.h"
#include "chrome/browser/ui/tabs/tab_strip_user_gesture_details.h"
#include "components/tab_groups/tab_group_id.h"
#include "components/tabs/public/tab_interface.h"

class BrowserWindowInterface;

namespace content {
class WebContents;
}  // namespace content

namespace parra {

// Everything the environment decision for one tab operation depends on.
//
// This is the "context" the isolation rules are expressed against: which window,
// which page asked, and which environment the window is currently showing. Keeping
// it in one struct is what stops the same decision from being re-derived (and
// re-derived differently) in each command handler.
struct EnvironmentTabContext {
  // The window the operation happens in. Never null when the context is valid.
  raw_ptr<BrowserWindowInterface> window = nullptr;

  // The page that initiated the operation, when there is one. Null for
  // user-initiated tab creation (Ctrl+T, `+`), which has no source page.
  raw_ptr<content::WebContents> source_contents = nullptr;

  // The environment this window is currently showing. Read from
  // WindowEnvironmentState at construction time, so it cannot disagree with the
  // tab strip's own view of the window.
  std::string environment_id;

  bool is_valid() const { return window != nullptr; }
};

// Environment-scoped tab operations: cycling, numbered activation, and closing.
//
// WHY THESE LIVE HERE INSTEAD OF IN THE COMMAND HANDLERS
//
// Every one of Ctrl+Tab, Ctrl+Shift+Tab, Ctrl+1..9, Ctrl+9 and Ctrl+W reduces to
// the same question: "which tab of the CURRENT ENVIRONMENT is this?" Answering it
// needs the two coordinate systems (global model index vs visible index) and the
// environment tag, which is exactly what EnvironmentIndexMapper exists for.
// Putting the answer in one class means:
//
//   * BrowserCommandController and browser_commands.cc stay nearly untouched --
//     each call site is one branch, not a copied environment rule;
//   * there is one place where "the environment is empty" is handled, instead of
//     six subtly different fallbacks;
//   * no second TabStripModel, no detaching, no reloading -- these functions only
//     ever activate or close a tab through the native APIs.
//
// All entry points return bool: TRUE means "Parra handled it, do not run the
// upstream path", FALSE means "behave exactly like stock Chromium". That contract
// is what keeps a Parra-disabled build bit-identical to upstream.
class EnvironmentTabOperations {
 public:
  EnvironmentTabOperations() = delete;

  // Captures `window`'s current environment into an EnvironmentTabContext.
  static EnvironmentTabContext MakeContext(BrowserWindowInterface* window);

  // The environment a user-initiated new tab (Ctrl+T, the strip's `+`) must be
  // created in, or an empty string when the normal inheritance rules already give
  // the right answer.
  //
  // Returns non-empty ONLY when the window's current environment has no tabs: in
  // every other case inheriting from the active tab is both correct and simpler,
  // and forcing an id would needlessly bypass the "inherit from the source page"
  // rule that keeps page-derived tabs safe.
  static std::string EnvironmentForUserInitiatedNewTab(
      BrowserWindowInterface* window);

  // Ctrl+Tab / Ctrl+Shift+Tab: the next/previous tab of the CURRENT
  // environment, wrapping inside it. Never leaves the environment.
  static bool SelectRelativeTab(BrowserWindowInterface* browser,
                                bool next,
                                TabStripUserGestureDetails gesture_details);

  // Ctrl+1..Ctrl+8: the Nth tab of the CURRENT environment, counted in visible
  // order. Returns false when the environment has fewer than N tabs, in which
  // case upstream behaviour (which is also "do nothing") is correct.
  static bool SelectTabAtVisibleIndex(BrowserWindowInterface* browser,
                                      int visible_index,
                                      TabStripUserGestureDetails gesture_details);

  // Ctrl+9: the LAST tab of the CURRENT environment.
  static bool SelectLastTab(BrowserWindowInterface* browser,
                            TabStripUserGestureDetails gesture_details);

  // Verifies that the tabs a close is about to remove all belong to the active
  // environment, and narrows the selection to the active tab if they do not.
  //
  // Returns false, because the actual close must still go through the upstream
  // path (unload handlers, beforeunload, pinned-tab confirmation, historical
  // tabs). Parra only refuses to let it reach another environment's tabs.
  static bool PrepareCloseActiveTab(BrowserWindowInterface* browser);

  // Runs after the upstream close completed.
  //
  // The native selection strategy (DetermineNewSelectedIndex) knows nothing about
  // environments, so it can pick a tab in a different one. This puts the window
  // back on the environment the user was in whenever that environment still has
  // tabs, which is what makes Ctrl+W feel like it operates inside a visible tab
  // strip.
  //
  // When the environment has no tabs left there is nothing to restore: the
  // window cannot show an empty content area, so this reports the empty state
  // instead of pretending. It never closes the window (the model still holds
  // other environments' tabs) and never touches another environment's pages.
  static void FinishCloseActiveTab(BrowserWindowInterface* browser,
                                   const std::string& environment_before_close);

  // Contains a tab-context-menu close command to the current environment.
  //
  // Kept as the single call site used by the tab strip controller so no menu path
  // can forget the containment. Returns true when the selection had to be
  // narrowed; only a selection change is performed here.
  //
  // `command_id` is a TabStripModel::ContextMenuCommand (an alias of
  // tabs::TabContextMenuCommand).
  static bool ContainCloseCommandToActiveEnvironment(
      TabStripModel* model,
      int context_index,
      TabStripModel::ContextMenuCommand command_id);

  // What a close command should do, as decided by Parra.
  //
  // A plain bool is NOT enough here, and using one was a real defect in the
  // previous round: "false" meant both "this is not a close command, let Chromium
  // handle it" AND "this is a close command but I could not produce a safe victim
  // set". The caller could not tell them apart, so it fell through to the native
  // path in both cases -- which is exactly the "silently widen the operation"
  // failure the containment exists to prevent. An empty victim set must ABORT, not
  // delegate.
  enum class CloseCommandDisposition {
    // Not a close command (or Parra is off). The caller runs the native path.
    kNotACloseCommand,
    // A safe, environment-scoped victim set was produced. The caller closes exactly
    // these tabs and nothing else.
    kCloseVictims,
    // A close command was recognized but no safe victim set exists. The caller must
    // do NOTHING: no close, no fallback to the native global path.
    kRefuse,
  };

  struct CloseCommandPlan {
    CloseCommandDisposition disposition = CloseCommandDisposition::kNotACloseCommand;
    // Only meaningful for kCloseVictims.
    std::vector<tabs::TabHandle> victims;
    // Human-readable reason, used for the diagnostic log on kRefuse.
    std::string reason;

    bool is_close_command() const {
      return disposition != CloseCommandDisposition::kNotACloseCommand;
    }
  };

  // Decides what a close command must do, without executing anything.
  //
  // The three outcomes exist because the caller has to distinguish "not mine"
  // from "mine, and refused". See CloseCommandDisposition.
  static CloseCommandPlan PlanCloseCommand(
      TabStripModel* model,
      int context_index,
      TabStripModel::ContextMenuCommand command_id);

  // Re-validates a final victim set immediately before it is handed to Chromium.
  //
  // This is the invariant the whole phase rests on:
  //
  //   every WebContents about to be destroyed belongs to the
  //   environment this operation is allowed to touch
  //
  // Checked as a separate step -- not folded into PlanCloseCommand() -- so that a
  // future refactor of the planning code cannot skip it, and so the check can be
  // called on victims that came from anywhere (including a future
  // CloseAllTabsInGroup path). Returns false and logs when any victim fails.
  static bool ValidateVictimsBelongToEnvironment(
      TabStripModel* model,
      const std::vector<tabs::TabHandle>& victims,
      const std::string& environment_id,
      const char* what);

  // ---- environment-scoped victim resolution for close commands -------------
  //
  // The close commands differ in how Chromium decides WHICH tabs die, and each has
  // a different failure mode once the strip is filtered:
  //
  //   CommandCloseTab         victims = selection_model_.selected_tabs().
  //                           A selection is a MODEL-level concept, so it can
  //                           contain tabs the user cannot see.
  //   CommandCloseOtherTabs   victims = every tab except the context tab, in
  //                           GLOBAL model order.
  //   CommandCloseTabsToRight victims = tabs after the context tab, in GLOBAL
  //                           model order.
  //   CommandCloseAllTabs     victims = everything after the pinned home tab, in
  //                           GLOBAL model order (web-app windows only).
  //
  // All four are rewritten here to "Chromium's own victim set, intersected with
  // the active environment, re-validated per tab". The result is a SUBSET of what
  // Chromium chose, never a superset, so every Chromium safeguard (pinned tabs,
  // focused group, split members, collapsed groups, the pinned home tab) is
  // preserved -- Parra only ever REMOVES candidates. That is what makes this safe
  // to reuse rather than reimplement.
  static bool ResolveCloseCommandVictims(
      TabStripModel* model,
      int context_index,
      TabStripModel::ContextMenuCommand command_id,
      std::vector<tabs::TabHandle>* victims);

  // ---- tab group safety ----------------------------------------------------
  //
  // PRODUCT RULE: a tab group belongs to exactly ONE environment.
  //
  // Chromium's TabStripModel imposes no such constraint -- a group is just a
  // collection of tabs -- so with a filtered strip a group can end up holding tabs
  // from several environments. The dangerous consequence is Group Close, which
  // destroys every tab in the group: a user looking at three visible tabs would
  // silently lose another environment's live pages.
  //
  // Rather than rewriting the Tab Group system, this phase does two minimal things:
  //
  //   1. Prevent NEW cross-environment groups at the two points where membership is
  //      decided by a user action (create-group / add-to-group).
  //   2. Make Group Close close only the members of the current environment, and
  //      refuse when that would leave an ambiguous result.
  //
  // Moving a tab into a group never changes its EnvironmentTag (see
  // docs/parra/46), so a group's environment is defined as "the environment of its
  // members", not as stored state.

  // Returns the environment id that `group` belongs to, or an empty string when the
  // group spans several environments (an inconsistent group).
  //
  // A group with no readable member also returns empty.
  static std::string EnvironmentOfGroup(TabStripModel* model,
                                        tab_groups::TabGroupId group);

  // True when every member of `group` carries the same environment tag.
  static bool GroupSpansEnvironments(TabStripModel* model,
                                     tab_groups::TabGroupId group);

  // True when the tab at `context_index` may join `group`.
  //
  // Used to block creating a NEW cross-environment group. The environment used for
  // the comparison is the CONTEXT TAB's own environment, not the window's active
  // one: adding a background tab to a background group is legitimate as long as
  // both belong to the same environment.
  static bool CanJoinGroup(TabStripModel* model,
                           int context_index,
                           tab_groups::TabGroupId group);

  // Closes `group`, restricted to the active environment.
  //
  // Returns false (and closes nothing) when the group cannot be closed safely:
  // a member's tag disagrees with its partition, or the group holds no tab of the
  // active environment. Closing a single environment's members is done through the
  // native CloseTabs() path so unload handlers and group bookkeeping still run; a
  // cross-environment group is first DEFUSED by detaching the foreign members from
  // the group, so the group close that follows can only see this environment.
  static bool CloseGroupInActiveEnvironment(TabStripModel* model,
                                            tab_groups::TabGroupId group);

  // Installs (or removes) the group-membership policy on `model`.
  //
  // Called once per window from BrowserTabStripController::InitFromModel(), which is
  // the same place the window's environment state is created and is the only point
  // that runs exactly once per window.
  //
  // With Parra disabled the policy is NOT installed, so the model behaves exactly
  // like upstream.
  static void InstallGroupMembershipPolicy(TabStripModel* model);

  // Installs the close-command policy on `model`.
  //
  // MUST be installed on the model rather than only in the views controller:
  // ExecuteContextMenuCommand() is called directly by keyboard accelerators
  // (IDC_CLOSE_OTHER_TABS, IDC_CLOSE_TABS_TO_RIGHT), and those callers never see
  // BrowserTabStripController. Installing here covers every caller by construction.
  //
  // Clearing the policy when Parra is disabled restores exact upstream behaviour.
  static void InstallCloseCommandPolicy(TabStripModel* model);

  // The policy itself, exposed for tests: may `indices` be grouped together (when
  // `group` is nullopt) or added to `group`?
  static bool MayGroupTabs(TabStripModel* model,
                           const std::vector<int>& indices,
                           std::optional<tab_groups::TabGroupId> group);

  // RAII wrapper around the pair above: capture the environment on entry, run
  // FinishCloseActiveTab on exit.
  //
  // Exists because CloseTab() has several early-return paths through the native
  // close (split tab, contextual-tasks panel, toast confirmation, normal close)
  // and every one of them must restore the environment. A scope guard cannot
  // forget one, whereas four hand-written calls can.
  class ScopedCloseEnvironmentRestorer {
   public:
    explicit ScopedCloseEnvironmentRestorer(BrowserWindowInterface* browser);
    ScopedCloseEnvironmentRestorer(const ScopedCloseEnvironmentRestorer&) =
        delete;
    ScopedCloseEnvironmentRestorer& operator=(
        const ScopedCloseEnvironmentRestorer&) = delete;
    ~ScopedCloseEnvironmentRestorer();

   private:
    raw_ptr<BrowserWindowInterface> browser_;
    std::string environment_before_close_;
  };

 private:
  // The window's active environment, or an empty string when Parra is off or the
  // window has no environment state.
  static std::string ActiveEnvironment(TabStripModel* model);
};

}  // namespace parra

#endif  // CHROME_BROWSER_PARRA_ENVIRONMENT_TAB_OPERATIONS_H_
