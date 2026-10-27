// Copyright 2026 The Parra Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chrome/browser/parra/environment_index_mapper.h"

#include <algorithm>
#include <utility>

#include "base/check.h"
#include "base/logging.h"
#include "chrome/browser/parra/environment_tag.h"
#include "chrome/browser/parra/window_environment_state.h"
#include "chrome/browser/ui/tabs/tab_model.h"
#include "chrome/browser/ui/tabs/tab_strip_model.h"
#include "components/tabs/public/tab_interface.h"
#include "content/public/browser/web_contents.h"

namespace parra {

EnvironmentIndexMapper::EnvironmentIndexMapper(TabStripModel* model)
    : model_(model) {
  CHECK(model_);
}

EnvironmentIndexMapper::~EnvironmentIndexMapper() = default;

// static
void EnvironmentIndexMapper::SetEnvironmentForContents(
    content::WebContents* contents,
    const std::string& environment_id) {
  // Delegated, so there is exactly one implementation of environment ownership.
  // EnvironmentTag lives in the low layer because session restore needs it
  // without being able to depend on the tab-strip layer.
  EnvironmentTag::Set(contents, environment_id);
}

// static
std::string EnvironmentIndexMapper::GetEnvironmentForContents(
    const content::WebContents* contents) {
  return EnvironmentTag::Get(contents);
}

class WindowEnvironmentState* EnvironmentIndexMapper::GetWindowState() const {
  return WindowEnvironmentState::GetForModel(model_);
}

void EnvironmentIndexMapper::SetActiveEnvironment(
    const std::string& environment_id) {
  WindowEnvironmentState* state = GetWindowState();
  if (!state) {
    // No resolvable window: unit tests and teardown. Dropping the write is
    // correct here -- inventing process-wide state to "store" it would recreate
    // exactly the bug this class was changed to remove.
    LOG(WARNING) << "parra: no window for this tab strip; ignoring environment "
                    "change to '"
                 << environment_id << "'";
    return;
  }
  state->SetActiveEnvironment(environment_id);
}

const std::string& EnvironmentIndexMapper::active_environment() const {
  WindowEnvironmentState* state = GetWindowState();
  if (!state) {
    // Static storage, not a temporary: this is returned by reference, and the
    // caller must be able to hold it. kDefaultEnvironmentId is a compile-time
    // constant, so it lives for the whole program.
    static const std::string kFallback(kDefaultEnvironmentId);
    return kFallback;
  }
  return state->active_environment();
}

std::optional<std::string> EnvironmentIndexMapper::EnvironmentOfGlobalIndex(
    int global_index) const {
  // Out of range is reported as "no answer", never mapped to the default
  // environment: callers must be able to tell "this index does not exist" apart
  // from "this tab belongs to Personal".
  if (!model_->ContainsIndex(global_index))
    return std::nullopt;

  content::WebContents* contents = model_->GetWebContentsAt(global_index);
  if (!contents)
    return std::nullopt;

  return GetEnvironmentForContents(contents);
}

bool EnvironmentIndexMapper::IsTabVisible(const std::string& environment_id,
                                          int global_index) const {
  const std::optional<std::string> owner =
      EnvironmentOfGlobalIndex(global_index);
  return owner.has_value() && *owner == environment_id;
}

int EnvironmentIndexMapper::GetVisibleCount(
    const std::string& environment_id) const {
  int count = 0;
  for (int i = 0; i < model_->count(); ++i) {
    if (IsTabVisible(environment_id, i))
      ++count;
  }
  return count;
}

std::vector<int> EnvironmentIndexMapper::GetVisibleGlobalIndices(
    const std::string& environment_id) const {
  std::vector<int> result;
  for (int i = 0; i < model_->count(); ++i) {
    if (IsTabVisible(environment_id, i))
      result.push_back(i);
  }
  return result;
}

std::optional<int> EnvironmentIndexMapper::VisibleToGlobal(
    const std::string& environment_id,
    int visible_index) const {
  if (visible_index < 0)
    return std::nullopt;

  const std::vector<int> indices = GetVisibleGlobalIndices(environment_id);
  if (visible_index >= static_cast<int>(indices.size()))
    return std::nullopt;
  return indices[visible_index];
}

std::optional<int> EnvironmentIndexMapper::GlobalToVisible(
    const std::string& environment_id,
    int global_index) const {
  if (!IsTabVisible(environment_id, global_index))
    return std::nullopt;

  int visible = 0;
  for (int i = 0; i < global_index; ++i) {
    if (IsTabVisible(environment_id, i))
      ++visible;
  }
  return visible;
}

std::optional<int> EnvironmentIndexMapper::GetFirstVisibleGlobalIndex(
    const std::string& environment_id) const {
  for (int i = 0; i < model_->count(); ++i) {
    if (IsTabVisible(environment_id, i))
      return i;
  }
  return std::nullopt;
}

std::optional<int> EnvironmentIndexMapper::GetNextVisibleGlobalIndex(
    const std::string& environment_id,
    int global_index,
    bool wrap) const {
  const std::vector<int> indices = GetVisibleGlobalIndices(environment_id);
  if (indices.empty())
    return std::nullopt;

  for (int index : indices) {
    if (index > global_index)
      return index;
  }
  // Past the last member. Wrapping is explicit so a caller that wants
  // stop-at-the-end is never silently given a cycle.
  return wrap ? std::optional<int>(indices.front()) : std::nullopt;
}

std::optional<int> EnvironmentIndexMapper::GetPreviousVisibleGlobalIndex(
    const std::string& environment_id,
    int global_index,
    bool wrap) const {
  const std::vector<int> indices = GetVisibleGlobalIndices(environment_id);
  if (indices.empty())
    return std::nullopt;

  for (auto it = indices.rbegin(); it != indices.rend(); ++it) {
    if (*it < global_index)
      return *it;
  }
  return wrap ? std::optional<int>(indices.back()) : std::nullopt;
}

std::optional<int> EnvironmentIndexMapper::GetActiveGlobalIndexFor(
    const std::string& environment_id) const {
  // Delegated to the window: the memory is a TabHandle list that names tabs in
  // THIS window's model, so it belongs with the window, not with this transient
  // mapper object and not with the process.
  WindowEnvironmentState* state = GetWindowState();
  if (state)
    return state->GetLastActiveIndex(environment_id);

  // No window (unit tests / teardown): fall back to the deterministic answer
  // rather than reporting "cannot switch".
  return GetFirstVisibleGlobalIndex(environment_id);
}

void EnvironmentIndexMapper::RememberActiveTab(const std::string& environment_id,
                                               int global_index) {
  WindowEnvironmentState* state = GetWindowState();
  if (!state) {
    LOG(WARNING) << "parra: no window for this tab strip; not remembering "
                    "global index "
                 << global_index << " for environment '" << environment_id
                 << "'";
    return;
  }
  state->RememberActiveTab(environment_id, global_index);
}

void EnvironmentIndexMapper::RememberCurrentActiveTab() {
  WindowEnvironmentState* state = GetWindowState();
  if (!state)
    return;
  state->RememberCurrentActiveTab();
}

std::optional<int> EnvironmentIndexMapper::GetActiveGlobalIndex() const {
  const int index = model_->active_index();
  if (index == TabStripModel::kNoTab)
    return std::nullopt;
  if (!model_->ContainsIndex(index))
    return std::nullopt;
  return index;
}

}  // namespace parra
