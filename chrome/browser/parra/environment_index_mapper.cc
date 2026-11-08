// Copyright 2026 The Parra Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chrome/browser/parra/environment_index_mapper.h"

#include <algorithm>
#include <utility>

#include "base/check.h"
#include "base/logging.h"
#include "chrome/browser/parra/environment_tag.h"
#include "chrome/browser/parra/parra_storage_partition_util.h"
#include "chrome/browser/parra/window_environment_state.h"
#include "chrome/browser/ui/tabs/tab_model.h"
#include "chrome/browser/ui/tabs/tab_strip_model.h"
#include "components/tabs/public/tab_interface.h"
#include "content/public/browser/security_principal.h"
#include "content/public/browser/site_instance.h"
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
    content::WebContents* contents) {
  if (!contents) {
    return kDefaultEnvironmentId;
  }

  if (BrowserLevelTab::Is(contents)) {
    return std::string();
  }

  // The tag is authoritative when present: it is the only record that still
  // knows an environment's ID after that environment has been deleted.
  if (EnvironmentTag::HasTag(contents)) {
    return EnvironmentTag::Get(contents);
  }

  // NO TAG: FALL BACK TO THE STORAGE PARTITION.
  //
  // An untagged tab is NOT necessarily Personal. Two different tabs reach here:
  //
  //   * an ordinary Chromium tab, which really is Personal (default partition),
  //   * a tab created directly inside `parra/<id>` that never got a tag.
  //
  // Reporting the second as Personal is not cosmetic. The strip would draw that
  // tab next to Personal's own tabs while the window believed it was on
  // Personal, so every environment's tabs appeared side by side -- exactly what
  // the M1 build did on startup, because the startup path adds tabs with
  // TabStripModel::AddWebContents() without writing a tag.
  //
  // The partition is the ground truth for isolation regardless: the page's
  // cookies, storage and host mappings came from it. Reading the ID back out of
  // it therefore answers "which environment is this tab really in" even when
  // nobody wrote a tag.
  //
  // Deliberately NOT a fix-on-write (calling EnvironmentTag::Set here): this is
  // a query reached from layout, so writing would make painting mutate browser
  // state and could re-enter the very layout asking the question.
  content::SiteInstance* site_instance = contents->GetSiteInstance();
  if (!site_instance) {
    return kDefaultEnvironmentId;
  }

  // SecurityPrincipal reports the DEFAULT partition for a site-unassigned
  // instance, so a tab that has not committed a document yet still resolves to
  // Personal. The tag therefore stays the primary source; this is only the
  // fallback for a tab that has a real document but no tag.
  const std::string from_partition = GetEnvironmentIdFromPartitionConfig(
      site_instance->GetSecurityPrincipal().GetStoragePartitionConfig());
  if (!from_partition.empty()) {
    return from_partition;
  }

  return kDefaultEnvironmentId;
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
  if (!model_->ContainsIndex(global_index)) {
    return std::nullopt;
  }

  content::WebContents* contents = model_->GetWebContentsAt(global_index);
  if (!contents) {
    return std::nullopt;
  }

  if (BrowserLevelTab::Is(contents)) {
    return std::nullopt;
  }
  return GetEnvironmentForContents(contents);
}

bool EnvironmentIndexMapper::IsTabVisible(const std::string& environment_id,
                                          int global_index) const {
  if (!model_->ContainsIndex(global_index)) {
    return false;
  }
  if (BrowserLevelTab::Is(model_->GetWebContentsAt(global_index))) {
    return true;
  }
  const std::optional<std::string> owner =
      EnvironmentOfGlobalIndex(global_index);
  return owner.has_value() && *owner == environment_id;
}

int EnvironmentIndexMapper::GetEnvironmentTabCount(
    const std::string& environment_id) const {
  int count = 0;
  for (int i = 0; i < model_->count(); ++i) {
    const auto owner = EnvironmentOfGlobalIndex(i);
    if (owner && *owner == environment_id) {
      ++count;
    }
  }
  return count;
}

std::vector<int> EnvironmentIndexMapper::GetEnvironmentTabIndices(
    const std::string& environment_id) const {
  std::vector<int> result;
  for (int i = 0; i < model_->count(); ++i) {
    const auto owner = EnvironmentOfGlobalIndex(i);
    if (owner && *owner == environment_id) {
      result.push_back(i);
    }
  }
  return result;
}

std::vector<int> EnvironmentIndexMapper::GetVisibleGlobalIndices(
    const std::string& environment_id) const {
  std::vector<int> result;
  for (int i = 0; i < model_->count(); ++i) {
    if (IsTabVisible(environment_id, i)) {
      result.push_back(i);
    }
  }
  return result;
}

std::optional<int> EnvironmentIndexMapper::VisibleToGlobal(
    const std::string& environment_id,
    int visible_index) const {
  if (visible_index < 0) {
    return std::nullopt;
  }

  const std::vector<int> indices = GetVisibleGlobalIndices(environment_id);
  if (visible_index >= static_cast<int>(indices.size())) {
    return std::nullopt;
  }
  return indices[visible_index];
}

std::optional<int> EnvironmentIndexMapper::GlobalToVisible(
    const std::string& environment_id,
    int global_index) const {
  if (!IsTabVisible(environment_id, global_index)) {
    return std::nullopt;
  }

  int visible = 0;
  for (int i = 0; i < global_index; ++i) {
    if (IsTabVisible(environment_id, i)) {
      ++visible;
    }
  }
  return visible;
}

std::optional<int> EnvironmentIndexMapper::GetFirstVisibleGlobalIndex(
    const std::string& environment_id) const {
  for (int i = 0; i < model_->count(); ++i) {
    if (IsTabVisible(environment_id, i)) {
      return i;
    }
  }
  return std::nullopt;
}

std::optional<int> EnvironmentIndexMapper::GetNextVisibleGlobalIndex(
    const std::string& environment_id,
    int global_index,
    bool wrap) const {
  const std::vector<int> indices = GetVisibleGlobalIndices(environment_id);
  if (indices.empty()) {
    return std::nullopt;
  }

  for (int index : indices) {
    if (index > global_index) {
      return index;
    }
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
  if (indices.empty()) {
    return std::nullopt;
  }

  for (auto it = indices.rbegin(); it != indices.rend(); ++it) {
    if (*it < global_index) {
      return *it;
    }
  }
  return wrap ? std::optional<int>(indices.back()) : std::nullopt;
}

std::optional<int> EnvironmentIndexMapper::GetActiveGlobalIndexFor(
    const std::string& environment_id) const {
  // Delegated to the window: the memory is a TabHandle list that names tabs in
  // THIS window's model, so it belongs with the window, not with this transient
  // mapper object and not with the process.
  WindowEnvironmentState* state = GetWindowState();
  if (state) {
    std::optional<int> remembered = state->GetLastActiveIndex(environment_id);

    // VALIDATE THE REMEMBERED INDEX BEFORE TRUSTING IT.
    //
    // The memory answers "which tab was this environment on last time", which
    // is the right intent, but it is not a guarantee: indexes shift when tabs
    // are opened or closed, and the remembered tab may since have been closed.
    // Returning a stale index makes SwitchTo() activate a tab that belongs to
    // ANOTHER environment -- the one failure this whole design must not have.
    // Observed with the M1 build: switching to Personal activated g2, which
    // belonged to test-b, and the invariant check caught it:
    //
    //   INVARIANT VIOLATED (after-switch): active tab g2 belongs to 'test-b'
    //   but active environment is 'personal'
    //
    // Checking the tab's actual ownership (via GetEnvironmentForContents, which
    // also covers tabs whose tag was never written) turns a stale memory into a
    // miss, and a miss falls through to the deterministic first-visible index
    // below.
    if (remembered.has_value() && model_->ContainsIndex(*remembered)) {
      content::WebContents* contents = model_->GetWebContentsAt(*remembered);
      if (contents && GetEnvironmentForContents(contents) == environment_id) {
        return remembered;
      }
      LOG(INFO)
          << "parra: remembered index " << *remembered << " for environment '"
          << environment_id
          << "' no longer belongs to it; using the first visible tab instead";
    }
  }

  // No usable memory (unit tests, teardown, or a stale index): fall back to the
  // deterministic answer rather than reporting "cannot switch".
  const auto owned = GetEnvironmentTabIndices(environment_id);
  return owned.empty() ? std::nullopt : std::optional<int>(owned.front());
}

void EnvironmentIndexMapper::RememberActiveTab(
    const std::string& environment_id,
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
  if (!state) {
    return;
  }
  state->RememberCurrentActiveTab();
}

std::optional<int> EnvironmentIndexMapper::GetActiveGlobalIndex() const {
  const int index = model_->active_index();
  if (index == TabStripModel::kNoTab) {
    return std::nullopt;
  }
  if (!model_->ContainsIndex(index)) {
    return std::nullopt;
  }
  return index;
}

}  // namespace parra
