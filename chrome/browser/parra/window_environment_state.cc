// Copyright 2026 The Parra Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chrome/browser/parra/window_environment_state.h"

#include <utility>

#include "base/check.h"
#include "base/logging.h"
#include "chrome/browser/parra/environment_index_mapper.h"
#include "chrome/browser/ui/browser_window/public/browser_window_interface.h"
#include "chrome/browser/ui/tabs/tab_strip_model.h"
#include "chrome/browser/ui/tabs/tab_strip_model_delegate.h"
#include "content/public/browser/web_contents.h"
#include "ui/base/unowned_user_data/unowned_user_data_host.h"
namespace parra {

DEFINE_USER_DATA(WindowEnvironmentState);

namespace {

// Live instance count, used by the lifetime tests in docs/parra/64 and 67.
//
// This exists because "no leak" cannot be observed from the outside: a leaked
// WindowEnvironmentState has no user-visible symptom, so a regression would
// only surface later as a use-after-free of its raw `window_` pointer. Counting
// constructions and destructions makes the property directly assertable --
// create and close windows N times, and the live count must return to its
// starting value.
int g_live_states = 0;
int g_total_created = 0;

}  // namespace

int WindowEnvironmentState::live_instance_count() {
  return g_live_states;
}

int WindowEnvironmentState::total_created_count() {
  return g_total_created;
}

// static
WindowEnvironmentState* WindowEnvironmentState::GetFor(
    BrowserWindowInterface* window) {
  if (!window) {
    return nullptr;
  }
  return Get(window->GetUnownedUserDataHost());
}

// static
std::unique_ptr<WindowEnvironmentState> WindowEnvironmentState::CreateFor(
    BrowserWindowInterface* window) {
  if (!window) {
    return nullptr;
  }

  // Already installed? Then somebody else created it and owns it; do not create
  // a second one. Returning null (rather than the existing pointer) keeps
  // ownership unambiguous: this function never hands out a pointer it does not
  // own.
  if (GetFor(window)) {
    return nullptr;
  }

  return std::unique_ptr<WindowEnvironmentState>(
      new WindowEnvironmentState(window));
}

// static
WindowEnvironmentState* WindowEnvironmentState::GetForModel(
    TabStripModel* model) {
  if (!model) {
    return nullptr;
  }
  TabStripModelDelegate* delegate = model->delegate();
  if (!delegate) {
    return nullptr;
  }
  return GetFor(delegate->GetBrowserWindowInterface());
}

WindowEnvironmentState::WindowEnvironmentState(BrowserWindowInterface* window)
    : window_(window),
      scoped_data_holder_(window->GetUnownedUserDataHost(), *this) {
  CHECK(window_);
  ++g_live_states;
  ++g_total_created;
  LOG(INFO) << "parra-state: constructed live=" << g_live_states
            << " total=" << g_total_created;
}

WindowEnvironmentState::~WindowEnvironmentState() {
  --g_live_states;
  LOG(INFO) << "parra-state: destroyed live=" << g_live_states
            << " total_created=" << g_total_created;
}

TabStripModel* WindowEnvironmentState::GetModel() const {
  if (!window_) {
    return nullptr;
  }
  return window_->GetTabStripModel();
}

bool WindowEnvironmentState::SetActiveEnvironment(
    const std::string& environment_id) {
  CHECK(!environment_id.empty())
      << "refusing to set an empty active environment";

  if (active_environment_ == environment_id) {
    return false;
  }

  LOG(INFO) << "parra-env: window "
            << (window_ ? window_->GetSessionID().id() : -1)
            << " active environment '" << active_environment_ << "' -> '"
            << environment_id << "'";
  active_environment_ = environment_id;
  ++generation_;

  // Notified AFTER the state is updated, so a listener that reads
  // active_environment() inside the callback sees the new value. Notifying
  // before would force every listener to trust a value passed alongside, and
  // any listener that ignored it would silently act on stale state.
  //
  // Coalesced: inside a deferral, only the fact that "something changed" is
  // recorded, and the single delivery happens when the outermost deferral ends.
  if (notification_deferral_depth_ > 0) {
    notification_pending_ = true;
  } else {
    active_environment_changed_.Notify();
  }
  return true;
}

bool WindowEnvironmentState::ReserveBrowserLevelTab(
    content::WebContents* contents) {
  if (!contents || browser_level_reservation_) {
    return false;
  }
  browser_level_reservation_ = contents->GetWeakPtr();
  return true;
}

void WindowEnvironmentState::ReleaseBrowserLevelTabReservation(
    content::WebContents* contents) {
  if (browser_level_reservation_.get() == contents) {
    browser_level_reservation_.reset();
  }
}

bool WindowEnvironmentState::BrowserLevelTabSlotAvailable(
    content::WebContents* contents) const {
  return !browser_level_reservation_ ||
         browser_level_reservation_.get() == contents;
}

base::CallbackListSubscription
WindowEnvironmentState::AddActiveEnvironmentChangedCallback(
    ActiveEnvironmentChangedCallback callback) {
  return active_environment_changed_.Add(std::move(callback));
}

WindowEnvironmentState::ScopedNotificationDeferral::ScopedNotificationDeferral(
    WindowEnvironmentState* state)
    : state_(state) {
  if (state_) {
    ++state_->notification_deferral_depth_;
  }
}

WindowEnvironmentState::ScopedNotificationDeferral::
    ~ScopedNotificationDeferral() {
  if (!state_) {
    return;
  }

  // Decrement before delivering: a listener that changes the environment again
  // (or that triggers a nested deferral) must see the un-deferred state, not a
  // depth that this destructor is still holding open.
  if (--state_->notification_deferral_depth_ > 0) {
    return;
  }

  if (!state_->notification_pending_) {
    return;
  }

  state_->notification_pending_ = false;
  state_->active_environment_changed_.Notify();
}

void WindowEnvironmentState::ResetActiveEnvironment() {
  if (active_environment_ == kDefaultEnvironmentId) {
    return;
  }
  active_environment_ = kDefaultEnvironmentId;
  ++generation_;
  // A reset is an environment change like any other: a listener that filters
  // the strip must repaint for `personal`. Skipping the notification here would
  // leave the strip showing the environment that was just left.
  if (notification_deferral_depth_ > 0) {
    notification_pending_ = true;
  } else {
    active_environment_changed_.Notify();
  }
}

void WindowEnvironmentState::BeginEnvironmentAdoptionSuppression() {
  ++adoption_suppression_depth_;
  // Logged (rather than silently counted) because the failure this guards
  // against is INVISIBLE: a depth that never returns to zero makes the window
  // stop adopting environments, which looks like "the switcher stopped
  // working", not like a missing decrement. Printing both ends makes an
  // unbalanced pair visible in the log even when nothing else goes wrong.
  LOG(INFO) << "parra-env: adoption suppression begin -> depth="
            << adoption_suppression_depth_;
}

void WindowEnvironmentState::EndEnvironmentAdoptionSuppression() {
  if (adoption_suppression_depth_ > 0) {
    --adoption_suppression_depth_;
  } else {
    // Reaching zero here would be an underflow, i.e. an End without a Begin.
    // Refusing to go negative keeps EnvironmentAdoptionSuppressed() meaningful,
    // and saying so is better than silently clamping.
    LOG(WARNING) << "parra-env: adoption suppression end called with depth 0; "
                    "ignoring (an End ran without a matching Begin)";
  }
  LOG(INFO) << "parra-env: adoption suppression end -> depth="
            << adoption_suppression_depth_;
}

bool WindowEnvironmentState::IsTabInEnvironment(
    const std::string& environment_id,
    int global_index) const {
  TabStripModel* model = GetModel();
  if (!model || !model->ContainsIndex(global_index)) {
    return false;
  }
  content::WebContents* contents = model->GetWebContentsAt(global_index);
  return !BrowserLevelTab::Is(contents) &&
         EnvironmentIndexMapper::GetEnvironmentForContents(contents) ==
             environment_id;
}

std::optional<int> WindowEnvironmentState::GetLastActiveIndex(
    const std::string& environment_id) const {
  TabStripModel* model = GetModel();
  if (!model) {
    return std::nullopt;
  }

  // Prefer the remembered tab, but validate it on every read: a closed tab, a
  // tab moved to another window, or a tab whose environment changed all
  // invalidate the memory, and in those cases the first tab of the environment
  // is a better answer than a stale index.
  for (const LastActiveEntry& entry : last_active_) {
    if (entry.environment_id != environment_id) {
      continue;
    }

    tabs::TabInterface* tab = entry.handle.Get();
    if (!tab) {
      break;
    }

    const int index = model->GetIndexOfWebContents(tab->GetContents());
    if (index == TabStripModel::kNoTab) {
      break;
    }

    if (!IsTabInEnvironment(environment_id, index)) {
      break;
    }

    return index;
  }

  for (int i = 0; i < model->count(); ++i) {
    if (IsTabInEnvironment(environment_id, i)) {
      return i;
    }
  }
  return std::nullopt;
}

void WindowEnvironmentState::RememberActiveTab(
    const std::string& environment_id,
    int global_index) {
  if (!IsTabInEnvironment(environment_id, global_index)) {
    LOG(ERROR) << "parra: refusing to remember global index " << global_index
               << " as active for environment '" << environment_id
               << "': that tab does not belong to this environment";
    return;
  }

  TabStripModel* model = GetModel();
  if (!model) {
    return;
  }

  content::WebContents* contents = model->GetWebContentsAt(global_index);
  if (!contents) {
    return;
  }

  // MaybeGetFromContents(): during teardown the model can briefly hold a
  // WebContents that is no longer a tab, and bookkeeping must never be the
  // thing that crashes the browser.
  tabs::TabInterface* tab = tabs::TabInterface::MaybeGetFromContents(contents);
  if (!tab) {
    LOG(ERROR) << "parra: cannot remember global index " << global_index
               << ": that WebContents is not a tab";
    return;
  }

  const tabs::TabHandle handle = tab->GetHandle();
  for (LastActiveEntry& entry : last_active_) {
    if (entry.environment_id == environment_id) {
      entry.handle = handle;
      return;
    }
  }
  last_active_.push_back({environment_id, handle});
}

void WindowEnvironmentState::RememberCurrentActiveTab() {
  TabStripModel* model = GetModel();
  if (!model) {
    return;
  }

  const int active = model->active_index();
  if (active == TabStripModel::kNoTab || !model->ContainsIndex(active)) {
    return;
  }

  // Remember under the tab's OWN environment, never under active_environment_.
  //
  // The two can disagree whenever a native Chromium path activates a tab
  // without going through the Parra environment switcher (Ctrl+Tab, a bookmark,
  // session restore). Trusting active_environment_ there would reject the write
  // and leave that environment with stale memory. Use the same tag/partition
  // resolution as visibility, so an untagged tab in a named partition is
  // remembered by its owner.
  const std::string owner = EnvironmentIndexMapper::GetEnvironmentForContents(
      model->GetWebContentsAt(active));
  if (!owner.empty()) {
    RememberActiveTab(owner, active);
  }
}

}  // namespace parra
