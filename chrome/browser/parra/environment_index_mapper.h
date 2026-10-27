// Copyright 2026 The Parra Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CHROME_BROWSER_PARRA_ENVIRONMENT_INDEX_MAPPER_H_
#define CHROME_BROWSER_PARRA_ENVIRONMENT_INDEX_MAPPER_H_

#include <optional>
#include <string>
#include <vector>

#include "base/memory/raw_ptr.h"
#include "base/types/pass_key.h"
#include "chrome/browser/parra/environment_tag.h"
#include "components/tabs/public/tab_interface.h"

namespace tabs {
class TabInterface;
}  // namespace tabs

class TabStripModel;

namespace content {
class WebContents;
}  // namespace content

namespace parra {

// Translates between "visible" indices (what the user sees for the currently
// active environment) and "global" indices (positions in the native
// TabStripModel, which always holds every environment's tabs).
//
// The two are DIFFERENT COORDINATE SYSTEMS and must never be mixed:
//
//   Global index  -> accepted by TabStripModel APIs (ActivateTabAt,
//                    CloseWebContentsAt, GetWebContentsAt, ...).
//   Visible index -> only meaningful for the filtered tab strip UI and for
//                    per-environment operations.
//
// Passing one where the other is expected is a correctness bug that can close
// or activate the wrong environment's tab. Every conversion in this class
// therefore returns std::optional and fails loudly on invalid input rather than
// clamping.
//
// Design constraints, in order of importance:
//
//  1. TabStripModel stays the single source of truth and the only owner of
//     TabModels. This class never mutates ordering, never detaches, never
//     creates a second model.
//  2. Environment ownership is read from the WebContents (see
//     SetEnvironmentForContents), never inferred from URL, cookie, title or
//     position.
//  3. The environment -> index mapping is derived from the model on demand, so
//     it cannot go stale after insert / close / move / pin.
//  4. Only the "last active tab per environment" memory is cached, and it is
//     cached as a TabHandle (stable across reordering) rather than an index.
//  5. Unreadable state is reported, never silently filtered.
class EnvironmentIndexMapper {
 public:
  // The environment a tab belongs to when it carries no explicit environment
  // tag. Defined once in environment_tag.h so the low layer and this class can
  // never disagree about what an untagged tab is.
  static constexpr const char* kDefaultEnvironmentId =
      parra::kDefaultEnvironmentId;

  // Tags `contents` as belonging to `environment_id`.
  //
  // The ONLY writer of environment ownership. It lives on the WebContents
  // because the environment is a property of the page that owns the
  // StoragePartition, not of the tab strip's visual grouping. Storing it here
  // also means reordering a tab cannot silently reassign it.
  static void SetEnvironmentForContents(content::WebContents* contents,
                                        const std::string& environment_id);

  // Reads the environment id of `contents` without needing a TabStripModel.
  // Returns kDefaultEnvironmentId when untagged.
  static std::string GetEnvironmentForContents(
      const content::WebContents* contents);

  explicit EnvironmentIndexMapper(TabStripModel* model);
  EnvironmentIndexMapper(const EnvironmentIndexMapper&) = delete;
  EnvironmentIndexMapper& operator=(const EnvironmentIndexMapper&) = delete;
  ~EnvironmentIndexMapper();

  // ---- active environment ------------------------------------------------

  // The environment THIS WINDOW is showing.
  //
  // Backed by the window-scoped WindowEnvironmentState, not by a field on this
  // object and not by a process-global. A mapper is constructed per call site
  // (the tab strip controller builds one per query), so a member here would be
  // meaningless; and a process-global would make two windows share one
  // environment, which is exactly what Phase 2.2B-1.1 removes.
  //
  // When the model has no resolvable window (unit tests, teardown) this falls
  // back to the default environment rather than inventing state.
  const std::string& active_environment() const;
  void SetActiveEnvironment(const std::string& environment_id);

  // ---- queries scoped to an explicit environment -------------------------

  // Number of tabs belonging to `environment_id`.
  int GetVisibleCount(const std::string& environment_id) const;

  // Global indices of every tab in `environment_id`, in model order.
  std::vector<int> GetVisibleGlobalIndices(
      const std::string& environment_id) const;

  // True when the tab at `global_index` belongs to `environment_id`.
  bool IsTabVisible(const std::string& environment_id, int global_index) const;

  // ---- conversions (return nullopt on invalid input) ---------------------

  // visible index -> global index.
  std::optional<int> VisibleToGlobal(const std::string& environment_id,
                                     int visible_index) const;

  // global index -> visible index. nullopt when the tab is not in
  // `environment_id`.
  std::optional<int> GlobalToVisible(const std::string& environment_id,
                                     int global_index) const;

  // ---- navigation within one environment ---------------------------------

  // First tab of `environment_id`. nullopt when the environment is empty.
  std::optional<int> GetFirstVisibleGlobalIndex(
      const std::string& environment_id) const;

  // Next / previous tab of `environment_id` relative to `global_index`.
  // With `wrap`, an empty result wraps to the other end; without it, returns
  // nullopt at the boundary. Returns nullopt when the environment is empty.
  std::optional<int> GetNextVisibleGlobalIndex(const std::string& environment_id,
                                               int global_index,
                                               bool wrap) const;
  std::optional<int> GetPreviousVisibleGlobalIndex(
      const std::string& environment_id,
      int global_index,
      bool wrap) const;

  // ---- active tab per environment ---------------------------------------

  // Global index to activate when switching to `environment_id`: the tab
  // remembered via RememberActiveTab() if it still exists, otherwise the first
  // tab of that environment. nullopt when the environment has no tabs, which
  // callers must treat as "cannot switch" rather than "switch to nothing".
  std::optional<int> GetActiveGlobalIndexFor(
      const std::string& environment_id) const;

  // Remembers the tab at `global_index` as the active tab of
  // `environment_id`. Rejected (logged) if that tab does not belong to the
  // environment.
  void RememberActiveTab(const std::string& environment_id, int global_index);

  // Remembers the currently active tab as the active tab of the current
  // environment. No-op when the strip is empty.
  void RememberCurrentActiveTab();

  // ---- shared queries ----------------------------------------------------

  // Environment id of the tab at `global_index`; nullopt when out of range.
  std::optional<std::string> EnvironmentOfGlobalIndex(int global_index) const;

  // Global index of the currently active tab, or nullopt when none.
  std::optional<int> GetActiveGlobalIndex() const;

 private:
  // This model's window-scoped state, or nullptr when the model has no window
  // (unit tests, or the model is being torn down).
  class WindowEnvironmentState* GetWindowState() const;

  raw_ptr<TabStripModel> model_;
};

}  // namespace parra

#endif  // CHROME_BROWSER_PARRA_ENVIRONMENT_INDEX_MAPPER_H_
