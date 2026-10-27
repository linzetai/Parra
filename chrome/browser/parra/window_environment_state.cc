// Copyright 2026 The Parra Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chrome/browser/parra/window_environment_state.h"

#include <utility>

#include "base/check.h"
#include "base/logging.h"
#include "chrome/browser/parra/environment_tag.h"
#include "chrome/browser/ui/browser_window/public/browser_window_interface.h"
#include "chrome/browser/ui/tabs/tab_strip_model.h"
#include "chrome/browser/ui/tabs/tab_strip_model_delegate.h"
#include "content/public/browser/web_contents.h"
#include "ui/base/unowned_user_data/unowned_user_data_host.h"

namespace parra {

DEFINE_USER_DATA(WindowEnvironmentState);

// static
WindowEnvironmentState* WindowEnvironmentState::GetFor(
    BrowserWindowInterface* window) {
  if (!window)
    return nullptr;
  return Get(window->GetUnownedUserDataHost());
}

// static
std::unique_ptr<WindowEnvironmentState> WindowEnvironmentState::CreateFor(
    BrowserWindowInterface* window) {
  if (!window)
    return nullptr;

  // Already installed? Then somebody else created it and owns it; do not create a
  // second one. Returning null (rather than the existing pointer) keeps ownership
  // unambiguous: this function never hands out a pointer it does not own.
  if (GetFor(window))
    return nullptr;

  return std::unique_ptr<WindowEnvironmentState>(
      new WindowEnvironmentState(window));
}

// static
WindowEnvironmentState* WindowEnvironmentState::GetForModel(
    TabStripModel* model) {
  if (!model)
    return nullptr;
  TabStripModelDelegate* delegate = model->delegate();
  if (!delegate)
    return nullptr;
  return GetFor(delegate->GetBrowserWindowInterface());
}

WindowEnvironmentState::WindowEnvironmentState(BrowserWindowInterface* window)
    : window_(window),
      scoped_data_holder_(window->GetUnownedUserDataHost(), *this) {
  CHECK(window_);
}

WindowEnvironmentState::~WindowEnvironmentState() = default;

TabStripModel* WindowEnvironmentState::GetModel() const {
  if (!window_)
    return nullptr;
  return window_->GetTabStripModel();
}

bool WindowEnvironmentState::SetActiveEnvironment(
    const std::string& environment_id) {
  CHECK(!environment_id.empty())
      << "refusing to set an empty active environment";

  if (active_environment_ == environment_id)
    return false;

  LOG(INFO) << "parra-env: window "
            << (window_ ? window_->GetSessionID().id() : -1)
            << " active environment '" << active_environment_ << "' -> '"
            << environment_id << "'";
  active_environment_ = environment_id;
  ++generation_;
  return true;
}

void WindowEnvironmentState::ResetActiveEnvironment() {
  if (active_environment_ == kDefaultEnvironmentId)
    return;
  active_environment_ = kDefaultEnvironmentId;
  ++generation_;
}

bool WindowEnvironmentState::IsTabInEnvironment(
    const std::string& environment_id,
    int global_index) const {
  TabStripModel* model = GetModel();
  if (!model || !model->ContainsIndex(global_index))
    return false;
  return EnvironmentTag::Get(model->GetWebContentsAt(global_index)) ==
         environment_id;
}

std::optional<int> WindowEnvironmentState::GetLastActiveIndex(
    const std::string& environment_id) const {
  TabStripModel* model = GetModel();
  if (!model)
    return std::nullopt;

  // Prefer the remembered tab, but validate it on every read: a closed tab, a tab
  // moved to another window, or a tab whose environment changed all invalidate
  // the memory, and in those cases the first tab of the environment is a better
  // answer than a stale index.
  for (const LastActiveEntry& entry : last_active_) {
    if (entry.environment_id != environment_id)
      continue;

    tabs::TabInterface* tab = entry.handle.Get();
    if (!tab)
      break;

    const int index = model->GetIndexOfWebContents(tab->GetContents());
    if (index == TabStripModel::kNoTab)
      break;

    if (!IsTabInEnvironment(environment_id, index))
      break;

    return index;
  }

  for (int i = 0; i < model->count(); ++i) {
    if (IsTabInEnvironment(environment_id, i))
      return i;
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
  if (!model)
    return;

  content::WebContents* contents = model->GetWebContentsAt(global_index);
  if (!contents)
    return;

  // MaybeGetFromContents(): during teardown the model can briefly hold a
  // WebContents that is no longer a tab, and bookkeeping must never be the thing
  // that crashes the browser.
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
  if (!model)
    return;

  const int active = model->active_index();
  if (active == TabStripModel::kNoTab || !model->ContainsIndex(active))
    return;

  // Remember under the tab's OWN environment, never under active_environment_.
  //
  // The two can disagree whenever a native Chromium path activates a tab without
  // going through the Parra environment switcher (Ctrl+Tab, a bookmark, session
  // restore). Trusting active_environment_ there would reject the write and leave
  // that environment with a stale memory, which later shows up as "switching back
  // lands on the wrong tab". The tag is the single definition of ownership, so
  // remembering by tag cannot be wrong.
  const std::string owner = EnvironmentTag::Get(model->GetWebContentsAt(active));
  RememberActiveTab(owner, active);
}

}  // namespace parra
