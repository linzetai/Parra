// Copyright 2026 The Parra Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CHROME_BROWSER_PARRA_PARRA_LIFECYCLE_POC_H_
#define CHROME_BROWSER_PARRA_PARRA_LIFECYCLE_POC_H_

#include <map>
#include <memory>
#include <string>
#include <vector>

#include "base/callback_list.h"
#include "base/memory/raw_ptr.h"
namespace tabs {
class TabModel;
}

namespace content {
class WebContents;
}

namespace base {
class RepeatingTimer;
}

namespace parra {

// PoC-only tab environment switching for Phase 2.0.
//
// This exists purely to answer one question with runtime evidence:
//
//   Can a tab be removed from TabStripModel and kept alive in our own
//   container, then re-inserted, WITHOUT breaking Chromium's tab lifecycle?
//
// It is deliberately NOT the EnvironmentRegistry from the design doc. There is
// no persistence, no host rules, no UI. It only moves tabs between the tab
// strip and a per-environment holding area, and exposes counters through a
// JavaScript bridge so the CDP harness can assert on them.
//
// Shutdown contract: every owned TabModel is released in the destructor, so
// closing the Browser window destroys all tabs exactly once even while an
// environment is "not shown".
class ParraLifecyclePoc {
 public:
  // Must be public: base::NoDestructor constructs the instance lazily and
  // cannot call a private constructor.
  ParraLifecyclePoc();

  // Returns nullptr unless --parra-poc-lifecycle was supplied.
  static ParraLifecyclePoc* GetInstance();

  // Creates the PoC tabs (tab_count per environment) in the last active
  // browser window, then drives `cycles` rounds of A->B->A->B on the message
  // loop. No-op unless --parra-poc-lifecycle was supplied.
  //
  // Called once from ChromeBrowserMainParts::PostBrowserStart().
  static void RunFromCommandLine();

  ParraLifecyclePoc(const ParraLifecyclePoc&) = delete;
  ParraLifecyclePoc& operator=(const ParraLifecyclePoc&) = delete;
  ~ParraLifecyclePoc();

  // Installs the JavaScript bridge. Safe to call more than once.
  void Initialize();

  // Moves every tab of the active environment into its holding area, then
  // moves every tab of `environment` into the tab strip.
  // Returns false if `environment` is unknown or has no tabs.
  bool SwitchToEnvironment(const std::string& environment);

  // Registers an existing tab as belonging to `environment`. Called for tabs
  // created by --parra-poc-lifecycle.
  void RegisterTab(const std::string& environment, content::WebContents* contents);

  // Moves `tab` out of the tab strip and into the holding area for
  // `environment` without destroying it. Used by RunFromCommandLine() when a
  // non-active environment is first set up.
  void ParkTab(const std::string& environment,
               std::unique_ptr<tabs::TabModel> tab);

  // Sets the environment considered active without moving any tab.
  void SetActiveEnvironmentForTesting(const std::string& environment) {
    active_environment_ = environment;
  }

 private:
  // Writes a structured one-line state dump to stderr so a run can be asserted
  // from a log file alone, without a debugger.
  void LogSnapshot(const char* reason) const;

  // Removes all tabs currently in the tab strip and stores them under
  // `environment`.
  void DetachCurrentTabs(const std::string& environment);

  // Moves stored tabs of `environment` back into the tab strip.
  void AttachTabs(const std::string& environment);

  std::string active_environment_;

  // Tabs that are currently NOT in the tab strip.
  std::map<std::string, std::vector<std::unique_ptr<tabs::TabModel>>>
      detached_tabs_;

  // environment -> WebContents most recently active, so switching back restores
  // the previously active tab rather than always index 0.
  std::map<std::string, raw_ptr<content::WebContents>> last_active_;

  // Counters the CDP harness asserts on. They prove which code paths ran
  // without needing a debugger attached.
  int switch_count_ = 0;
  int detach_count_ = 0;
  int attach_count_ = 0;

  // Owns the switch cycle timer. Must be a member: a local shared_ptr would
  // destroy the timer as soon as RunFromCommandLine() returns, so no cycle
  // would ever fire.
  std::unique_ptr<base::RepeatingTimer> cycle_timer_;

  // Observes Browser closure so the log records when/why the window vanished.
  base::CallbackListSubscription close_subscription_;

  bool initialized_ = false;
};

}  // namespace parra

#endif  // CHROME_BROWSER_PARRA_PARRA_LIFECYCLE_POC_H_
