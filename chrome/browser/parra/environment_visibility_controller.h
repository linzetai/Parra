// Copyright 2026 The Parra Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CHROME_BROWSER_PARRA_ENVIRONMENT_VISIBILITY_CONTROLLER_H_
#define CHROME_BROWSER_PARRA_ENVIRONMENT_VISIBILITY_CONTROLLER_H_

#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "base/memory/raw_ptr.h"
#include "chrome/browser/parra/environment_index_mapper.h"

class TabStripModel;

namespace parra {

// Observer for "the active environment changed".
//
// SUPERSEDED BY WindowEnvironmentState::AddActiveEnvironmentChangedCallback().
//
// Kept so existing self-tests and the visibility PoC keep compiling, but it is
// no longer the mechanism the product uses, and it is not what a new caller
// should register: the controller that owns this pointer is a short-lived value
// object (EnvironmentTabOperations constructs one per operation), so an
// observer registered here would be forgotten the moment that particular
// controller died. See window_environment_state.h for the lifetime argument.
class EnvironmentFilterObserver {
 public:
  virtual ~EnvironmentFilterObserver() = default;

  // Called after the active environment has changed and after the new
  // environment's tab has been activated. Implementations must only re-apply
  // visibility; they must not change the active tab.
  virtual void OnActiveEnvironmentChanged() = 0;
};

// Applies environment visibility to a tab strip without ever removing a tab
// from the TabStripModel.
//
// The mechanism, and why it is the one Chromium already supports:
//
//   A tab's page visibility is driven by whether its WebContents is attached to
//   a view hierarchy. BrowserView swaps the visible WebContents by calling
//   WebContentsView::SetWebContents(), and ui/views/controls/webview/webview.cc
//   explicitly documents that the previously hosted WebContents keeps receiving
//   updates while detached from the hierarchy.
//
// So "hiding an environment" is exactly "make its tabs non-active in the
// strip", which is what TabStripModel already does for background tabs. There
// is no separate visibility flag to invent, and no WasHidden() to call by hand.
//
// Deliberate non-goals:
//   * Never calls DetachTabAtForInsertion().
//   * Never destroys or recreates a WebContents.
//   * Never re-navigates.
//   * Never touches StoragePartition or cookies.
class EnvironmentVisibilityController {
 public:
  explicit EnvironmentVisibilityController(TabStripModel* model);
  EnvironmentVisibilityController(const EnvironmentVisibilityController&) =
      delete;
  EnvironmentVisibilityController& operator=(
      const EnvironmentVisibilityController&) = delete;
  ~EnvironmentVisibilityController();

  const EnvironmentIndexMapper& mapper() const { return mapper_; }
  EnvironmentIndexMapper& mapper() { return mapper_; }

  const std::string& active_environment() const {
    return mapper_.active_environment();
  }

  // Switches the strip to `environment_id`:
  //   1. remembers the outgoing environment's active tab,
  //   2. updates the mapper's active environment,
  //   3. activates the target environment's remembered tab.
  //
  // Returns false when the target environment has no tabs, in which case
  // nothing changes. This is not an error: an empty environment simply cannot
  // be shown, and silently leaving the strip on the old environment is better
  // than closing the window.
  bool SwitchTo(const std::string& environment_id);

  // Records the currently active tab as the active tab of the current
  // environment. Call this whenever activation changes.
  void RememberCurrentTab();

  // Counts switches, so tests can assert the operation actually ran.
  int switch_count() const { return switch_count_; }

  // Registers an observer notified whenever the active environment changes.
  // Only one observer is supported: there is exactly one tab strip per window,
  // and the PoC has one window.
  void set_environment_filter_observer(EnvironmentFilterObserver* observer) {
    filter_observer_ = observer;
  }

  // Verifies the two invariants the design depends on:
  //   1. active tab's environment == active environment
  //   2. the active tab is visible in that environment
  // Logs an ERROR when violated. Never mutates state: a violated invariant is a
  // bug to be reported, not something to paper over by moving the selection.
  void VerifyInvariants(const char* when) const;

 private:
  // Records this window's active environment into the session file so a restart
  // can restore it. Best-effort and silent on failure (a missing SessionService
  // must not break switching).
  void RecordWindowActiveEnvironment() const;

  void LogSnapshot(const char* reason) const;

  // Tells the tab strip to re-apply the environment filter.
  void NotifyEnvironmentChanged();

  raw_ptr<TabStripModel> model_;
  EnvironmentIndexMapper mapper_;

  raw_ptr<EnvironmentFilterObserver> filter_observer_ = nullptr;

  int switch_count_ = 0;
};

}  // namespace parra

#endif  // CHROME_BROWSER_PARRA_ENVIRONMENT_VISIBILITY_CONTROLLER_H_
