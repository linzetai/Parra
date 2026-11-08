// Copyright 2026 The Parra Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CHROME_BROWSER_PARRA_PARA_STORAGE_PARTITION_UTIL_H_
#define CHROME_BROWSER_PARRA_PARA_STORAGE_PARTITION_UTIL_H_

#include <string>
#include <string_view>

#include "base/files/file_path.h"
#include "content/public/browser/storage_partition_config.h"

class Profile;

namespace parra {

// ---------------------------------------------------------------------------
// Two independent enablement questions (Phase 3 T3.1.3)
//
// These used to be one bool, which conflated "Parra exists in this build" with
// "this profile has user-defined environments". Conflating them meant that a
// profile with no non-Personal environment silently disabled every Parra code
// path -- including paths that have nothing to do with environments -- and, far
// worse, made "this environment is gone" indistinguishable from "Parra is off".
// ---------------------------------------------------------------------------

// BUILD-LEVEL: does this binary contain Parra environment support?
//
// This is a COMPILE-TIME capability, not a runtime configuration. Parra is
// compiled into this browser unconditionally, so this is now `true` for every
// process. It deliberately no longer consults EnvironmentRegistry: that made
// "Parra exists" depend on a PoC command line switch, which meant a user who
// configured environments through the UI lost ALL of them -- including tab
// filtering -- the moment they launched without `--parra-poc-environments`.
// See docs/parra/93 §2.1 for the failure this fixes.
//
// WHAT THIS DOES **NOT** ANSWER: "should this particular operation be
// environment-aware?" That is a per-profile question (a profile with no
// environments must browse exactly like stock Chromium), and it must be
// answered with HasUserDefinedEnvironments() or with a per-window / per-model
// query. Do not use this function as a stand-in for those; doing so
// re-introduces the same conflation in a different place.
bool IsEnabled();

// Profile-level: whether `profile` has at least one user-defined (non-Personal)
// environment.
//
// This is a *capability* question, not an *authorization* question: a profile
// with zero environments still has the reserved `personal` environment, and
// ordinary browsing must keep working normally in it.
//
// Answered by the profile's EnvironmentStore (P3.2+). Only a profile with no
// store at all (unit-test shells, very early startup before the profile is
// built) falls back to the command-line registry.
bool HasUserDefinedEnvironments(Profile* profile);

// Is `environment_id` an environment this profile actually has?
//
// The question every environment-aware code path really wants: "should this
// operation behave the Parra way?". Answers true for `personal` always (it is
// reserved and always usable) and for any stored, ENABLED environment.
//
// Deliberately takes the id rather than the Profile alone, because the answer
// differs per environment: a window showing `personal` behaves like stock
// Chromium even in a profile that has five other environments configured.
bool ProfileHasEnvironment(Profile* profile, std::string_view environment_id);

// ---------------------------------------------------------------------------
// Partition resolution (Phase 3 T3.1.2)
// ---------------------------------------------------------------------------

// Why a lookup answered the way it did. Recorded so a refusal can say which
// input it refused, instead of "the environment was not found" for all of them.
enum class EnvironmentLookup {
  // Caller did not name an environment at all (empty id). The ONLY case that
  // may
  // fall back to the default partition -- a stock Chromium navigation must not
  // be
  // broken by Parra being configured.
  kNotSpecified,
  // The reserved `personal` environment: exists by definition, maps to the
  // default partition.
  kPersonal,
  // A defined, usable environment with its own partition.
  kNamed,
  // Non-empty id that this profile does not define. Covers "deleted", "typo",
  // "config was reset" and "the manifest is gone".
  kUnknown,
  // Defined but currently disabled (P3.2). Its partition and data are intact,
  // so
  // the refusal is about SELECTION only -- a caller that reports this can say
  // "this environment is switched off" rather than "it does not exist".
  kDisabled,
};

// The result of resolving an environment id to a StoragePartitionConfig.
//
// WHY THIS EXISTS: the previous API returned a bare StoragePartitionConfig and
// signalled every failure by returning the default one. A caller could not tell
// "this page belongs to Personal" from "your environment is gone", so it
// proceeded and silently loaded the page in Personal -- the exact isolation
// break this project exists to prevent. `ok()` is now the only thing a caller
// may use to decide whether it is allowed to proceed.
struct PartitionResolution {
  // True only when the caller may proceed with `config`.
  bool ok() const { return failure.empty(); }

  EnvironmentLookup lookup = EnvironmentLookup::kNotSpecified;

  // The environment the result refers to. `personal` for kPersonal, empty for
  // kNotSpecified and kUnknown.
  std::string environment_id;

  // Meaningful only when ok().
  content::StoragePartitionConfig config;

  // Non-empty means the caller MUST refuse. Never set for kNotSpecified.
  std::string failure;
};

// Resolves `environment_id` for `profile`.
//
// `empty` id -> ok(), default partition (kNotSpecified).
// `personal`  -> ok(), default partition (kPersonal).
// a defined   -> ok(), dedicated partition (kNamed).
// anything else -> NOT ok, with `failure` naming the id (kUnknown / kDisabled).
//
// This is intentionally strict. Callers that have a source of environment
// information they cannot fully trust (session data, a renderer-supplied
// string) must use this and refuse on !ok().
PartitionResolution ResolvePartitionForEnvironment(
    Profile* profile,
    std::string_view environment_id);

// Returns the StoragePartitionConfig that should be used for a new tab whose
// owning environment is `environment_id`.
//
// FAIL-OPEN, legacy: returns the default config whenever the id cannot be
// resolved. A caller that can report an error to its caller must use
// ResolvePartitionForEnvironment() instead. Kept for the startup PoC paths,
// which have no way to surface a refusal, and because removing it would change
// the behaviour of code that legitimately only wants "a config" for a known id.
content::StoragePartitionConfig GetStoragePartitionConfigForEnvironment(
    Profile* profile,
    std::string_view environment_id);

// Returns the environment id that owns `config`, but only when that environment
// is currently defined. Empty for the default partition and for undefined ones.
std::string_view GetEnvironmentIdForStoragePartition(
    const content::StoragePartitionConfig& config);

// Returns the environment id STRUCTURALLY encoded in `config`, whether or not
// it is currently defined. Empty for the default partition.
//
// Use this to answer "which Parra partition is this page really in?". The
// definition-aware variant above answers a different question ("which live
// environment is this page in?") and returns empty for a deleted environment --
// which, if used for classification, would report a page living in parra/gone
// as if it were in Personal.
std::string GetEnvironmentIdFromPartitionConfig(
    const content::StoragePartitionConfig& config);

// True when `config` is a Parra-created partition (not the default one).
bool IsEnvironmentStoragePartition(
    const content::StoragePartitionConfig& config);

// Outcome of resolving an environment id that came from persisted session data.
//
// Session restore may encounter an environment id that no longer exists (the
// environment was deleted, or the manifest was lost). Failing open to the
// default partition would silently drop isolation and let the restored page
// send requests with the wrong cookies, so the caller needs to distinguish
// "this environment is gone" from "this tab belongs to the default
// environment". Restore must then surface an explicit error state instead of
// quietly loading the URL in Personal.
enum class EnvironmentResolution {
  // Use the default partition. Covers the reserved `personal` environment and
  // every tab created outside Parra (no environment recorded).
  kDefaultPartition,
  // Use a dedicated fixed partition; `config` is filled in.
  kDedicatedPartition,
  // The id is unknown. Restore must NOT proceed with the saved URL.
  kUnknownEnvironment,
  // The environment exists but is disabled. Its partition is still valid and
  // its
  // data is intact, so the tab is restored into it; it just cannot be activated
  // until it is re-enabled. Distinct from kUnknownEnvironment because the
  // page's
  // storage is NOT at risk here.
  kDisabledEnvironment,
};

struct ResolvedEnvironment {
  EnvironmentResolution status = EnvironmentResolution::kDefaultPartition;
  content::StoragePartitionConfig config;
};

// Resolves an environment id read from SessionTab::extra_data.
//
// Unlike GetStoragePartitionConfigForEnvironment() this NEVER fails open: an
// unrecognized id yields kUnknownEnvironment so restore can refuse the load.
// An empty id yields kDefaultPartition, because an empty id means the tab never
// had an environment -- not that one went missing.
ResolvedEnvironment ResolveEnvironmentForRestore(
    Profile* profile,
    std::string_view environment_id);

// ---------------------------------------------------------------------------
// Unified refusal diagnostics (Phase 3 T3.1.4)
// ---------------------------------------------------------------------------

// Emits one line per refusal, in a single machine-greppable shape, so the
// verification scripts do not have to know about five different phrasings:
//
//   parra-guard: refuse=<site> env='<id>' reason='<why>'
//
// `site` names the decision point and must be a stable literal:
//   "new_tab"        EnvironmentTabRouter::ResolveForNewTab
//   "tab_insert"     NavigateImpl inserting contents_to_insert
//   "target_contents" CreateTargetContents
//   "restore"        browser_tabrestore.cc
//   "switch"         EnvironmentVisibilityController::SwitchTo
//
// The id is rendered as <empty> when the caller never named an environment.
void LogEnvironmentGuardRefusal(const char* site,
                                std::string_view environment_id,
                                std::string_view reason);

// Returns the net::HostMappingRules string for the partition identified by
// `relative_partition_path` (the argument that
// ContentBrowserClient::ConfigureNetworkContextParams() receives).
//
// Returns an empty string for the default partition, for non-Parra partitions,
// or when the PoC is disabled, in which case callers must leave the parameter
// unset so the global switch (if any) still applies.
std::string GetHostMappingRulesForPartitionPath(
    const base::FilePath& relative_partition_path);

// Same, but answered by `profile`'s EnvironmentStore.
//
// PREFERRED. The process-wide variant above can only see command-line
// definitions, so a profile whose environments were created through the store
// (the whole point of P3.2) would get its rules from the wrong place and the
// page would resolve to real DNS instead of the intended backend -- a silent
// routing failure with no error anywhere.
//
// `relative_partition_path` is the argument
// ContentBrowserClient::ConfigureNetworkContextParams() receives, which is the
// only partition identity available at that point.
std::string GetHostMappingRulesForPartitionPath(
    Profile* profile,
    const base::FilePath& relative_partition_path);

// Logs the effective routing for a navigation when --parra-poc-verbose is set.
// `environment_id` may be empty for non-Parra pages.
void LogParraNavigationDiagnostics(std::string_view environment_id,
                                   const std::string& url);

}  // namespace parra

#endif  // CHROME_BROWSER_PARRA_PARA_STORAGE_PARTITION_UTIL_H_
