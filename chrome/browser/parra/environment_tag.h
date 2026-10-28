// Copyright 2026 The Parra Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CHROME_BROWSER_PARRA_ENVIRONMENT_TAG_H_
#define CHROME_BROWSER_PARRA_ENVIRONMENT_TAG_H_

#include <string>

namespace content {
class WebContents;
}

namespace parra {

// The environment a tab belongs to when it carries no explicit tag. Matches what
// a stock Chromium tab does.
inline constexpr char kDefaultEnvironmentId[] = "personal";

// Environment ownership of a WebContents.
//
// This lives in the low layer (//chrome/browser/parra:parra) rather than next to
// EnvironmentIndexMapper so that code which can only see content/ -- most
// importantly session restore in chrome/browser/ui/browser_tabrestore.cc -- can
// tag a tab without depending on the UI tab-strip layer. EnvironmentIndexMapper
// is a pure delegator over this.
//
// Why the tag lives on the WebContents and not on the tab's position: the
// environment is a property of the page that owns the StoragePartition. Storing
// it on the page means reordering, inserting or closing other environments' tabs
// can never silently reassign a tab to a different environment.
class EnvironmentTag {
 public:
  EnvironmentTag() = delete;

  // The only writer. `environment_id` must not be empty.
  static void Set(content::WebContents* contents,
                  const std::string& environment_id);

  // Reads the tag without needing a TabStripModel. Returns
  // kDefaultEnvironmentId for untagged or null contents, which is the correct
  // answer for a restored upstream tab.
  static std::string Get(const content::WebContents* contents);

  // True when somebody explicitly wrote a tag. This exists because Get()
  // deliberately conflates two very different situations:
  //
  //   * an upstream Chromium tab that was never meant to carry an environment
  //     (a native default tab -- "Personal" is the right answer), and
  //   * a tab created through a Parra path that forgot to bind its environment.
  //
  // The first is legitimate, the second is a bug that would silently drop a page
  // into Personal while its StoragePartition says otherwise. Callers that need to
  // tell them apart (see EnvironmentTabRouter) must ask this, not compare Get()
  // against kDefaultEnvironmentId.
  static bool HasTag(const content::WebContents* contents);
};

}  // namespace parra

#endif  // CHROME_BROWSER_PARRA_ENVIRONMENT_TAG_H_
