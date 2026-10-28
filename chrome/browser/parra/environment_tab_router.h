// Copyright 2026 The Parra Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CHROME_BROWSER_PARRA_ENVIRONMENT_TAB_ROUTER_H_
#define CHROME_BROWSER_PARRA_ENVIRONMENT_TAB_ROUTER_H_

#include <string>

#include "content/public/browser/storage_partition_config.h"

class Profile;

namespace content {
class WebContents;
}  // namespace content

namespace parra {

// Why the router picked the environment it picked. Recorded so log lines and
// tests can distinguish a legitimate inheritance from a fallback, instead of
// inferring intent from a string comparison.
enum class EnvironmentOrigin {
  // Parra is not enabled; nothing in this file applies.
  kParraDisabled,
  // The source page carried an explicit EnvironmentTag.
  kInheritedFromTag,
  // The source page had no tag but its SiteInstance was pinned to a Parra
  // StoragePartition, so the partition named the environment.
  kInheritedFromPartition,
  // No source page at all (a brand new window, or a caller that did not name
  // one). Falls back to the default environment, which is what a stock
  // Chromium tab is.
  kNoSourceDefault,
  // The caller stated the environment because it has no source page that could
  // express it: Ctrl+T / `+` while the window's current environment is empty.
  kExplicitRequest,
  // A source page exists, is untagged, and lives in the default partition. That
  // is a legitimate stock Chromium tab, so Personal is the correct answer -- but
  // it is recorded separately from kInheritedFromTag so a future Parra creation
  // path that forgets to tag can be told apart from an upstream tab.
  kUntaggedSourceDefault,
};

// The environment a new tab must join, plus the StoragePartition that
// environment maps to.
//
// THREE VALUES, DELIBERATELY KEPT APART
//
// Conflating them is how a routing bug becomes invisible, so each one has its own
// field and its own meaning:
//
//   requested_environment_id  What the caller asked for (Ctrl+T while an
//                             environment is empty states this explicitly).
//                             Empty when the caller asked for nothing.
//   environment_id            What the router decided, by inheritance or by
//                             honoring the request. This is a CLAIM, not a fact.
//   partition                 The StoragePartition that claim maps to.
//
// The fact -- which partition a page actually landed in -- is not known until the
// WebContents exists, and is checked separately by TagNewContents() and
// VerifyContentsMatchesPartition(). A mismatch between `environment_id` and the
// page's real partition is a routing failure, NOT something to be repaired by
// rewriting the tag.
struct ResolvedTabEnvironment {
  // False when Parra is disabled, no profile was supplied, or the request was
  // refused. Every caller must fall through to the plain upstream path when this
  // is false AND `failure` is empty; when `failure` is non-empty the caller must
  // NOT create the tab at all.
  bool active = false;

  // What the caller explicitly requested, or empty. Kept so a refusal can name
  // both sides, and so the invariant "requested == actual" can be asserted.
  std::string requested_environment_id;

  // The environment the router decided on. Never empty when `active`.
  std::string environment_id;

  // The partition `environment_id` maps to. Default for the `personal`
  // environment; a dedicated fixed partition otherwise.
  content::StoragePartitionConfig partition;

  EnvironmentOrigin origin = EnvironmentOrigin::kParraDisabled;

  // Non-empty means the request MUST be refused: the tab must not be created and
  // must not be navigated, because doing so would put a page in an environment
  // other than the one that was asked for. Empty means "proceed".
  std::string failure;

  bool ok() const { return active && failure.empty(); }
};

// Decides which environment a tab being created belongs to, and which
// StoragePartition it must therefore be bound to BEFORE its first navigation.
//
// This is the single decision point for every tab-creation path: Ctrl+T, the
// strip's `+` button, omnibox-to-new-tab, window.open(), target=_blank,
// "open link in new tab", bookmarks, history, and duplicate-tab. The rules:
//
//   * A tab derived from a page inherits THAT PAGE's environment, never the
//     window's currently visible one. A Personal page that is still running
//     script in the background while the window shows Test-A must open its
//     popups in Personal, otherwise a script running in one environment would
//     read and write another environment's cookies.
//   * A tab with no source at all (Ctrl+T, `+`) uses the source the navigator
//     already resolved -- the active tab -- and therefore inherits the active
//     environment.
//   * Nothing here ever looks at a URL, a cookie, a title or a tab position.
class EnvironmentTabRouter {
 public:
  EnvironmentTabRouter() = delete;

  // Resolves the environment for a tab that is about to be created from
  // `source_contents` (may be null).
  static ResolvedTabEnvironment ResolveForNewTab(
      Profile* profile,
      content::WebContents* source_contents);

  // Same, but for the one case where the caller already knows the environment and
  // no source page can express it: Ctrl+T / the strip's `+` while the window's
  // current environment has no tabs left.
  //
  // `explicit_environment_id` WINS over any source, and that is safe because of
  // who may write it: only chrome's own tab-creation command sets it, and only
  // after reading the window's active environment. It is never reachable from
  // renderer-controlled data, so a background page cannot use it to place its own
  // popups in another environment -- page-derived creations leave it empty and
  // inherit from the source page as usual.
  //
  // An id that does not name a configured environment is reported and ignored
  // rather than turned into a private partition nothing else shares.
  static ResolvedTabEnvironment ResolveForNewTab(
      Profile* profile,
      content::WebContents* source_contents,
      const std::string& explicit_environment_id);

  // Writes the environment tag onto a freshly created WebContents, and VERIFIES
  // that the page actually landed in the partition the router chose.
  //
  // Must be called before the first navigation and before the tab is inserted
  // into the strip, so no observer can ever see the tab in the wrong environment
  // on its way in.
  //
  // Returns false when the WebContents ended up in a different StoragePartition
  // than `resolved.environment_id` names. The caller MUST then not navigate and not
  // insert the tab: its cookies would belong to the wrong environment. The tag is
  // deliberately left unwritten in that case, so the inconsistency cannot later be
  // mistaken for a valid decision.
  //
  // This replaces an earlier design that derived the tag FROM the actual partition.
  // That made the tag always self-consistent, which hid exactly the failure this
  // check exists to catch: a requested environment silently downgraded to another.
  static bool TagNewContents(content::WebContents* contents,
                             const ResolvedTabEnvironment& resolved);

  // Checks the invariant "tag == partition" for `contents` and logs an ERROR
  // when it is violated. Never mutates anything: a violated invariant is
  // reported, not papered over by rewriting either side.
  //
  // DEFERS when the page has no site assigned yet (about:blank, an empty document
  // written by document.write). Such a page has made no request and touched no
  // storage, so there is nothing to disagree with; comparing at that moment would
  // flag every blank popup as a violation for doing nothing wrong.
  static void VerifyContentsMatchesPartition(Profile* profile,
                                             const content::WebContents* contents,
                                             const char* when);

  // True when a page that HAS a committed document still agrees with its tag.
  //
  // This is the check that must run after the first real navigation, because that
  // is the moment a previously site-unassigned page acquires a partition. Returns
  // true (vacuously) when the page has not committed yet.
  //
  // `tag_environment` / `actual_environment` are filled for diagnostics when
  // non-null.
  static bool TagMatchesCommittedPartition(const content::WebContents* contents,
                                           std::string* tag_environment,
                                           std::string* actual_environment);

  // Copies the environment tag from `source` onto `destination`.
  //
  // Used by duplicate-tab, where WebContents::Clone() intentionally reuses the
  // source's SiteInstance -- and therefore its StoragePartition -- but cannot
  // copy UserData, which is where the tag lives. Without this the copy would sit
  // in the right partition while reading as Personal, which is exactly the
  // tag/partition disagreement that must never exist.
  static void CopyEnvironmentFrom(const content::WebContents* source,
                                  content::WebContents* destination);
};

}  // namespace parra

#endif  // CHROME_BROWSER_PARRA_ENVIRONMENT_TAB_ROUTER_H_
