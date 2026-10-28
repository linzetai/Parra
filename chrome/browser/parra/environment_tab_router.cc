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
  const std::string_view named = GetEnvironmentIdForStoragePartition(config);
  if (named.empty()) {
    result.state = PagePartitionState::kDefaultPartition;
    result.environment_id = kDefaultEnvironmentId;
  } else {
    result.state = PagePartitionState::kNamedPartition;
    result.environment_id = std::string(named);
  }
  return result;
}

// The environment named by the partition a page actually lives in.
//
// Empty means either "the default partition" or "no site assigned yet"; callers
// that need to tell those apart must use InspectPagePartition().
std::string_view EnvironmentIdFromContents(const content::WebContents* contents) {
  const PagePartition partition = InspectPagePartition(contents);
  if (partition.state == PagePartitionState::kSiteNotAssigned)
    return std::string_view();
  return GetEnvironmentIdForStoragePartition(
      const_cast<content::WebContents*>(contents)
          ->GetSiteInstance()
          ->GetSecurityPrincipal()
          .GetStoragePartitionConfig());
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
  if (!profile || !IsParraPocEnabled())
    return resolved;

  resolved.active = true;
  resolved.requested_environment_id = explicit_environment_id;

  // The explicit id wins when present; see the header for why that is safe (only
  // chrome's own tab-creation command writes it, never renderer-controlled data).
  if (!explicit_environment_id.empty()) {
    if (EnvironmentRegistry::GetInstance()->Find(explicit_environment_id)) {
      resolved.environment_id = explicit_environment_id;
      resolved.origin = EnvironmentOrigin::kExplicitRequest;
      resolved.partition = GetStoragePartitionConfigForEnvironment(
          profile, resolved.environment_id);

      // The request must actually be honorable. If the environment maps back to the
      // DEFAULT partition under a non-default name, honoring it would mean creating
      // the page in Personal while the caller believes otherwise -- the exact silent
      // downgrade this phase must not allow.
      const std::string_view named_by_partition =
          GetEnvironmentIdForStoragePartition(resolved.partition);
      const std::string effective =
          named_by_partition.empty() ? std::string(kDefaultEnvironmentId)
                                     : std::string(named_by_partition);
      if (effective != resolved.environment_id) {
        resolved.failure = "requested environment '" + resolved.environment_id +
                           "' resolves to partition '" + effective +
                           "', which is a different environment";
        LOG(ERROR) << "parra: refusing new tab: " << resolved.failure;
      }
      return resolved;
    }

    // An id that does not name a configured environment must NOT be turned into a
    // private partition no other tab shares, and must NOT be silently downgraded to
    // Personal either -- the caller asked for something we cannot provide.
    resolved.failure =
        "requested environment '" + explicit_environment_id +
        "' is not configured";
    LOG(ERROR) << "parra: refusing new tab: " << resolved.failure;
    return resolved;
  }

  if (!source_contents) {
    // A window being created from scratch, or a caller that legitimately has no
    // source (startup, restore). Personal is what a stock tab would be.
    resolved.environment_id = kDefaultEnvironmentId;
    resolved.origin = EnvironmentOrigin::kNoSourceDefault;
    resolved.partition =
        GetStoragePartitionConfigForEnvironment(profile, resolved.environment_id);
    return resolved;
  }

  if (EnvironmentTag::HasTag(source_contents)) {
    // The authoritative answer: the source page's own tag.
    resolved.environment_id = EnvironmentTag::Get(source_contents);
    resolved.origin = EnvironmentOrigin::kInheritedFromTag;
  } else {
    // Untagged. Distinguish the two meanings that Get() cannot express:
    //
    //  - the page's SiteInstance is pinned to a Parra partition => the
    //    partition itself names the environment (this happens for tabs created
    //    before tagging was introduced, and for any content whose environment
    //    was bound by partition only). Inheriting it is safe and strictly
    //    better than dropping the page into Personal.
    //  - the page is in the default partition and untagged => it is a stock
    //    Chromium tab. Personal is correct.
    const std::string_view from_partition = EnvironmentIdFromContents(source_contents);
    if (!from_partition.empty()) {
      resolved.environment_id = std::string(from_partition);
      resolved.origin = EnvironmentOrigin::kInheritedFromPartition;
    } else {
      resolved.environment_id = kDefaultEnvironmentId;
      resolved.origin = EnvironmentOrigin::kUntaggedSourceDefault;
    }
  }

  resolved.partition =
      GetStoragePartitionConfigForEnvironment(profile, resolved.environment_id);
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
  if (!source || !destination || !IsParraPocEnabled())
    return;

  // Prefer the partition the clone actually landed in, for the same reason
  // TagNewContents() does: the partition is what decides which cookies the page
  // will read, so it must win over any inherited id.
  const std::string_view partition_environment =
      EnvironmentIdFromContents(destination);
  if (!partition_environment.empty()) {
    EnvironmentTag::Set(destination, std::string(partition_environment));
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
  if (!contents || !profile || !IsParraPocEnabled())
    return;

  // Reuse the same partition reader as every other function in this file, so the
  // invariant is checked against the exact value the tag was derived from.
  const PagePartition partition = InspectPagePartition(contents);
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
  if (!contents || !IsParraPocEnabled())
    return true;

  const PagePartition partition = InspectPagePartition(contents);
  if (partition.state == PagePartitionState::kSiteNotAssigned)
    return true;  // nothing committed; nothing to compare

  if (tag_environment)
    *tag_environment = EnvironmentTag::Get(contents);
  if (actual_environment)
    *actual_environment = partition.environment_id;

  return EnvironmentTag::Get(contents) == partition.environment_id;
}

}  // namespace parra
