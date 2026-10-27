// Copyright 2026 The Parra Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chrome/browser/parra/environment_visibility_controller.h"

#include "base/check.h"
#include "base/logging.h"
#include "chrome/browser/ui/tabs/tab_strip_model.h"
#include "content/public/browser/web_contents.h"

namespace parra {

EnvironmentVisibilityController::EnvironmentVisibilityController(
    TabStripModel* model)
    : model_(model), mapper_(model) {
  CHECK(model_);
}

EnvironmentVisibilityController::~EnvironmentVisibilityController() = default;

bool EnvironmentVisibilityController::SwitchTo(
    const std::string& environment_id) {
  if (environment_id.empty()) {
    LOG(ERROR) << "parra-vis: refusing to switch to an empty environment id";
    return false;
  }

  // Refuse to switch to an environment with no tabs. Doing so would leave the
  // strip showing a foreign tab, which is a state the rest of the code cannot
  // reason about.
  if (mapper_.GetVisibleCount(environment_id) == 0) {
    LOG(WARNING) << "parra-vis: environment '" << environment_id
                 << "' has no tabs; switch ignored (still on '"
                 << mapper_.active_environment() << "')";
    return false;
  }

  if (environment_id == mapper_.active_environment()) {
    LOG(INFO) << "parra-vis: already on '" << environment_id << "'; no-op";
    return true;
  }

  // Step 1: remember where the outgoing environment was.
  RememberCurrentTab();

  const std::string previous = mapper_.active_environment();

  // Step 2: change which environment is active. Note this does NOT move any
  // tab: the model is untouched.
  mapper_.SetActiveEnvironment(environment_id);

  // Step 3: activate the target environment's remembered tab.
  //
  // ActivateTabAt() changes selection only. Chromium then drives the page
  // visibility itself: BrowserView::ActiveTabChanged() swaps the WebContents
  // hosted by ContentsWebView, which is the single supported path by which a
  // tab becomes visible or hidden. Pages in other environments stay alive as
  // background tabs -- exactly the same state as a normal unfocused tab.
  const std::optional<int> target = mapper_.GetActiveGlobalIndexFor(environment_id);
  if (!target.has_value()) {
    // GetVisibleCount() > 0 guarantees this cannot happen; treat as a bug
    // rather than silently leaving the strip in a half-switched state.
    LOG(ERROR) << "parra-vis: environment '" << environment_id
               << "' has tabs but no restorable index; reverting to '"
               << previous << "'";
    mapper_.SetActiveEnvironment(previous);
    return false;
  }

  ++switch_count_;
  LOG(INFO) << "parra-vis: switch #" << switch_count_ << " '" << previous
            << "' -> '" << environment_id << "' target_model_index=" << *target
            << " visible_count="
            << mapper_.GetVisibleCount(environment_id);

  model_->ActivateTabAt(*target);

  // Step 4: the tab strip must now show the new environment's tabs and hide the
  // previous one's. The UI reads the active environment from the mapper on
  // demand, so all that is required here is to make it re-lay out; see
  // EnvironmentFilterObserver::OnActiveEnvironmentChanged().
  NotifyEnvironmentChanged();

  LogSnapshot("after-switch");
  VerifyInvariants("after-switch");
  return true;
}

void EnvironmentVisibilityController::VerifyInvariants(const char* when) const {
  // The invariant that the whole design rests on:
  //   the active tab's environment == the active environment.
  //
  // It is checked rather than assumed because two independent things can break
  // it: a native activation path that does not go through SwitchTo(), and the
  // environment being changed without re-activating a tab in it. Either case
  // would leave the content area showing one environment while the tab strip
  // shows another -- the single worst failure mode of this architecture, and one
  // that is invisible without an explicit check.
  const int active_index = model_->active_index();
  if (active_index == TabStripModel::kNoTab) {
    LOG(WARNING) << "parra-vis: invariant check (" << when
                 << ") skipped: no active tab";
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
    LOG(ERROR) << "parra-vis: INVARIANT VIOLATED (" << when
               << "): active tab g" << active_index << " belongs to '"
               << *active_env << "' but active environment is '"
               << mapper_.active_environment()
               << "'. The tab strip and the content area disagree.";
    return;
  }

  // Second invariant: the active tab must be visible in its own environment.
  if (!mapper_.IsTabVisible(mapper_.active_environment(), active_index)) {
    LOG(ERROR) << "parra-vis: INVARIANT VIOLATED (" << when
               << "): active tab g" << active_index
               << " is not visible in its own environment '"
               << mapper_.active_environment() << "'";
    return;
  }

  LOG(INFO) << "parra-vis: invariant OK (" << when << ") active tab g"
            << active_index << " env='" << *active_env << "'";
}

void EnvironmentVisibilityController::NotifyEnvironmentChanged() {
  if (!filter_observer_)
    return;
  filter_observer_->OnActiveEnvironmentChanged();
}

void EnvironmentVisibilityController::RememberCurrentTab() {
  const int active = model_->active_index();
  if (active == TabStripModel::kNoTab)
    return;

  // Remember under the tab's own environment rather than assuming the tab is in
  // the active environment. Activating a tab through the tab strip (a click, a
  // session restore, a window move) changes selection without going through
  // SwitchTo(), so the two can legitimately disagree for a moment. Recording the
  // tab under its real owner keeps each environment's memory correct, and the
  // mapper logs the disagreement so Phase 2.2B can wire the missing sync.
  mapper_.RememberCurrentActiveTab();
}

void EnvironmentVisibilityController::LogSnapshot(const char* reason) const {
  LOG(INFO) << "parra-vis: snapshot (" << reason << ") active='"
            << mapper_.active_environment() << "' model_count="
            << model_->count() << " active_index=" << model_->active_index()
            << " switches=" << switch_count_;

  // Report every tab's environment so a mismatch is visible in the log instead
  // of silently filtered out.
  for (int i = 0; i < model_->count(); ++i) {
    const std::optional<std::string> env = mapper_.EnvironmentOfGlobalIndex(i);
    content::WebContents* contents = model_->GetWebContentsAt(i);
    LOG(INFO) << "parra-vis:   index=" << i
              << " env=" << (env ? *env : "<none>")
              << " contents="
              << (contents ? contents->GetUniqueToken().ToString() : "<null>")
              << " active=" << (i == model_->active_index() ? "yes" : "no");
  }
}

}  // namespace parra
