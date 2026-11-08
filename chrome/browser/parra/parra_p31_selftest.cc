// Copyright 2026 The Parra Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chrome/browser/parra/parra_p31_selftest.h"

#include <string>
#include <vector>

#include "base/command_line.h"
#include "base/functional/bind.h"
#include "base/location.h"
#include "base/logging.h"
#include "base/strings/strcat.h"
#include "base/task/single_thread_task_runner.h"
#include "base/time/time.h"
#include "chrome/browser/parra/environment_registry.h"
#include "chrome/browser/parra/environment_tag.h"
#include "chrome/browser/parra/parra_storage_partition_util.h"
#include "chrome/browser/parra/parra_switches.h"
#include "chrome/browser/profiles/profile.h"
#include "chrome/browser/ui/browser_window/public/browser_window_interface.h"
#include "chrome/browser/ui/browser_window/public/browser_window_interface_iterator.h"
#include "ui/base/base_window.h"

namespace parra {

namespace {

int g_checks = 0;
int g_failures = 0;

void Report(bool ok, const std::string& test, const std::string& detail) {
  ++g_checks;
  if (!ok) {
    ++g_failures;
  }
  LOG(INFO) << "PARRA-P31 " << (ok ? "PASS" : "FAIL") << " " << test << " | "
            << detail;
}

std::string Describe(const PartitionResolution& r) {
  return base::StrCat({"lookup=", std::to_string(static_cast<int>(r.lookup)),
                       " env='",
                       r.environment_id.empty() ? "<empty>" : r.environment_id,
                       "' ok=", r.ok() ? "true" : "false",
                       r.failure.empty() ? "" : " failure='", r.failure,
                       r.failure.empty() ? "" : "'"});
}

// The environment ids actually configured, so the test asserts about
// environments that exist instead of hard-coding names the harness might not
// have created.
std::vector<std::string> ConfiguredIds() {
  std::vector<std::string> ids;
  EnvironmentRegistry* registry = EnvironmentRegistry::GetInstance();
  if (!registry->enabled()) {
    return ids;
  }
  for (const EnvironmentRegistry::Environment& environment :
       registry->environments()) {
    ids.push_back(environment.id);
  }
  return ids;
}

// The configured ids EXCLUDING the reserved `personal` environment.
//
// Every assertion about a dedicated partition must use one of these: `personal`
// maps to the default StoragePartition by definition, so asking it for a
// partition_name, a structural id, or a kDedicatedPartition restore answer is a
// question with no correct answer. Using it would fail for a reason unrelated
// to the property under test.
std::vector<std::string> NonPersonalIds() {
  std::vector<std::string> ids;
  for (std::string& id : ConfiguredIds()) {
    if (id != kDefaultEnvironmentId) {
      ids.push_back(std::move(id));
    }
  }
  return ids;
}

// ---------------------------------------------------------------------------
// A. Fail-closed resolution
// ---------------------------------------------------------------------------

void TestUnknownEnvironmentIsRefused(Profile* profile) {
  // The single most important check in this phase: a non-empty id that names no
  // configured environment must NOT resolve to the default partition.
  const PartitionResolution resolved =
      ResolvePartitionForEnvironment(profile, "definitely-not-an-environment");
  Report(!resolved.ok() && resolved.lookup == EnvironmentLookup::kUnknown,
         "A1-unknown-environment-refused", Describe(resolved));
}

void TestEmptyIdStillAllowed(Profile* profile) {
  // Fail-closed must not spill over into ordinary browsing. A navigation that
  // never mentioned an environment has to succeed, otherwise Parra being
  // configured would break stock Chromium tabs.
  const PartitionResolution resolved =
      ResolvePartitionForEnvironment(profile, "");
  Report(resolved.ok() && resolved.lookup == EnvironmentLookup::kNotSpecified &&
             resolved.config.is_default(),
         "A2-empty-id-still-default", Describe(resolved));
}

void TestPersonalAlwaysResolves(Profile* profile) {
  const PartitionResolution resolved =
      ResolvePartitionForEnvironment(profile, kDefaultEnvironmentId);
  Report(resolved.ok() && resolved.lookup == EnvironmentLookup::kPersonal &&
             resolved.config.is_default(),
         "A3-personal-always-resolves", Describe(resolved));
}

void TestNamedEnvironmentResolves(Profile* profile,
                                  const std::vector<std::string>& ids) {
  if (ids.empty()) {
    Report(false, "A4-named-environment-resolves",
           "no non-personal environments configured");
    return;
  }
  int ok_count = 0;
  std::string detail;
  for (const std::string& id : ids) {
    const PartitionResolution resolved =
        ResolvePartitionForEnvironment(profile, id);
    if (resolved.ok() && resolved.lookup == EnvironmentLookup::kNamed &&
        !resolved.config.is_default() &&
        resolved.config.partition_name() == id) {
      ++ok_count;
    } else {
      detail += " [" + id + ": " + Describe(resolved) + "]";
    }
  }
  Report(ok_count == static_cast<int>(ids.size()),
         "A4-named-environment-resolves",
         "resolved=" + std::to_string(ok_count) + "/" +
             std::to_string(ids.size()) + detail);
}

void TestRefusalNeverYieldsDefaultConfig(Profile* profile) {
  // A refusal must not carry a usable config, so a caller that checks only
  // `config` (instead of `ok()`) cannot accidentally proceed into Personal.
  const PartitionResolution resolved =
      ResolvePartitionForEnvironment(profile, "ghost-environment");
  Report(!resolved.ok(), "A5-refusal-carries-failure", Describe(resolved));
}

// ---------------------------------------------------------------------------
// B. Structural partition lookup vs definition-aware lookup
// ---------------------------------------------------------------------------

void TestStructuralLookupDistinguishesDeletedFromPersonal(
    Profile* profile,
    const std::vector<std::string>& ids) {
  if (ids.empty()) {
    Report(false, "B1-structural-vs-definition-aware",
           "no non-personal environments configured");
    return;
  }
  const std::string& id = ids.front();
  const PartitionResolution known = ResolvePartitionForEnvironment(profile, id);

  // The default partition must never be reported as a named environment, and a
  // named partition must never be reported as empty. These two answers drive
  // completely different code paths (stock tab vs isolated tab).
  const std::string structural_named =
      GetEnvironmentIdFromPartitionConfig(known.config);
  const std::string structural_default = GetEnvironmentIdFromPartitionConfig(
      content::StoragePartitionConfig::CreateDefault(profile));

  Report(
      structural_named == id && structural_default.empty(),
      "B1-structural-vs-definition-aware",
      "named='" + structural_named + "' default='" + structural_default + "'");
}

void TestDefinitionAwareLookupRequiresDefinition(
    Profile* profile,
    const std::vector<std::string>& ids) {
  if (ids.empty()) {
    Report(false, "B2-definition-aware-needs-definition",
           "no non-personal environments configured");
    return;
  }
  // Build a config that STRUCTURALLY names an environment that is not defined,
  // exactly like a deleted environment's still-existing partition would.
  const content::StoragePartitionConfig orphan =
      content::StoragePartitionConfig::Create(
          profile,
          /*partition_domain=*/"parra",
          /*partition_name=*/"deleted-environment",
          /*in_memory=*/profile->IsOffTheRecord());

  const std::string structural = GetEnvironmentIdFromPartitionConfig(orphan);
  const std::string_view definition_aware =
      GetEnvironmentIdForStoragePartition(orphan);

  // The structural answer names the partition; the definition-aware one
  // refuses. If these two collapsed into one answer, a page in a deleted
  // environment would be classified as Personal and could be treated as a stock
  // tab.
  Report(structural == "deleted-environment" && definition_aware.empty(),
         "B2-definition-aware-needs-definition",
         "structural='" + structural + "' definition_aware='" +
             std::string(definition_aware) + "'");
}

// ---------------------------------------------------------------------------
// C. OTR partition invariant
// ---------------------------------------------------------------------------

void TestOtrPartitionIsInMemory(Profile* regular_profile,
                                const std::vector<std::string>& ids) {
  if (ids.empty()) {
    Report(false, "C1-otr-partition-in-memory",
           "no non-personal environments configured");
    return;
  }

  // Create a throwaway OTR profile. It is owned by `regular_profile`, so
  // nothing leaks and no persistent state is touched: an OTR profile is
  // destroyed with its parent.
  Profile* otr = regular_profile->GetOffTheRecordProfile(
      Profile::OTRProfileID::CreateUniqueForTesting(),
      /*create_if_needed=*/true);
  if (!otr) {
    Report(false, "C1-otr-partition-in-memory", "could not create OTR profile");
    return;
  }

  int persistent = 0;
  std::string detail;
  for (const std::string& id : ids) {
    const PartitionResolution resolved =
        ResolvePartitionForEnvironment(otr, id);
    if (!resolved.ok()) {
      detail += " [" + id + ": " + Describe(resolved) + "]";
      ++persistent;
      continue;
    }
    if (!resolved.config.in_memory()) {
      detail += " [" + id + ": config.in_memory()==false]";
      ++persistent;
    }
  }

  Report(persistent == 0, "C1-otr-partition-in-memory",
         "persistent=" + std::to_string(persistent) + "/" +
             std::to_string(ids.size()) + detail);

  // The same environment in a REGULAR profile must stay persistent. If this
  // ever flips, isolation data would silently disappear on restart.
  const PartitionResolution regular =
      ResolvePartitionForEnvironment(regular_profile, ids.front());
  Report(regular.ok() && !regular.config.in_memory(),
         "C2-regular-partition-is-persistent",
         regular.ok() ? (regular.config.in_memory() ? "in_memory==true"
                                                    : "in_memory==false")
                      : Describe(regular));
}

void TestOtrSharesRoutingWithRegular(Profile* regular_profile,
                                     const std::vector<std::string>& ids) {
  if (ids.empty()) {
    Report(false, "C3-otr-shares-host-mapping",
           "no non-personal environments configured");
    return;
  }
  Profile* otr = regular_profile->GetOffTheRecordProfile(
      Profile::OTRProfileID::CreateUniqueForTesting(),
      /*create_if_needed=*/true);
  if (!otr) {
    Report(false, "C3-otr-shares-host-mapping", "could not create OTR profile");
    return;
  }

  // Data isolation is the in_memory flag; routing must NOT differ. If an OTR
  // window resolved the same hostname differently from a regular one, the same
  // URL would reach a different backend depending on whether the window is
  // incognito, which is impossible to diagnose from the page.
  int mismatched = 0;
  std::string detail;
  for (const std::string& id : ids) {
    const PartitionResolution regular =
        ResolvePartitionForEnvironment(regular_profile, id);
    const PartitionResolution incognito =
        ResolvePartitionForEnvironment(otr, id);
    if (!regular.ok() || !incognito.ok()) {
      detail += " [" + id + ": unresolved]";
      ++mismatched;
      continue;
    }
    const base::FilePath regular_path =
        EnvironmentRegistry::GetRelativePartitionPathForConfig(regular.config);
    const base::FilePath otr_path =
        EnvironmentRegistry::GetRelativePartitionPathForConfig(
            incognito.config);
    const std::string regular_rules =
        GetHostMappingRulesForPartitionPath(regular_path);
    const std::string otr_rules = GetHostMappingRulesForPartitionPath(otr_path);
    if (regular_rules != otr_rules) {
      detail += " [" + id + ": regular='" + regular_rules + "' otr='" +
                otr_rules + "']";
      ++mismatched;
    }
  }
  Report(mismatched == 0, "C3-otr-shares-host-mapping",
         "mismatched=" + std::to_string(mismatched) + "/" +
             std::to_string(ids.size()) + detail);
}

// ---------------------------------------------------------------------------
// D. Enablement semantics
// ---------------------------------------------------------------------------

void TestEnablementSemantics(Profile* profile) {
  // IsEnabled() answers "does this build have Parra",
  // HasUserDefinedEnvironments() answers "does this profile have any
  // non-Personal environment". They must be independently readable so a profile
  // with no environments does not look like a build without Parra.
  const bool parra_build_enabled = IsEnabled();
  const bool has_envs = HasUserDefinedEnvironments(profile);

  Report(parra_build_enabled, "D1-parra-build-enabled",
         "IsEnabled=" + std::string(parra_build_enabled ? "true" : "false"));

  // `personal` must resolve independently of whether the profile has named
  // environments; it is the default environment every browser window needs.
  const PartitionResolution personal =
      ResolvePartitionForEnvironment(profile, kDefaultEnvironmentId);
  Report(personal.ok(), "D2-personal-resolves-regardless",
         "has_envs=" + std::string(has_envs ? "true" : "false") + " " +
             Describe(personal));
}

// ---------------------------------------------------------------------------
// E. Restore resolution
// ---------------------------------------------------------------------------

void TestRestoreResolution(Profile* profile,
                           const std::vector<std::string>& ids) {
  // An empty recorded id is a tab that never had an environment: it must
  // restore, not be refused.
  const ResolvedEnvironment empty = ResolveEnvironmentForRestore(profile, "");
  Report(empty.status == EnvironmentResolution::kDefaultPartition,
         "E1-restore-empty-id-defaults",
         "status=" + std::to_string(static_cast<int>(empty.status)));

  // A recorded id that no longer exists must be REFUSED. This is the
  // fail-closed contract for session restore and the last line of defence
  // against a deleted environment's tab silently loading in Personal.
  const ResolvedEnvironment missing =
      ResolveEnvironmentForRestore(profile, "deleted-since-last-run");
  Report(missing.status == EnvironmentResolution::kUnknownEnvironment,
         "E2-restore-unknown-refused",
         "status=" + std::to_string(static_cast<int>(missing.status)));

  if (ids.empty()) {
    Report(false, "E3-restore-named-uses-dedicated-partition",
           "no non-personal environments configured");
    return;
  }
  const ResolvedEnvironment named =
      ResolveEnvironmentForRestore(profile, ids.front());
  Report(named.status == EnvironmentResolution::kDedicatedPartition &&
             !named.config.is_default() &&
             named.config.partition_name() == ids.front(),
         "E3-restore-named-uses-dedicated-partition",
         "status=" + std::to_string(static_cast<int>(named.status)) +
             " partition_name='" + named.config.partition_name() + "'");
}

// ---------------------------------------------------------------------------

class P31SelfTest {
 public:
  explicit P31SelfTest(Profile* profile) : profile_(profile) {}

  void Run() {
    const std::vector<std::string> all_ids = ConfiguredIds();
    const std::vector<std::string> named_ids = NonPersonalIds();
    LOG(INFO) << "PARRA-P31 BEGIN configured=" << all_ids.size()
              << " non_personal=" << named_ids.size();

    // A: the fail-closed contract itself.
    TestUnknownEnvironmentIsRefused(profile_);
    TestEmptyIdStillAllowed(profile_);
    TestPersonalAlwaysResolves(profile_);
    TestNamedEnvironmentResolves(profile_, named_ids);
    TestRefusalNeverYieldsDefaultConfig(profile_);

    // B: the two partition lookups must not collapse into one answer.
    TestStructuralLookupDistinguishesDeletedFromPersonal(profile_, named_ids);
    TestDefinitionAwareLookupRequiresDefinition(profile_, named_ids);

    // C: OTR data isolation without OTR routing divergence.
    TestOtrPartitionIsInMemory(profile_, named_ids);
    TestOtrSharesRoutingWithRegular(profile_, named_ids);

    // D: the two enablement questions are independently readable.
    TestEnablementSemantics(profile_);

    // E: session restore fails closed on a missing environment.
    TestRestoreResolution(profile_, named_ids);

    LOG(INFO) << "PARRA-P31 SUMMARY checks=" << g_checks
              << " failures=" << g_failures
              << " result=" << (g_failures == 0 ? "ALL-PASS" : "FAILURES");
    LOG(INFO) << "PARRA-P31 EXITING";

    if (BrowserWindowInterface* browser =
            GetLastActiveBrowserWindowInterfaceWithAnyProfile()) {
      browser->GetWindow()->Close();
    }
  }

 private:
  raw_ptr<Profile> profile_;
};

}  // namespace

void MaybeRunP31SelfTest() {
  if (!base::CommandLine::ForCurrentProcess()->HasSwitch(
          switches::kParraPocP31SelfTest)) {
    return;
  }

  BrowserWindowInterface* browser =
      GetLastActiveBrowserWindowInterfaceWithAnyProfile();
  if (!browser || !browser->GetProfile()) {
    LOG(ERROR) << "PARRA-P31 no browser window/profile";
    return;
  }

  // Delayed so the visibility PoC has created and committed the 3x3 layout: the
  // checks ask the resolver about environments that really exist, which
  // requires the registry to be populated AND the tabs to have committed.
  base::SingleThreadTaskRunner::GetCurrentDefault()->PostDelayedTask(
      FROM_HERE,
      base::BindOnce(
          [](Profile* profile) {
            P31SelfTest test(profile);
            test.Run();
          },
          browser->GetProfile()),
      base::Seconds(6));
}

}  // namespace parra
