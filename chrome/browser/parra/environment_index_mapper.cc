// Copyright 2026 The Parra Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chrome/browser/parra/environment_index_mapper.h"

#include <algorithm>

#include "base/check.h"
#include "base/logging.h"
#include "chrome/browser/ui/tabs/tab_model.h"
#include "chrome/browser/ui/tabs/tab_strip_model.h"
#include "components/tabs/public/tab_interface.h"
#include "content/public/browser/web_contents.h"

namespace parra {

namespace {

// Key used to store the environment id on a WebContents.
//
// WebContents::SetUserData() is used rather than TabInterface's
// UnownedUserDataHost for a practical reason: it survives the whole lifetime of
// the page, is trivially inspectable from tests, and does not require a
// template wrapper. It also proves that the environment is a property of the
// CONTENT (which is what actually owns the StoragePartition), not of the tab
// strip's visual grouping.
class EnvironmentUserData : public base::SupportsUserData::Data {
 public:
  static constexpr char kKey[] = "parra.environment";

  explicit EnvironmentUserData(std::string environment_id)
      : environment_id_(std::move(environment_id)) {}

  const std::string& environment_id() const { return environment_id_; }

 private:
  std::string environment_id_;
};

constexpr char EnvironmentUserData::kKey[];

}  // namespace

EnvironmentIndexMapper::EnvironmentIndexMapper(TabStripModel* model)
    : model_(model) {
  CHECK(model_);
}

EnvironmentIndexMapper::~EnvironmentIndexMapper() = default;

// static
void EnvironmentIndexMapper::SetEnvironmentForContents(
    content::WebContents* contents,
    const std::string& environment_id) {
  CHECK(contents);
  CHECK(!environment_id.empty());
  contents->SetUserData(EnvironmentUserData::kKey,
                        std::make_unique<EnvironmentUserData>(environment_id));
}

// static
std::string EnvironmentIndexMapper::GetEnvironmentForContents(
    const content::WebContents* contents) {
  if (!contents)
    return kDefaultEnvironmentId;

  auto* data = static_cast<EnvironmentUserData*>(
      contents->GetUserData(EnvironmentUserData::kKey));
  if (!data)
    return kDefaultEnvironmentId;
  return data->environment_id();
}

void EnvironmentIndexMapper::SetActiveEnvironment(
    const std::string& environment_id) {
  CHECK(!environment_id.empty());
  active_environment_ = environment_id;
}

std::optional<std::string> EnvironmentIndexMapper::EnvironmentOfModelIndex(
    int model_index) const {
  if (!model_->ContainsIndex(model_index))
    return std::nullopt;

  content::WebContents* contents =
      model_->GetWebContentsAt(model_index);
  if (!contents)
    return std::nullopt;

  return GetEnvironmentForContents(contents);
}

bool EnvironmentIndexMapper::IsModelIndexVisible(
    const std::string& environment_id,
    int model_index) const {
  const std::optional<std::string> owner = EnvironmentOfModelIndex(model_index);
  return owner.has_value() && *owner == environment_id;
}

int EnvironmentIndexMapper::GetVisibleCount(
    const std::string& environment_id) const {
  int count = 0;
  for (int i = 0; i < model_->count(); ++i) {
    if (IsModelIndexVisible(environment_id, i))
      ++count;
  }
  return count;
}

std::vector<int> EnvironmentIndexMapper::GetVisibleModelIndices(
    const std::string& environment_id) const {
  std::vector<int> result;
  for (int i = 0; i < model_->count(); ++i) {
    if (IsModelIndexVisible(environment_id, i))
      result.push_back(i);
  }
  return result;
}

std::optional<int> EnvironmentIndexMapper::VisibleToModel(
    const std::string& environment_id,
    int visible_index) const {
  if (visible_index < 0)
    return std::nullopt;

  const std::vector<int> indices = GetVisibleModelIndices(environment_id);
  if (visible_index >= static_cast<int>(indices.size()))
    return std::nullopt;
  return indices[visible_index];
}

std::optional<int> EnvironmentIndexMapper::ModelToVisible(
    const std::string& environment_id,
    int model_index) const {
  if (!IsModelIndexVisible(environment_id, model_index))
    return std::nullopt;

  int visible = 0;
  for (int i = 0; i < model_index; ++i) {
    if (IsModelIndexVisible(environment_id, i))
      ++visible;
  }
  return visible;
}

std::optional<int> EnvironmentIndexMapper::NextVisibleModelIndex(
    const std::string& environment_id,
    int from_model_index) const {
  const std::vector<int> indices = GetVisibleModelIndices(environment_id);
  if (indices.empty())
    return std::nullopt;

  // First visible tab strictly after `from_model_index`, wrapping.
  for (int index : indices) {
    if (index > from_model_index)
      return index;
  }
  return indices.front();
}

std::optional<int> EnvironmentIndexMapper::PreviousVisibleModelIndex(
    const std::string& environment_id,
    int from_model_index) const {
  const std::vector<int> indices = GetVisibleModelIndices(environment_id);
  if (indices.empty())
    return std::nullopt;

  // Last visible tab strictly before `from_model_index`, wrapping.
  for (auto it = indices.rbegin(); it != indices.rend(); ++it) {
    if (*it < from_model_index)
      return *it;
  }
  return indices.back();
}

std::optional<int> EnvironmentIndexMapper::RestoreIndexFor(
    const std::string& environment_id) const {
  // Prefer the remembered tab, but validate it: model indices shift when other
  // environments' tabs are inserted or removed, so a stale entry must not be
  // used blindly.
  for (const auto& [id, model_index] : last_active_) {
    if (id != environment_id)
      continue;
    if (IsModelIndexVisible(environment_id, model_index))
      return model_index;
    break;
  }

  const std::vector<int> indices = GetVisibleModelIndices(environment_id);
  if (indices.empty())
    return std::nullopt;
  return indices.front();
}

void EnvironmentIndexMapper::RememberActiveTab(const std::string& environment_id,
                                               int model_index) {
  if (!IsModelIndexVisible(environment_id, model_index)) {
    LOG(ERROR) << "parra: refusing to remember model index " << model_index
               << " as active for environment '" << environment_id
               << "': the tab belongs to a different environment";
    return;
  }

  for (auto& [id, index] : last_active_) {
    if (id == environment_id) {
      index = model_index;
      return;
    }
  }
  last_active_.emplace_back(environment_id, model_index);
}

}  // namespace parra
