// Copyright 2026 The Parra Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chrome/browser/parra/environment_visibility_controller.h"

#include <optional>

#include "base/check.h"
#include "base/logging.h"
#include "chrome/browser/parra/environment_session_persistence.h"
#include "chrome/browser/parra/parra_storage_partition_util.h"
#include "chrome/browser/parra/window_environment_state.h"
#include "chrome/browser/profiles/profile.h"
#include "chrome/browser/ui/browser_window/public/browser_window_interface.h"
#include "chrome/browser/ui/tabs/tab_strip_model.h"
#include "chrome/browser/ui/tabs/tab_strip_model_delegate.h"
#include "content/public/browser/web_contents.h"

namespace parra {

namespace {

// The profile owning this strip, or null when the model has no resolvable
// window (unit tests, teardown). Deliberately never guesses: a wrong profile
// would make the environment-existence check answer for the wrong profile.
Profile* ProfileForModel(TabStripModel* model) {
  if (!model || !model->delegate()) {
    return nullptr;
  }
  BrowserWindowInterface* window =
      model->delegate()->GetBrowserWindowInterface();
  return window ? window->GetProfile() : nullptr;
}

}  // namespace

EnvironmentVisibilityController::EnvironmentVisibilityController(
    TabStripModel* model)
    : model_(model), mapper_(model) {
  CHECK(model_);
}

EnvironmentVisibilityController::~EnvironmentVisibilityController() = default;

bool EnvironmentVisibilityController::SwitchTo(
    const std::string& environment_id) {
  if (environment_id.empty()) {
    LogEnvironmentGuardRefusal("switch", environment_id,
                               "empty environment id");
    return false;
  }

  // The environment must exist. Refusing here is what stops a stale id (a
  // bookmarklet, a restored window, a deleted environment) from putting the
  // window into a state whose only visible outcome would be "it showed Personal
  // instead", which is the silent downgrade this project must never produce.
  //
  // The reserved `personal` environment always resolves, so switching to it
  // keeps working even when no environments are configured.
  if (Profile* profile = ProfileForModel(model_)) {
    const PartitionResolution lookup =
        ResolvePartitionForEnvironment(profile, environment_id);
    if (!lookup.ok()) {
      LogEnvironmentGuardRefusal("switch", environment_id, lookup.failure);
      return false;
    }
  }

  // Switching to an environment with NO tabs is allowed, and lands the window
  // in an empty tab strip owned by that environment.
  //
  // This used to be refused, on the grounds that "the strip would show a
  // foreign tab". That reasoning was wrong: the alternative to an empty strip
  // is not "show something sensible", it is "silently refuse and leave the user
  // on the environment they were trying to leave". A user who has configured an
  // environment and closed its last tab must still be able to select it,
  // otherwise the only way to ever put a tab in it is to already have one
  // there.
  //
  // The state is coherent: the active environment is an environment that
  // exists, and it simply has no tabs. The first tab created while it is active
  // is created in ITS partition (EnvironmentTabRouter resolves from the active
  // environment), so the empty state resolves itself as soon as the user opens
  // anything.
  const int visible_count = mapper_.GetEnvironmentTabCount(environment_id);

  if (environment_id == mapper_.active_environment()) {
    LOG(INFO) << "parra-vis: already on '" << environment_id << "'; no-op";
    return true;
  }

  // Step 1: remember where the outgoing environment was.
  RememberCurrentTab();

  const std::string previous = mapper_.active_environment();

  // The whole switch is ONE observable change.
  //
  // Steps 2 and 3 together (change environment, then activate that
  // environment's tab) pass through an intermediate state where the active
  // environment is the new one but the active tab still belongs to the old one.
  // A notification delivered in that window would make the tab strip lay out
  // with the filter pointed at the new environment while the active tab is
  // hidden by it -- one frame of an empty strip with no selection. Deferring
  // collapses the two steps into a single notification, so the only layout a
  // user can see is the finished state.
  parra::WindowEnvironmentState* state =
      parra::WindowEnvironmentState::GetForModel(model_);
  std::optional<parra::WindowEnvironmentState::ScopedNotificationDeferral>
      deferral;
  if (state) {
    deferral.emplace(state);
  }

  // Step 2: change which environment is active. Note this does NOT move any
  // tab: the model is untouched.
  mapper_.SetActiveEnvironment(environment_id);

  // A browser-level tab stays selected and is visible in every environment.
  if (BrowserLevelTab::Is(model_->GetActiveWebContents())) {
    ++switch_count_;
    RecordWindowActiveEnvironment();
    return true;
  }

  // An environment with no tabs in this window: create one IN IT.
  //
  // WHY A TAB AND NOT AN EMPTY STRIP
  //
  // Chromium requires an active tab at all times
  // (BrowserView::OnActiveTabChanged and the model both assume non-null
  // contents). Leaving the strip "empty" therefore never actually produced an
  // empty window: the previously active tab stayed active and stayed ATTACHED
  // to the content view, so the pane kept rendering the OTHER environment's
  // page -- including its cookies and its document focus. Measured before this
  // change with probe-empty-environment.js: toolbar `test-b`, strip empty, pane
  // rendering ENV-A with `hasFocus=true`.
  //
  // Creating the tab here is the only way to satisfy Chromium's constraint
  // without showing foreign content, and it also matches what the user asked
  // for by choosing an environment: a place to browse in it. The tab is created
  // through the same path a Ctrl+T uses
  // (EnvironmentTabOperations::EnvironmentForUserInitiatedNewTab), so it lands
  // in the right partition and is tagged with the right environment -- there is
  // no second implementation of "which environment does a new tab belong to".
  //
  // The tab is created only when this window really has no tabs for the
  // environment, so re-selecting an environment that has tabs still just
  // activates its remembered tab.
  //
  // WHO CREATES THE TAB, AND WHY IT IS NOT THIS CLASS
  //
  // The tab is created by the UI layer
  // (BrowserTabStripController::OnActiveEnvironmentChanged, which calls
  // chrome::NewTab), NOT here. The tab has to carry the environment id
  // explicitly, and the only API that accepts it is
  // NavigateParams::parra_environment_id, which chrome::NewTab fills through
  // EnvironmentTabOperations::EnvironmentForUserInitiatedNewTab().
  //
  // The tempting shortcut -- model_->delegate()->AddTabAt() -- is WRONG: it
  // reaches chrome::AddTabAt() in chrome/browser/ui/browser_tabstrip.cc, which
  // hard-codes parra_environment_id = "" and lets the router inherit from the
  // active tab. While the target environment is empty the active tab belongs to
  // a DIFFERENT environment, so that inherited id is exactly the
  // cross-environment leak this method exists to prevent. Verified by reading
  // the call chain, not assumed.
  //
  // This class can only reach TabStripModel, and
  // //chrome/browser/parra:parra_tabs may not depend on chrome/browser/ui (GN
  // cycle), so the creation is delegated upward. The switch itself is still
  // reported as successful here: the window IS on the requested environment.
  if (visible_count == 0) {
    ++switch_count_;
    LOG(INFO) << "parra-vis: switch #" << switch_count_ << " '" << previous
              << "' -> '" << environment_id
              << "' (environment has no tabs; the UI layer will create the "
                 "first one)";
    // Recorded here so the tab-creation path, which resolves the environment
    // from the window's active environment, sees the target rather than the
    // outgoing environment.
    RecordWindowActiveEnvironment();

    // NO VerifyInvariants() HERE, DELIBERATELY.
    //
    // The invariant is "the active tab's environment == the active
    // environment", and at this instant it is necessarily FALSE: the target
    // environment has no tabs, so the active tab still belongs to the outgoing
    // one. The tab that will satisfy it does not exist yet -- it is created by
    // the UI layer immediately after this returns.
    //
    // Checking here therefore produced a guaranteed false alarm:
    //   parra-vis: INVARIANT VIOLATED (after-switch-empty): active tab g1
    //   belongs to 'test-a' but active environment is 'test-b'
    // which is the CORRECT intermediate state of a two-step operation, not a
    // defect. A check that always fires teaches readers to ignore it, which is
    // worse than no check at all.
    //
    // The check that does carry information runs where the invariant can
    // actually hold: BrowserTabStripController::EnsureActiveEnvironmentHasTab()
    // verifies, right after chrome::NewTab(), that the new tab really is in the
    // requested environment. That is the moment a cross-environment leak would
    // first be observable in the model.
    LogSnapshot("after-switch-empty");
    return true;
  }

  // Step 3: activate the target environment's remembered tab.
  //
  // ActivateTabAt() changes selection only. Chromium then drives the page
  // visibility itself: BrowserView::ActiveTabChanged() swaps the WebContents
  // hosted by ContentsWebView, which is the single supported path by which a
  // tab becomes visible or hidden. Pages in other environments stay alive as
  // background tabs -- exactly the same state as a normal unfocused tab.
  const std::optional<int> target =
      mapper_.GetActiveGlobalIndexFor(environment_id);
  if (!target.has_value()) {
    // GetEnvironmentTabCount() > 0 guarantees this cannot happen; treat as a
    // bug rather than silently leaving the strip in a half-switched state.
    LOG(ERROR) << "parra-vis: environment '" << environment_id
               << "' has tabs but no restorable index; reverting to '"
               << previous << "'";
    mapper_.SetActiveEnvironment(previous);
    // The revert is itself a change; let it be delivered so the strip returns
    // to the environment it was actually showing.
    deferral.reset();
    return false;
  }

  ++switch_count_;
  LOG(INFO) << "parra-vis: switch #" << switch_count_ << " '" << previous
            << "' -> '" << environment_id << "' target_model_index=" << *target
            << " visible_count="
            << mapper_.GetEnvironmentTabCount(environment_id);

  model_->ActivateTabAt(*target);

  // Step 4: record the window's active environment so a restart can put the
  // user back where they were.
  //
  // This is recorded SEPARATELY from any tab's ownership, under its own key,
  // and that separation is deliberate: the active environment is a VIEW
  // preference which is allowed to fall back to `personal` on restore, while a
  // tab's partition must never be changed by such a fallback. Two concerns, two
  // keys; one key would let a fallback silently look like a re-ownership.
  RecordWindowActiveEnvironment();

  LogSnapshot("after-switch");
  VerifyInvariants("after-switch");

  // Step 5: `deferral` is released here by the return, which is what delivers
  // the single notification that makes the tab strip re-lay out. The strip
  // therefore repaints against a fully settled state: new environment AND its
  // active tab.
  return true;
}

void EnvironmentVisibilityController::RecordWindowActiveEnvironment() const {
  if (!model_) {
    return;
  }

  TabStripModelDelegate* delegate = model_->delegate();
  if (!delegate) {
    return;
  }

  BrowserWindowInterface* window = delegate->GetBrowserWindowInterface();
  if (!window) {
    return;
  }

  EnvironmentSessionPersistence::RecordWindowActiveEnvironment(
      window->GetProfile(), window, mapper_.active_environment());
}

void EnvironmentVisibilityController::VerifyInvariants(const char* when) const {
  // The invariant that the whole design rests on:
  //   the active tab's environment == the active environment.
  //
  // It is checked rather than assumed because two independent things can break
  // it: a native activation path that does not go through SwitchTo(), and the
  // environment being changed without re-activating a tab in it. Either case
  // would leave the content area showing one environment while the tab strip
  // shows another -- the single worst failure mode of this architecture, and
  // one that is invisible without an explicit check.
  const int active_index = model_->active_index();
  if (active_index == TabStripModel::kNoTab) {
    LOG(WARNING) << "parra-vis: invariant check (" << when
                 << ") skipped: no active tab";
    return;
  }

  if (BrowserLevelTab::Is(model_->GetWebContentsAt(active_index))) {
    return;
  }

  const std::optional<std::string> active_env =
      mapper_.EnvironmentOfGlobalIndex(active_index);
  if (!active_env.has_value()) {
    LOG(ERROR) << "parra-vis: INVARIANT VIOLATED (" << when
               << "): active tab at g" << active_index
               << " has no readable environment";
    return;
  }

  if (*active_env != mapper_.active_environment()) {
    LOG(ERROR) << "parra-vis: INVARIANT VIOLATED (" << when << "): active tab g"
               << active_index << " belongs to '" << *active_env
               << "' but active environment is '"
               << mapper_.active_environment()
               << "'. The tab strip and the content area disagree.";
    return;
  }

  // Second invariant: the active tab must be visible in its own environment.
  if (!mapper_.IsTabVisible(mapper_.active_environment(), active_index)) {
    LOG(ERROR) << "parra-vis: INVARIANT VIOLATED (" << when << "): active tab g"
               << active_index << " is not visible in its own environment '"
               << mapper_.active_environment() << "'";
    return;
  }

  LOG(INFO) << "parra-vis: invariant OK (" << when << ") active tab g"
            << active_index << " env='" << *active_env << "'";
}

void EnvironmentVisibilityController::NotifyEnvironmentChanged() {
  if (!filter_observer_) {
    return;
  }
  filter_observer_->OnActiveEnvironmentChanged();
}

void EnvironmentVisibilityController::RememberCurrentTab() {
  const int active = model_->active_index();
  if (active == TabStripModel::kNoTab) {
    return;
  }

  // Remember under the tab's own environment rather than assuming the tab is in
  // the active environment. Activating a tab through the tab strip (a click, a
  // session restore, a window move) changes selection without going through
  // SwitchTo(), so the two can legitimately disagree for a moment. Recording
  // the tab under its real owner keeps each environment's memory correct, and
  // the mapper logs the disagreement so Phase 2.2B can wire the missing sync.
  mapper_.RememberCurrentActiveTab();
}

void EnvironmentVisibilityController::LogSnapshot(const char* reason) const {
  LOG(INFO) << "parra-vis: snapshot (" << reason << ") active='"
            << mapper_.active_environment()
            << "' model_count=" << model_->count()
            << " active_index=" << model_->active_index()
            << " switches=" << switch_count_;

  // Report every tab's environment so a mismatch is visible in the log instead
  // of silently filtered out.
  for (int i = 0; i < model_->count(); ++i) {
    const std::optional<std::string> env = mapper_.EnvironmentOfGlobalIndex(i);
    content::WebContents* contents = model_->GetWebContentsAt(i);
    LOG(INFO) << "parra-vis:   index=" << i
              << " env=" << (env ? *env : "<none>") << " contents="
              << (contents ? contents->GetUniqueToken().ToString() : "<null>")
              << " active=" << (i == model_->active_index() ? "yes" : "no");
  }
}

}  // namespace parra
