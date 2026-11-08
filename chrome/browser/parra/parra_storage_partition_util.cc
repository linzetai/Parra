// Copyright 2026 The Parra Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chrome/browser/parra/parra_storage_partition_util.h"

#include <string>
#include <utility>

#include "base/command_line.h"
#include "base/logging.h"
#include "chrome/browser/parra/environment_registry.h"
#include "chrome/browser/parra/environment_store.h"
#include "chrome/browser/parra/environment_tag.h"
#include "chrome/browser/parra/parra_switches.h"
#include "chrome/browser/profiles/profile.h"

namespace parra {

namespace {

// The reserved environment that maps to the default StoragePartition. Declared
// in environment_tag.h so the low layer and the tab-strip layer cannot disagree
// about what an untagged tab is.
constexpr std::string_view kPersonalId = kDefaultEnvironmentId;

}  // namespace

bool IsEnabled() {
  // BUILD-LEVEL capability. Parra is compiled into this browser
  // unconditionally, so this is true for every process.
  //
  // This used to return EnvironmentRegistry::GetInstance()->enabled(), i.e.
  // "did the command line carry --parra-poc-environments". That conflated three
  // different questions and had a user-visible failure: a profile whose
  // environments were created through the Store lost tab filtering entirely
  // when launched without the PoC switch, because BrowserTabStripController::
  // IsModelIndexVisible() early-returned `true` for every tab. See
  // docs/parra/93 §2.1.
  //
  // Callers that need "does THIS profile / THIS window have environments" must
  // ask HasUserDefinedEnvironments() or ProfileHasEnvironment() instead.
  return true;
}

bool HasUserDefinedEnvironments(Profile* profile) {
  // P3.2: answered by the PROFILE's store, so two profiles with different
  // definitions get different answers. This is the whole point of moving the
  // definitions off the process-wide registry.
  //
  // A null or storeless profile answers from the process-level registry as a
  // fallback: that happens in unit-test shells and very early startup, and
  // reporting "no environments" there would silently disable isolation instead
  // of failing visibly.
  if (profile) {
    if (EnvironmentStore* store = EnvironmentStore::GetForProfile(profile)) {
      return store->has_user_defined_environments();
    }
  }
  // No store: fall back to the command-line registry, which is the only
  // definition source that exists before a profile is built.
  return EnvironmentRegistry::GetInstance()->enabled();
}

bool ProfileHasEnvironment(Profile* profile, std::string_view environment_id) {
  if (environment_id.empty()) {
    return false;
  }

  // The reserved environment always exists. This is what keeps an ordinary
  // Personal window behaving exactly like stock Chromium even when the profile
  // has several named environments configured.
  if (environment_id == kPersonalId) {
    return true;
  }

  if (profile) {
    if (EnvironmentStore* store = EnvironmentStore::GetForProfile(profile)) {
      const Environment* environment = store->Find(environment_id);
      return environment != nullptr && environment->enabled;
    }
  }
  return EnvironmentRegistry::GetInstance()->Find(environment_id) != nullptr;
}

PartitionResolution ResolvePartitionForEnvironment(
    Profile* profile,
    std::string_view environment_id) {
  PartitionResolution result;
  content::BrowserContext* browser_context = profile;

  if (!browser_context) {
    result.lookup = EnvironmentLookup::kUnknown;
    result.failure = "no profile";
    return result;
  }

  // (1) The caller never named an environment. This is the ONLY path allowed to
  // fall back to the default partition, and it must stay allowed: a stock
  // Chromium navigation carries no environment and must not fail merely because
  // Parra happens to be configured.
  if (environment_id.empty()) {
    result.lookup = EnvironmentLookup::kNotSpecified;
    result.config =
        content::StoragePartitionConfig::CreateDefault(browser_context);
    return result;
  }

  // (2) The reserved environment. Exists by definition, and correctness does
  // not depend on Parra being enabled: it is the default partition either way.
  if (environment_id == kPersonalId) {
    result.lookup = EnvironmentLookup::kPersonal;
    result.environment_id = std::string(kPersonalId);
    result.config =
        content::StoragePartitionConfig::CreateDefault(browser_context);
    return result;
  }

  // (3) A named environment. It must be DEFINED IN THIS PROFILE. Anything else
  // is a refusal, not a downgrade: returning the default config here is how a
  // page ends up reading Personal's cookies while its caller believes
  // otherwise.
  //
  // P3.2: the authority is the profile's EnvironmentStore. The process-wide
  // registry is consulted ONLY as a fallback for shells where no store exists
  // (unit tests, very early startup before the profile is built). It is never
  // allowed to answer for a profile that HAS a store, because that is exactly
  // the "one global truth for two profiles" failure this phase removes.
  if (EnvironmentStore* store =
          profile ? EnvironmentStore::GetForProfile(profile) : nullptr) {
    const ResolvedEnvironmentLookup resolved =
        store->ResolveForEnvironment(environment_id);
    switch (resolved.kind) {
      case StoreLookupKind::kNotSpecified:
        result.lookup = EnvironmentLookup::kNotSpecified;
        result.config = resolved.config;
        return result;
      case StoreLookupKind::kPersonal:
        result.lookup = EnvironmentLookup::kPersonal;
        result.environment_id = resolved.environment_id;
        result.config = resolved.config;
        return result;
      case StoreLookupKind::kNamed:
        result.lookup = EnvironmentLookup::kNamed;
        result.environment_id = resolved.environment_id;
        result.config = resolved.config;
        return result;
      case StoreLookupKind::kDisabled:
        // Defined but switched off. Kept distinct from kUnknown so a caller can
        // report "this environment is disabled" instead of "it does not exist".
        //
        // The CONFIG is still filled in, even though ok() is false. Two callers
        // need it:
        //
        //   * session restore restores a disabled environment's tabs rather
        //   than
        //     destroying them (its data is intact; only activation is blocked),
        //     and it needs the real partition to do that;
        //   * leaving it default while reporting kDisabled made
        //     ResolveEnvironmentForRestore() derive an empty config, which
        //     SiteInstance::CreateForFixedStoragePartition() rejects with
        //     `Check failed: !partition_config.is_default()` -- i.e. a disabled
        //     environment crashed the browser on startup instead of restoring.
        //
        // ok() stays false: being disabled still means "not selectable", and a
        // caller that only checks ok() will refuse, which is the safe default.
        result.lookup = EnvironmentLookup::kDisabled;
        result.environment_id = resolved.environment_id;
        result.config = resolved.config;
        result.failure = resolved.failure;
        return result;
      case StoreLookupKind::kUnknown:
        result.lookup = EnvironmentLookup::kUnknown;
        result.failure = resolved.failure;
        return result;
    }
  }

  EnvironmentRegistry* registry = EnvironmentRegistry::GetInstance();
  if (!registry->enabled()) {
    result.lookup = EnvironmentLookup::kUnknown;
    result.failure =
        "environment '" + std::string(environment_id) +
        "' was requested but no environments are configured in this process";
    return result;
  }

  if (!registry->Find(environment_id)) {
    result.lookup = EnvironmentLookup::kUnknown;
    result.failure = "environment '" + std::string(environment_id) +
                     "' is not defined (deleted, misspelled, or the "
                     "configuration was lost)";
    return result;
  }

  result.lookup = EnvironmentLookup::kNamed;
  result.environment_id = std::string(environment_id);
  result.config = registry->GetStoragePartitionConfig(profile, environment_id);

  // The config must actually name the environment. A definition that resolves
  // to the default partition under a non-default name would put the page in
  // Personal while the caller believes it is isolated, so it is a refusal too.
  //
  // Owned `std::string`, not `string_view`: GetEnvironmentIdForConfig() returns
  // by value, and a view into its temporary would dangle before the check
  // below.
  const std::string named_by_config =
      registry->GetEnvironmentIdForConfig(result.config);
  if (named_by_config.empty()) {
    result.lookup = EnvironmentLookup::kUnknown;
    result.failure = "environment '" + std::string(environment_id) +
                     "' resolves to the default partition; honoring it would "
                     "place the page in '" +
                     std::string(kPersonalId) + "'";
    return result;
  }

  return result;
}

content::StoragePartitionConfig GetStoragePartitionConfigForEnvironment(
    Profile* profile,
    std::string_view environment_id) {
  // Legacy fail-open wrapper. Kept for the startup PoC paths, which cannot
  // report a refusal to anyone. Production call sites that can report must use
  // ResolvePartitionForEnvironment() and check ok().
  const PartitionResolution resolved =
      ResolvePartitionForEnvironment(profile, environment_id);
  if (!resolved.ok()) {
    LOG(ERROR) << "parra-store: fail-open partition lookup for "
               << resolved.failure;
    return content::StoragePartitionConfig::CreateDefault(profile);
  }
  return resolved.config;
}

std::string_view GetEnvironmentIdForStoragePartition(
    const content::StoragePartitionConfig& config) {
  EnvironmentRegistry* registry = EnvironmentRegistry::GetInstance();
  if (!registry->enabled()) {
    return std::string_view();
  }

  const std::string id = registry->GetEnvironmentIdForConfig(config);
  if (id.empty()) {
    return std::string_view();
  }

  // Return a view into the registry's stable storage rather than into the
  // temporary above. Registry environments live for the process lifetime.
  for (const EnvironmentRegistry::Environment& environment :
       registry->environments()) {
    if (environment.id == id) {
      return environment.id;
    }
  }
  return std::string_view();
}

bool IsEnvironmentStoragePartition(
    const content::StoragePartitionConfig& config) {
  EnvironmentRegistry* registry = EnvironmentRegistry::GetInstance();
  if (!registry->enabled()) {
    return false;
  }
  return registry->IsParraPartition(config);
}

std::string GetEnvironmentIdFromPartitionConfig(
    const content::StoragePartitionConfig& config) {
  // Structural, definition-free: answers "which Parra partition is this?" even
  // when the environment has since been deleted. Deliberately does not consult
  // the registry's definition list, because a page that lives in parra/gone
  // must be reported as 'gone', not as Personal.
  EnvironmentRegistry* registry = EnvironmentRegistry::GetInstance();
  if (!registry->IsParraPartition(config)) {
    return std::string();
  }
  return config.partition_name();
}

void LogEnvironmentGuardRefusal(const char* site,
                                std::string_view environment_id,
                                std::string_view reason) {
  LOG(ERROR) << "parra-guard: refuse=" << site << " env='"
             << (environment_id.empty() ? std::string_view("<empty>")
                                        : environment_id)
             << "' reason='" << reason << "'";
}

ResolvedEnvironment ResolveEnvironmentForRestore(
    Profile* profile,
    std::string_view environment_id) {
  ResolvedEnvironment resolved;
  content::BrowserContext* browser_context = profile;

  // Empty id means the tab predates Parra or was created by upstream code.
  // Treating that as Personal is correct and is NOT a silent downgrade: nobody
  // ever recorded an environment for this tab, so there is nothing to restore.
  if (environment_id.empty()) {
    resolved.status = EnvironmentResolution::kDefaultPartition;
    resolved.config =
        content::StoragePartitionConfig::CreateDefault(browser_context);
    return resolved;
  }

  // Delegate the definition lookup to the single fail-closed resolver so
  // restore and tab creation can never disagree about what "this environment
  // exists" means.
  const PartitionResolution lookup =
      ResolvePartitionForEnvironment(profile, environment_id);

  switch (lookup.lookup) {
    case EnvironmentLookup::kPersonal:
      resolved.status = EnvironmentResolution::kDefaultPartition;
      resolved.config = lookup.config;
      return resolved;

    case EnvironmentLookup::kNamed:
      resolved.config = lookup.config;
      resolved.status = resolved.config.is_default()
                            ? EnvironmentResolution::kDefaultPartition
                            : EnvironmentResolution::kDedicatedPartition;
      return resolved;

    case EnvironmentLookup::kDisabled:
      // The partition and its data are intact; only activation is blocked. The
      // tab is restored so the user does not lose it.
      resolved.config = lookup.config;
      resolved.status = EnvironmentResolution::kDisabledEnvironment;
      return resolved;

    case EnvironmentLookup::kNotSpecified:
      // Cannot happen: the empty id was handled above. Treat defensively as
      // Personal rather than refusing a tab that recorded no environment.
      resolved.status = EnvironmentResolution::kDefaultPartition;
      resolved.config =
          content::StoragePartitionConfig::CreateDefault(browser_context);
      return resolved;

    case EnvironmentLookup::kUnknown:
      // Refusing here is the whole point of this function: loading the URL on
      // the default partition would send requests with Personal's cookies and
      // could also hit Personal's host mapping, which is exactly the leak this
      // is meant to prevent.
      resolved.status = EnvironmentResolution::kUnknownEnvironment;
      return resolved;
  }

  resolved.status = EnvironmentResolution::kUnknownEnvironment;
  return resolved;
}

std::string GetHostMappingRulesForPartitionPath(
    const base::FilePath& relative_partition_path) {
  EnvironmentRegistry* registry = EnvironmentRegistry::GetInstance();
  if (!registry->enabled()) {
    return std::string();
  }

  return registry->GetHostMappingRulesForPartitionPath(relative_partition_path);
}

std::string GetHostMappingRulesForPartitionPath(
    Profile* profile,
    const base::FilePath& relative_partition_path) {
  // The store is the authority when the profile has one. Falling back to the
  // registry only for shells without a store (unit tests, very early startup)
  // keeps those working without letting the registry answer for a real profile.
  if (EnvironmentStore* store =
          profile ? EnvironmentStore::GetForProfile(profile) : nullptr) {
    const std::string rules =
        store->GetHostMappingRulesForPartitionPath(relative_partition_path);
    if (!rules.empty()) {
      return rules;
    }
    // A stored profile with NO rules for this partition is a real answer ("no
    // remapping"), not a miss. Do not fall through to the registry: doing so
    // would let a stale command-line definition silently re-route a page in a
    // profile whose stored configuration says otherwise.
    if (store->has_user_defined_environments() ||
        relative_partition_path.empty()) {
      return std::string();
    }
  }

  EnvironmentRegistry* registry = EnvironmentRegistry::GetInstance();
  if (!registry->enabled()) {
    return std::string();
  }
  return registry->GetHostMappingRulesForPartitionPath(relative_partition_path);
}

void LogParraNavigationDiagnostics(std::string_view environment_id,
                                   const std::string& url) {
  const base::CommandLine* command_line =
      base::CommandLine::ForCurrentProcess();
  if (!command_line->HasSwitch(switches::kParraPocVerbose)) {
    return;
  }

  LOG(INFO) << "parra: navigate env='" << environment_id << "' url=" << url;
}

}  // namespace parra
