// Copyright 2026 The Parra Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chrome/browser/parra/environment_tag.h"

#include <memory>
#include <utility>

#include "base/check.h"
#include "content/public/browser/web_contents.h"

namespace parra {

namespace {

// The user-data payload holding the environment id.
//
// base::SupportsUserData::Data rather than the tab's UnownedUserDataHost: the
// tag must survive independent of any tab-strip object, so that it is readable
// during session restore before the tab has been inserted anywhere.
class EnvironmentUserData : public base::SupportsUserData::Data {
 public:
  // Changing this string breaks nothing at runtime, but it is part of the
  // session-restore contract's sibling key, so keep the two distinct and stable:
  //   "parra.environment"      -> live browser state (here)
  //   "parra.environment_id"   -> on-disk SessionTab::extra_data
  // See chrome/browser/ui/browser_tabrestore.cc kParraEnvironmentExtraDataKey.
  static constexpr char kKey[] = "parra.environment";

  explicit EnvironmentUserData(std::string environment_id)
      : environment_id_(std::move(environment_id)) {}

  const std::string& environment_id() const { return environment_id_; }

 private:
  std::string environment_id_;
};

constexpr char EnvironmentUserData::kKey[];

}  // namespace

// static
void EnvironmentTag::Set(content::WebContents* contents,
                         const std::string& environment_id) {
  CHECK(contents);
  CHECK(!environment_id.empty());
  contents->SetUserData(EnvironmentUserData::kKey,
                        std::make_unique<EnvironmentUserData>(environment_id));
}

// static
std::string EnvironmentTag::Get(const content::WebContents* contents) {
  if (!contents)
    return kDefaultEnvironmentId;

  auto* data = static_cast<EnvironmentUserData*>(
      contents->GetUserData(EnvironmentUserData::kKey));
  if (!data)
    return kDefaultEnvironmentId;
  return data->environment_id();
}

// static
bool EnvironmentTag::HasTag(const content::WebContents* contents) {
  if (!contents)
    return false;
  return contents->GetUserData(EnvironmentUserData::kKey) != nullptr;
}

}  // namespace parra
