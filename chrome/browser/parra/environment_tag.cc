// Copyright 2026 The Parra Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chrome/browser/parra/environment_tag.h"

#include <memory>
#include <utility>

#include "base/check.h"
#include "components/tabs/public/tab_interface.h"
#include "content/public/browser/web_contents.h"

namespace parra {

// The on-disk sessions::SessionTab::extra_data key. Declared in the header so
// //chrome/browser/sessions can reuse it on the rebuild path without depending
// on the UI layer (see the header comment).
const char kEnvironmentSessionExtraDataKey[] = "parra.environment_id";

// Marks the tab as Parra-owned even when the environment id itself is
// unreadable. See the header for the four cases this disambiguates.
const char kEnvironmentSessionOwnerKey[] = "parra.owner";

namespace {

// The user-data payload holding the environment id.
//
// base::SupportsUserData::Data rather than the tab's UnownedUserDataHost: the
// tag must survive independent of any tab-strip object, so that it is readable
// during session restore before the tab has been inserted anywhere.
class EnvironmentUserData : public base::SupportsUserData::Data {
 public:
  // Changing this string breaks nothing at runtime, but it is part of the
  // session-restore contract's sibling key, so keep the two distinct and
  // stable:
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

class BrowserLevelUserData : public base::SupportsUserData::Data {};
constexpr char kBrowserLevelKey[] = "parra.browser_level_tab";

// The installed observer, if any. Function-local static rather than a global so
// the initialisation order cannot bite during startup.
EnvironmentTag::Observer& TagObserver() {
  static base::NoDestructor<EnvironmentTag::Observer> observer;
  return *observer;
}

}  // namespace

// static
void BrowserLevelTab::Mark(content::WebContents* contents) {
  CHECK(contents);
  CHECK(!EnvironmentTag::HasTag(contents));
  CHECK(!tabs::TabInterface::MaybeGetFromContents(contents));
  CHECK(!Is(contents));
  contents->SetUserData(kBrowserLevelKey,
                        std::make_unique<BrowserLevelUserData>());
}

// static
bool BrowserLevelTab::Is(const content::WebContents* contents) {
  return contents && contents->GetUserData(kBrowserLevelKey);
}

// static
void EnvironmentTag::SetObserver(Observer observer) {
  TagObserver() = std::move(observer);
}

// static
void EnvironmentTag::Set(content::WebContents* contents,
                         const std::string& environment_id) {
  CHECK(contents);
  CHECK(!environment_id.empty());
  CHECK(!BrowserLevelTab::Is(contents));
  contents->SetUserData(EnvironmentUserData::kKey,
                        std::make_unique<EnvironmentUserData>(environment_id));

  // Announce immediately: this is the earliest moment the tab's ownership is
  // known, and doing it here means the record does not depend on winning a race
  // against a session-command rebuild.
  if (TagObserver() && !TagObserver().is_null()) {
    TagObserver().Run(contents, environment_id);
  }
}

// static
std::string EnvironmentTag::Get(const content::WebContents* contents) {
  if (!contents) {
    return kDefaultEnvironmentId;
  }
  if (BrowserLevelTab::Is(contents)) {
    return std::string();
  }

  auto* data = static_cast<EnvironmentUserData*>(
      contents->GetUserData(EnvironmentUserData::kKey));
  if (!data) {
    return kDefaultEnvironmentId;
  }
  return data->environment_id();
}

// static
bool EnvironmentTag::HasTag(const content::WebContents* contents) {
  if (!contents) {
    return false;
  }
  return contents->GetUserData(EnvironmentUserData::kKey) != nullptr;
}

}  // namespace parra
