// Copyright 2026 The Parra Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chrome/browser/parra/environment_tab_router.h"

#include <string_view>

#include "base/logging.h"
#include "chrome/browser/parra/environment_registry.h"
#include "chrome/browser/parra/environment_tag.h"
#include "chrome/browser/parra/parra_storage_partition_util.h"
#include "chrome/browser/profiles/profile.h"
#include "content/public/browser/security_principal.h"
#include "content/public/browser/site_instance.h"
#include "content/public/browser/storage_partition_config.h"
#include "content/public/browser/web_contents.h"

namespace parra {

namespace {

// Result of asking "which environment does this live page actually run in?".
//
// The three-way split exists because "the SecurityPrincipal reports the default
// partition" does NOT mean "this page belongs to Personal". A page whose
// SiteInstance has no site assigned yet (about:blank, about:srcdoc, an empty
// document written via document.write) legitimately reports the default partition
// while having made NO network request at all. Treating that as "Personal" is how
// a blank popup born in a non-default environment would silently be mislabelled;
// treating it as an error would reject a page that is perfectly safe.
enum class PagePartitionState {
  // No site assigned: the page has not committed a real document, so no request
  // has been made and no storage has been touched. Its eventual partition is
  // decided by the first real navigation, which honours
  // is_fixed_storage_partition when present.
  kSiteNotAssigned,
  // A site is assigned and its partition is the default one. This is Personal (or
  // an untouched stock tab).
  kDefaultPartition,
  // A site is assigned and its partition is a Parra partition.
  kNamedPartition,
};

struct PagePartition {
  PagePartitionState state = PagePartitionState::kSiteNotAssigned;
  // Environment id for kNamedPartition; "personal" for kDefaultPartition; empty
  // for kSiteNotAssigned.
  std::string environment_id;
};

PagePartition InspectPagePartition(const content::WebContents* contents) {
  PagePartition result;
  if (!contents)
    return result;

  content::WebContents* mutable_contents =
      const_cast<content::WebContents*>(contents);
  content::SiteInstance* site_instance = mutable_contents->GetSiteInstance();
  if (!site_instance)
    return result;  // no SiteInstance at all: nothing is assigned yet

  // "Has a real document been committed?" is the discriminator, and it is read from
  // the WebContents rather than from the SecurityPrincipal.
  //
  // Why not the SiteInstance's site URL: the public SiteInstance API in this
  // revision does not expose it, and the SecurityPrincipal deliberately reports the
  // DEFAULT partition for a site-unassigned instance -- which is exactly the value
  // that must not be mistaken for "Personal". A committed document is the only
  // signal that also guarantees a request has actually been made.
  //
  // `GetLastCommittedURL()` is empty until the first commit, and a committed
  // document always has a non-empty URL (about:blank included, once it commits).
  if (!mutable_contents->GetPrimaryMainFrame() ||
      mutable_contents->GetLastCommittedURL().is_empty()) {
    return result;  // kSiteNotAssigned
  }

  const content::StoragePartitionConfig config =
      site_instance->GetSecurityPrincipal().GetStoragePartitionConfig();
  // STRUCTURAL lookup, not definition-aware: a page living in `parra/gone` (an
  // environment that has since been deleted) must be reported as living in
  // 'gone', not as Personal. Using the definition-aware variant here would collapse
  // "the environment was deleted" into "the page is in the default partition",
  // and every caller would then treat a foreign page as a stock one.
  const std::string named = GetEnvironmentIdFromPartitionConfig(config);
  if (named.empty()) {
    result.state = PagePartitionState::kDefaultPartition;
    result.environment_id = kDefaultEnvironmentId;
  } else {
    result.state = PagePartitionState::kNamedPartition;
    result.environment_id = named;
  }
  return result;
}

// The environment named by the partition a page actually lives in.
//
// Empty means either "the default partition" or "no site assigned yet"; callers
// that need to tell those apart must use InspectPagePartition().
std::string EnvironmentIdFromContents(const content::WebContents* contents) {
  const PagePartition partition = InspectPagePartition(contents);
  if (partition.state != PagePartitionState::kNamedPartition)
    return std::string();
  return partition.environment_id;
}

}  // namespace

// static
ResolvedTabEnvironment EnvironmentTabRouter::ResolveForNewTab(
    Profile* profile,
    content::WebContents* source_contents) {
  return ResolveForNewTab(profile, source_contents, std::string());
}

// static
ResolvedTabEnvironment EnvironmentTabRouter::ResolveForNewTab(
    Profile* profile,
    content::WebContents* source_contents,
    const std::string& explicit_environment_id) {
  ResolvedTabEnvironment resolved;
  if (!profile)
    return resolved;

  resolved.requested_environment_id = explicit_environment_id;

  // ---------------------------------------------------------------------
  // Step 1: which environment did anybody actually NAME?
  //
  // Three sources, in priority order. Each is a CLAIM about the page that is
  // about to be created; Step 2 decides whether the claim can be honoured.
  // ---------------------------------------------------------------------
  std::string desired;
  EnvironmentOrigin origin = EnvironmentOrigin::kParraDisabled;

  if (!explicit_environment_id.empty()) {
    // Only chrome's own new-tab command writes this, and only from the window's
    // active environment -- never from renderer-controlled data. See the header.
    desired = explicit_environment_id;
    origin = EnvironmentOrigin::kExplicitRequest;
  } else if (source_contents && EnvironmentTag::HasTag(source_contents)) {
    // The authoritative answer: the source page's own tag.
    desired = EnvironmentTag::Get(source_contents);
    origin = EnvironmentOrigin::kInheritedFromTag;
  } else if (source_contents) {
    // Untagged. The page's SiteInstance may still be pinned to a Parra partition
    // (tabs created before tagging existed, or content bound by partition only).
    // Inheriting it is safe and strictly better than dropping the page into
    // Personal.
    desired = EnvironmentIdFromContents(source_contents);
    if (!desired.empty())
      origin = EnvironmentOrigin::kInheritedFromPartition;
  }

  // ---------------------------------------------------------------------
  // Step 2a: nobody named an environment -> a stock Chromium tab.
  //
  // This is the only branch allowed to reach the default partition without a
  // definition check, and it stays exactly as upstream: a navigation that never
  // mentioned an environment must not be affected by Parra being configured.
  // ---------------------------------------------------------------------
  if (desired.empty()) {
    if (!IsEnabled())
      return resolved;  // Parra inactive: plain upstream path, unchanged.

    resolved.active = true;
    resolved.environment_id = kDefaultEnvironmentId;
    resolved.origin = source_contents ? EnvironmentOrigin::kUntaggedSourceDefault
                                      : EnvironmentOrigin::kNoSourceDefault;
    resolved.partition =
        GetStoragePartitionConfigForEnvironment(profile, kDefaultEnvironmentId);
    return resolved;
  }

  // ---------------------------------------------------------------------
  // Step 2b: somebody NAMED an environment -> it must be honourable or the whole
  // creation is refused.
  //
  // Note this runs even when IsEnabled() is false. That is deliberate and is the
  // core of the fail-closed contract: a page whose opener lives in parra/test-a
  // exists (its partition is fixed), so its popups must join test-a. Falling
  // through to the upstream path because "Parra looks disabled" would create the
  // popup in Personal, where it reads Personal's cookies -- a silent isolation
  // break caused by a configuration problem rather than by a routing bug.
  // ---------------------------------------------------------------------
  resolved.active = true;

  const PartitionResolution lookup =
      ResolvePartitionForEnvironment(profile, desired);
  if (!lookup.ok()) {
    resolved.failure = lookup.failure;
    LogEnvironmentGuardRefusal("new_tab", desired, lookup.failure);
    return resolved;
  }

  resolved.environment_id = lookup.environment_id;
  resolved.partition = lookup.config;
  resolved.origin = origin;

  // The resolved environment must be the one that was asked for. A definition
  // that maps back to the default partition under a different name would put the
  // page in Personal while the caller believes it is isolated.
  const std::string effective =
      lookup.lookup == EnvironmentLookup::kPersonal
          ? std::string(kDefaultEnvironmentId)
          : lookup.environment_id;
  if (effective != desired) {
    resolved.failure = "requested environment '" + desired +
                       "' resolves to '" + effective +
                       "', which is a different environment";
    LogEnvironmentGuardRefusal("new_tab", desired, resolved.failure);
  }
  return resolved;
}

// static
bool EnvironmentTabRouter::TagNewContents(
    content::WebContents* contents,
    const ResolvedTabEnvironment& resolved) {
  if (!contents || !resolved.active)
    return true;

  // Read the partition this page ACTUALLY got. This is the ground truth; the
  // resolved id is only a claim about it.
  const PagePartition partition = InspectPagePartition(contents);

  if (partition.state == PagePartitionState::kSiteNotAssigned) {
    // about:blank / an empty document written via document.write / about:srcdoc.
    //
    // SiteInstance::CreateForFixedStoragePartition() does NOT assign a site for an
    // empty-document scheme, so the SecurityPrincipal still reports the default
    // partition -- but the page has made NO request and touched NO storage. Its
    // eventual partition is decided by its first real navigation, which honours
    // is_fixed_storage_partition on the BrowsingInstance.
    //
    // This is therefore NOT a routing mismatch and NOT "Personal". Tagging it with
    // the resolved environment is correct: it records where the page belongs, and
    // the tag will be re-validated the moment a real document commits.
    //
    // The distinction matters: without it, a blank popup born in test-a would
    // either be mislabelled Personal (silent downgrade) or refused (breaking
    // window.open('about:blank') entirely).
    EnvironmentTag::Set(contents, resolved.environment_id);
    LOG(INFO) << "parra-tabs: tagged site-unassigned contents token="
              << contents->GetUniqueToken().ToString() << " env='"
              << resolved.environment_id
              << "' (no document yet; the tag is a claim until the first real "
                 "navigation commits)";
    return true;
  }

  const std::string actual = partition.environment_id;

  // THE INVARIANT: the environment we decided on must be the one the page really
  // landed in.
  //
  // This is checked, not repaired. An earlier version derived the tag FROM the
  // partition, which made the tag always self-consistent and therefore hid exactly
  // the bug that matters: a requested "test-a" that silently became a Personal
  // page would look perfectly tagged. Comparing first and refusing on mismatch is
  // what turns that silent downgrade into a visible failure.
  if (actual != resolved.environment_id) {
    LOG(ERROR) << "parra: ROUTING MISMATCH token="
               << contents->GetUniqueToken().ToString() << ": resolved env='"
               << resolved.environment_id << "' (requested='"
               << (resolved.requested_environment_id.empty()
                       ? "<none>"
                       : resolved.requested_environment_id)
               << "', origin=" << static_cast<int>(resolved.origin)
               << ") but the WebContents actually landed in env='" << actual
               << "'. REFUSING to tag or navigate it.";
    // Deliberately do NOT set a tag: writing one now would record a decision the
    // page does not honour, and any later reader would trust it.
    return false;
  }

  // A stock default tab with no source stays untagged: Get() reads Personal for it,
  // which is identical, and Phase 1's behaviour for upstream tabs must not change.
  if (resolved.origin == EnvironmentOrigin::kNoSourceDefault) {
    LOG(INFO) << "parra-tabs: left new contents untagged token="
              << contents->GetUniqueToken().ToString()
              << " (stock default tab; reads as '" << kDefaultEnvironmentId
              << "')";
    return true;
  }

  EnvironmentTag::Set(contents, resolved.environment_id);
  LOG(INFO) << "parra-tabs: tagged new contents token="
            << contents->GetUniqueToken().ToString() << " env='"
            << resolved.environment_id << "' origin="
            << static_cast<int>(resolved.origin) << " actual_partition_env='"
            << actual << "'";
  return true;
}

// static
void EnvironmentTabRouter::CopyEnvironmentFrom(
    const content::WebContents* source,
    content::WebContents* destination) {
  if (!source || !destination || !IsEnabled())
    return;

  // Prefer the partition the clone actually landed in, for the same reason
  // TagNewContents() does: the partition is what decides which cookies the page
  // will read, so it must win over any inherited id.
  //
  // Owned `std::string`: EnvironmentIdFromContents() returns by value, so binding
  // its result to a string_view would dangle before the comparison below.
  const std::string partition_environment = EnvironmentIdFromContents(destination);
  if (!partition_environment.empty()) {
    EnvironmentTag::Set(destination, partition_environment);
    return;
  }

  // Clone() reuses the source SiteInstance, so the default-partition case here
  // means the source itself was in the default partition. Preserve its tag
  // verbatim rather than re-deriving an environment: a duplicated Personal tab
  // must stay Personal, and a duplicated stock tab must stay untagged.
  if (!EnvironmentTag::HasTag(source))
    return;

  EnvironmentTag::Set(destination, EnvironmentTag::Get(source));
}

// static
void EnvironmentTabRouter::VerifyContentsMatchesPartition(
    Profile* profile,
    const content::WebContents* contents,
    const char* when) {
  if (!contents || !profile)
    return;

  // Reuse the same partition reader as every other function in this file, so the
  // invariant is checked against the exact value the tag was derived from.
  const PagePartition partition = InspectPagePartition(contents);

  // Gate on the PAGE, not on the process-level switch.
  //
  // Gating on IsEnabled() would disable the check exactly when the configuration
  // is damaged -- the moment a tag/partition disagreement is most likely. A page
  // that carries a tag or lives in a Parra partition is part of the isolation
  // contract whether or not the configuration still loads, so it is always
  // checked.
  if (!EnvironmentTag::HasTag(contents) &&
      partition.state != PagePartitionState::kNamedPartition) {
    return;
  }

  const std::string tag_environment = EnvironmentTag::Get(contents);

  if (partition.state == PagePartitionState::kSiteNotAssigned) {
    // No document yet, so there is no partition to disagree with. Comparing here
    // would flag every about:blank popup as a violation for doing nothing wrong.
    // The check resumes on the first real commit (see
    // VerifyAfterFirstCommitIfNeeded()).
    LOG(INFO) << "parra: invariant check (" << when
              << ") deferred for token " << contents->GetUniqueToken().ToString()
              << ": no site assigned yet, tag='" << tag_environment
              << "'. Nothing has been requested, so nothing can be wrong yet.";
    return;
  }

  const std::string expected = partition.environment_id;

  if (tag_environment == expected)
    return;

  LOG(ERROR) << "parra: INVARIANT VIOLATED (" << when << "): contents "
             << contents->GetUniqueToken().ToString() << " is tagged '"
             << tag_environment << "' but its StoragePartition names environment '"
             << expected
             << "'. The tag and the partition disagree, so this page will read "
                "and write the wrong environment's storage.";
}

// static
bool EnvironmentTabRouter::TagMatchesCommittedPartition(
    const content::WebContents* contents,
    std::string* tag_environment,
    std::string* actual_environment) {
  if (!contents)
    return true;

  const PagePartition partition = InspectPagePartition(contents);
  if (partition.state == PagePartitionState::kSiteNotAssigned)
    return true;  // nothing committed; nothing to compare

  // Same reasoning as VerifyContentsMatchesPartition(): a page that carries no
  // tag and lives in the default partition is a stock tab and is vacuously fine.
  if (!EnvironmentTag::HasTag(contents) &&
      partition.state != PagePartitionState::kNamedPartition) {
    return true;
  }

  if (tag_environment)
    *tag_environment = EnvironmentTag::Get(contents);
  if (actual_environment)
    *actual_environment = partition.environment_id;

  return EnvironmentTag::Get(contents) == partition.environment_id;
}

}  // namespace parra
