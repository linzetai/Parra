// Copyright 2026 The Parra Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CHROME_BROWSER_PARRA_ENVIRONMENT_INDEX_MAPPER_H_
#define CHROME_BROWSER_PARRA_ENVIRONMENT_INDEX_MAPPER_H_

#include <optional>
#include <string>
#include <vector>

#include "base/memory/raw_ptr.h"

namespace tabs {
class TabInterface;
class TabStripModel;
}  // namespace tabs

class TabStripModel;

namespace content {
class WebContents;
}  // namespace content

namespace parra {

// Translates between "visible" tab indices (what the user sees for the active
// environment) and the underlying TabStripModel indices (which always include
// every environment's tabs).
//
// Design constraints, in order of importance:
//
//  1. TabStripModel keeps its original index definition. Nothing here mutates
//     the model's ordering, and no tab ever leaves the model.
//  2. EnvironmentId is stored as tab user data, not derived from position, so
//     moving or pinning a tab cannot silently reassign its environment.
//  3. This class never owns a WebContents or a TabModel. It only answers
//     questions about the model's current contents.
//  4. The mapping is derived on demand from the model rather than cached, so it
//     cannot drift out of sync after insert / close / move / pin.
//  5. Inconsistencies are reported loudly. A tab with no environment user data
//     is an error, not something to filter out.
//
// The mapper is deliberately small: it covers exactly what the visibility PoC
// needs. It is not a general framework.
class EnvironmentIndexMapper {
 public:
  // The environment a tab belongs to. Tabs without explicit environment user
  // data belong to the default environment, which matches what a stock
  // Chromium tab would do.
  static constexpr char kDefaultEnvironmentId[] = "personal";

  // Tags `contents` as belonging to `environment_id`.
  //
  // This is the ONLY writer of environment ownership. It is a WebContents user
  // data entry, deliberately not a TabStripModel field: the environment is a
  // property of the page (which owns the StoragePartition), and tying it to the
  // content means moving a tab in the strip cannot silently reassign it.
  static void SetEnvironmentForContents(content::WebContents* contents,
                                        const std::string& environment_id);

  // Reads the environment id of `contents` without a TabStripModel.
  // Returns kDefaultEnvironmentId when untagged.
  static std::string GetEnvironmentForContents(
      const content::WebContents* contents);

  explicit EnvironmentIndexMapper(TabStripModel* model);
  EnvironmentIndexMapper(const EnvironmentIndexMapper&) = delete;
  EnvironmentIndexMapper& operator=(const EnvironmentIndexMapper&) = delete;
  ~EnvironmentIndexMapper();

  // The environment currently being shown in the tab strip.
  const std::string& active_environment() const { return active_environment_; }
  void SetActiveEnvironment(const std::string& environment_id);

  // Number of tabs belonging to `environment_id`.
  int GetVisibleCount(const std::string& environment_id) const;

  // Model indices of every tab in `environment_id`, in model order.
  std::vector<int> GetVisibleModelIndices(
      const std::string& environment_id) const;

  // visible_index -> model index. Returns nullopt when out of range.
  // `visible_index` 0 is the first tab of `environment_id` in model order.
  std::optional<int> VisibleToModel(const std::string& environment_id,
                                    int visible_index) const;

  // model index -> visible index. Returns nullopt when the tab does not belong
  // to `environment_id`.
  std::optional<int> ModelToVisible(const std::string& environment_id,
                                    int model_index) const;

  // True when the tab at `model_index` belongs to `environment_id`.
  bool IsModelIndexVisible(const std::string& environment_id,
                           int model_index) const;

  // Next / previous visible tab, wrapping. Returns nullopt only when the
  // environment has no tabs at all.
  std::optional<int> NextVisibleModelIndex(const std::string& environment_id,
                                           int from_model_index) const;
  std::optional<int> PreviousVisibleModelIndex(const std::string& environment_id,
                                               int from_model_index) const;

  // Model index of the tab to activate when switching to `environment_id`:
  // the remembered tab if it still exists, else the first visible one.
  std::optional<int> RestoreIndexFor(const std::string& environment_id) const;

  // Remembers `model_index` as the active tab of `environment_id`.
  void RememberActiveTab(const std::string& environment_id, int model_index);

  // Environment id of the tab at `model_index`, or nullopt when out of range.
  // Never silently defaults: callers can distinguish "not in this environment"
  // from "does not exist".
  std::optional<std::string> EnvironmentOfModelIndex(int model_index) const;

 private:
  raw_ptr<TabStripModel> model_;

  std::string active_environment_ = kDefaultEnvironmentId;

  // environment id -> last active model index. Model indices are positional and
  // therefore not stable, so this is only a hint; RestoreIndexFor() validates
  // it before use.
  std::vector<std::pair<std::string, int>> last_active_;
};

}  // namespace parra

#endif  // CHROME_BROWSER_PARRA_ENVIRONMENT_INDEX_MAPPER_H_
