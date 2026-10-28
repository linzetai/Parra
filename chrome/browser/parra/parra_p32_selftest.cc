// Copyright 2026 The Parra Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chrome/browser/parra/parra_p32_selftest.h"

#include <string>
#include <vector>

#include "base/command_line.h"
#include "base/functional/bind.h"
#include "base/location.h"
#include "base/logging.h"
#include "base/run_loop.h"
#include "base/strings/strcat.h"
#include "base/strings/string_number_conversions.h"
#include "base/strings/string_split.h"
#include "base/strings/string_util.h"
#include "base/task/single_thread_task_runner.h"
#include "base/time/time.h"
#include "chrome/browser/parra/environment.h"
#include "chrome/browser/parra/environment_serialization.h"
#include "chrome/browser/parra/environment_store.h"
#include "chrome/browser/parra/environment_tag.h"
#include "chrome/browser/parra/parra_storage_partition_util.h"
#include "chrome/browser/parra/parra_switches.h"
#include "chrome/browser/profiles/profile.h"
#include "chrome/browser/ui/browser_window/public/browser_window_interface.h"
#include "chrome/browser/ui/browser_window/public/browser_window_interface_iterator.h"
#include "chrome/browser/ui/navigator/browser_navigator.h"
#include "chrome/browser/ui/navigator/browser_navigator_params.h"
#include "chrome/common/pref_names.h"
#include "components/prefs/pref_service.h"
#include "ui/base/base_window.h"
#include "url/gurl.h"

namespace parra {

namespace {

int g_checks = 0;
int g_failures = 0;

void Report(bool ok, const std::string& test, const std::string& detail) {
  ++g_checks;
  if (!ok)
    ++g_failures;
  LOG(INFO) << "PARRA-P32 " << (ok ? "PASS" : "FAIL") << " " << test << " | "
            << detail;
}

std::string Describe(StoreResult result) {
  return StoreResultToString(result);
}

// ---------------------------------------------------------------------------
// A. Data model and serialization (pure, no browser state touched)
// ---------------------------------------------------------------------------

void TestSerializationRoundTrip() {
  std::vector<Environment> input;
  Environment a;
  a.id = "test-a";
  a.name = "Test A";
  a.color = "#2E7D32";
  a.icon = "beaker";
  a.host_mappings = "map sheet.test 127.0.0.1:8443";
  a.created_at = base::Time::UnixEpoch() + base::Milliseconds(1793000000000);
  a.updated_at = base::Time::UnixEpoch() + base::Milliseconds(1793000001200);
  a.enabled = true;
  input.push_back(a);

  Environment b;
  b.id = "test-b";
  b.name = "Test B";
  b.host_mappings = "map sheet.test 127.0.0.2:8444, exclude localhost";
  b.created_at = base::Time::UnixEpoch() + base::Milliseconds(1793000002000);
  b.updated_at = b.created_at;
  b.enabled = false;
  input.push_back(b);

  const std::string blob = SerializeEnvironments(input);
  const ParsedEnvironments parsed = ParseEnvironments(blob);

  bool ok = parsed.status == ParseStatus::kOk &&
            parsed.environments.size() == input.size();
  std::string detail = "status=" + std::string(ParseStatusToString(parsed.status)) +
                       " count=" + base::NumberToString(parsed.environments.size());
  if (ok) {
    for (size_t i = 0; i < input.size(); ++i) {
      const Environment& expected = input[i];
      const Environment& actual = parsed.environments[i];
      if (expected.id != actual.id || expected.name != actual.name ||
          expected.color != actual.color || expected.icon != actual.icon ||
          expected.host_mappings != actual.host_mappings ||
          expected.enabled != actual.enabled ||
          expected.created_at != actual.created_at ||
          expected.updated_at != actual.updated_at) {
        ok = false;
        detail += " [mismatch at index " + base::NumberToString(i) + "]";
        break;
      }
    }
  }
  // Idempotence: serialising the parse result must reproduce the same blob. This
  // is what makes "did anything change?" answerable from the stored value.
  if (ok) {
    const std::string again = SerializeEnvironments(parsed.environments);
    if (again != blob) {
      ok = false;
      detail += " [not round-trip stable]";
    }
  }
  Report(ok, "A1-serialization-round-trip", detail);
}

void TestOrderPreserved() {
  std::vector<std::string> ids = {"zeta", "alpha", "mid"};
  std::vector<Environment> input;
  for (const std::string& id : ids) {
    Environment e;
    e.id = id;
    e.name = id;
    e.created_at = base::Time::UnixEpoch();
    e.updated_at = e.created_at;
    input.push_back(e);
  }
  const ParsedEnvironments parsed = ParseEnvironments(SerializeEnvironments(input));
  bool ok = parsed.environments.size() == ids.size();
  std::string actual;
  for (size_t i = 0; ok && i < ids.size(); ++i) {
    actual += parsed.environments[i].id + ",";
    ok = parsed.environments[i].id == ids[i];
  }
  Report(ok, "A2-order-preserved",
         "expected=" + base::JoinString(ids, ",") + " actual=" + actual);
}

void TestUnknownFieldsSurvive() {
  // A newer binary may add fields. This binary must not delete them, even though
  // it neither understands nor acts on them.
  const std::string blob = base::StrCat(
      {"{\"schemaVersion\":1,\"environments\":[{\"id\":\"test-a\",",
       "\"name\":\"Test A\",\"futureField\":\"keep me\"}]}"});
  const ParsedEnvironments parsed = ParseEnvironments(blob);
  // The known fields parse; the unknown one is neither required nor rejected.
  Report(parsed.status == ParseStatus::kOk && parsed.environments.size() == 1 &&
             parsed.environments[0].id == "test-a",
         "A3-unknown-fields-tolerated",
         "status=" + std::string(ParseStatusToString(parsed.status)));
}

void TestTooNewSchemaIsReadOnly() {
  const std::string blob = base::StrCat(
      {"{\"schemaVersion\":99,\"environments\":[{\"id\":\"test-a\",",
       "\"name\":\"Test A\"}]}"});
  const ParsedEnvironments parsed = ParseEnvironments(blob);
  Report(parsed.status == ParseStatus::kTooNew && parsed.read_only() &&
             !parsed.usable(),
         "A4-too-new-schema-read-only",
         "status=" + std::string(ParseStatusToString(parsed.status)) +
             " read_only=" + (parsed.read_only() ? "true" : "false"));
}

void TestCorruptIsDetected() {
  const ParsedEnvironments parsed = ParseEnvironments("{not json");
  Report(parsed.status == ParseStatus::kCorrupt && !parsed.usable(),
         "A5-corrupt-detected",
         "status=" + std::string(ParseStatusToString(parsed.status)));
}

void TestPartialInvalidKeepsGoodRecords() {
  // One bad record must not cost the user every environment, but it must be
  // counted and explained rather than silently dropped.
  const std::string blob = base::StrCat(
      {"{\"schemaVersion\":1,\"environments\":[",
       "{\"id\":\"good-one\",\"name\":\"Good\"},",
       "{\"id\":\"BAD ID\",\"name\":\"Bad\"},",
       "{\"id\":\"another-good\",\"name\":\"Also good\"}]}"});
  const ParsedEnvironments parsed = ParseEnvironments(blob);
  Report(parsed.status == ParseStatus::kPartiallyInvalid &&
             parsed.environments.size() == 2 && parsed.rejected_count == 1 &&
             !parsed.diagnostics.empty(),
         "A6-partial-invalid-keeps-good",
         "count=" + base::NumberToString(parsed.environments.size()) +
             " rejected=" + base::NumberToString(parsed.rejected_count));
}

void TestInvalidHostMappingRejected() {
  std::string error;
  const bool accepted = ValidateHostMappings("this is not a rule", &error);
  const bool empty_ok = ValidateHostMappings("", nullptr);
  const bool valid_ok =
      ValidateHostMappings("map sheet.test 127.0.0.1:8443", nullptr);
  Report(!accepted && empty_ok && valid_ok && !error.empty(),
         "A7-invalid-host-mapping-rejected",
         "accepted_invalid=" + std::string(accepted ? "true" : "false") +
             " empty_ok=" + (empty_ok ? "true" : "false") +
             " valid_ok=" + (valid_ok ? "true" : "false") + " error='" + error +
             "'");
}

// ---------------------------------------------------------------------------
// B. CRUD on the live profile's store
// ---------------------------------------------------------------------------

void TestCrud(EnvironmentStore* store) {
  // Clean slate for the ids this test owns. Refusals here are fine: the ids may
  // not exist on the first run.
  std::string ignored;
  store->Delete("p32-create", &ignored);
  store->Delete("p32-update", &ignored);

  // B1: create
  EnvironmentDraft draft;
  draft.id = "p32-create";
  draft.name = "P32 Create";
  draft.color = "#123456";
  draft.icon = "beaker";
  draft.host_mappings = "map sheet.test 127.0.0.1:8443";
  std::string error;
  const StoreResult created = store->Create(draft, &error);
  const Environment* found = store->Find("p32-create");
  Report(created == StoreResult::kOk && found != nullptr,
         "B1-create", Describe(created) + " error='" + error + "'");

  // B2: the id equals the partition name, and it is the only mapping.
  if (found) {
    const content::StoragePartitionConfig config =
        store->GetStoragePartitionConfig("p32-create");
    Report(!config.is_default() && config.partition_name() == found->id,
           "B2-id-equals-partition-name",
           "partition_name='" + config.partition_name() + "' id='" + found->id +
               "'");
  } else {
    Report(false, "B2-id-equals-partition-name", "no environment");
  }

  // B3: duplicate id rejected, original untouched
  const StoreResult duplicate = store->Create(draft, &error);
  const Environment* after_duplicate = store->Find("p32-create");
  Report(duplicate == StoreResult::kDuplicateId && after_duplicate &&
             after_duplicate->name == "P32 Create",
         "B3-duplicate-rejected",
         Describe(duplicate) + " name='" +
             (after_duplicate ? after_duplicate->name : "<none>") + "'");

  // B4: reserved id rejected
  EnvironmentDraft personal_draft;
  personal_draft.id = kDefaultEnvironmentId;
  personal_draft.name = "Personal";
  const StoreResult reserved = store->Create(personal_draft, &error);
  Report(reserved == StoreResult::kReservedId, "B4-reserved-id-rejected",
         Describe(reserved));

  // B5: invalid id rejected
  EnvironmentDraft bad_id;
  bad_id.id = "Bad Id";
  bad_id.name = "Bad";
  const StoreResult invalid = store->Create(bad_id, &error);
  Report(invalid == StoreResult::kInvalidId, "B5-invalid-id-rejected",
         Describe(invalid));

  // B6: invalid host mapping rejected
  EnvironmentDraft bad_rules;
  bad_rules.id = "p32-badrules";
  bad_rules.name = "Bad rules";
  bad_rules.host_mappings = "definitely not a rule";
  const StoreResult bad_mapping = store->Create(bad_rules, &error);
  const bool not_stored = store->Find("p32-badrules") == nullptr;
  Report(bad_mapping == StoreResult::kInvalidHostMapping && not_stored,
         "B6-invalid-host-mapping-rejected",
         Describe(bad_mapping) + " stored=" +
             (not_stored ? "false" : "true"));

  // B7: update changes only the supplied fields, and updated_at moves while
  // created_at does not.
  const base::Time created_at_before =
      after_duplicate ? after_duplicate->created_at : base::Time();
  EnvironmentPatch patch;
  patch.name = std::string("P32 Renamed");
  const StoreResult updated = store->Update("p32-create", patch, &error);
  const Environment* after_update = store->Find("p32-create");
  Report(updated == StoreResult::kOk && after_update &&
             after_update->name == "P32 Renamed" &&
             after_update->created_at == created_at_before &&
             after_update->updated_at != created_at_before,
         "B7-update-preserves-identity",
         Describe(updated) + " name='" +
             (after_update ? after_update->name : "<none>") + "'");

  // B8: update of a missing id is kNotFound, not a silent no-op.
  EnvironmentPatch missing_patch;
  missing_patch.name = std::string("nope");
  const StoreResult missing =
      store->Update("p32-does-not-exist", missing_patch, &error);
  Report(missing == StoreResult::kNotFound, "B8-update-missing-not-found",
         Describe(missing));

  // B9: Personal cannot be updated or deleted.
  const StoreResult personal_update = store->Update(kDefaultEnvironmentId,
                                                    missing_patch, &error);
  const StoreResult personal_delete = store->Delete(kDefaultEnvironmentId,
                                                    &error);
  Report(personal_update == StoreResult::kReservedId &&
             personal_delete == StoreResult::kReservedId,
         "B9-personal-immutable",
         "update=" + Describe(personal_update) +
             " delete=" + Describe(personal_delete));

  // B10: Personal is always resolvable even though it is never stored.
  const ResolvedEnvironmentLookup personal_lookup =
      store->ResolveForEnvironment(kDefaultEnvironmentId);
  Report(personal_lookup.ok() &&
             personal_lookup.kind == StoreLookupKind::kPersonal &&
             store->Find(kDefaultEnvironmentId) == nullptr,
         "B10-personal-implicit",
         "ok=" + std::string(personal_lookup.ok() ? "true" : "false") +
             " stored=" +
             (store->Find(kDefaultEnvironmentId) ? "true" : "false"));
}

// ---------------------------------------------------------------------------
// C. Enable / disable semantics
// ---------------------------------------------------------------------------

void TestDisabledSemantics(EnvironmentStore* store) {
  std::string error;
  EnvironmentDraft draft;
  draft.id = "p32-disable";
  draft.name = "P32 Disable";
  draft.host_mappings = "map sheet.test 127.0.0.3:8445";
  store->Delete("p32-disable", &error);
  store->Create(draft, &error);

  // Partition identity must be captured BEFORE the disable so the comparison is
  // meaningful: disabling must not move the data.
  const ResolvedEnvironmentLookup before =
      store->ResolveForEnvironment("p32-disable");

  const StoreResult disabled = store->SetEnabled("p32-disable", false, &error);
  const ResolvedEnvironmentLookup after =
      store->ResolveForEnvironment("p32-disable");
  const Environment* still_listed = store->Find("p32-disable");

  Report(disabled == StoreResult::kOk && before.ok() && !after.ok() &&
             after.kind == StoreLookupKind::kDisabled && still_listed != nullptr,
         "C1-disabled-refused-but-retained",
         "result=" + Describe(disabled) + " kind=" +
             base::NumberToString(static_cast<int>(after.kind)) +
             " listed=" + (still_listed ? "true" : "false"));

  // The partition the disabled environment maps to must be unchanged: `enabled`
  // is a selection-layer switch and must never re-route or re-locate storage.
  const content::StoragePartitionConfig config_before =
      before.ok() ? before.config : content::StoragePartitionConfig();
  const content::StoragePartitionConfig config_after =
      store->GetStoragePartitionConfig("p32-disable");
  Report(!config_after.is_default() &&
             config_after.partition_name() == "p32-disable" &&
             config_after == config_before,
         "C2-disabled-keeps-partition",
         "partition_name='" + config_after.partition_name() +
             "' in_memory=" + (config_after.in_memory() ? "true" : "false"));

  // Re-enabling restores usability.
  const StoreResult re_enabled = store->SetEnabled("p32-disable", true, &error);
  const ResolvedEnvironmentLookup restored =
      store->ResolveForEnvironment("p32-disable");
  Report(re_enabled == StoreResult::kOk && restored.ok() &&
             restored.kind == StoreLookupKind::kNamed,
         "C3-reenable-restores", Describe(re_enabled));

  store->Delete("p32-disable", &error);
}

// ---------------------------------------------------------------------------
// D. Fail-closed resolution through the store
// ---------------------------------------------------------------------------

void TestFailClosed(EnvironmentStore* store) {
  // Unknown id: refused, and the returned config must not be usable.
  const ResolvedEnvironmentLookup unknown =
      store->ResolveForEnvironment("p32-not-defined");
  Report(!unknown.ok() && unknown.kind == StoreLookupKind::kUnknown &&
             !unknown.failure.empty(),
         "D1-unknown-refused",
         "ok=" + std::string(unknown.ok() ? "true" : "false") +
             " failure='" + unknown.failure + "'");

  // Empty id: allowed (a stock Chromium navigation must keep working).
  const ResolvedEnvironmentLookup empty = store->ResolveForEnvironment("");
  Report(empty.ok() && empty.kind == StoreLookupKind::kNotSpecified &&
             empty.config.is_default(),
         "D2-empty-still-allowed",
         "ok=" + std::string(empty.ok() ? "true" : "false"));

  // Structural id lookup must not depend on the definition existing: a page in a
  // deleted environment's partition must still be reported as that environment.
  //
  // The config is built through the store's own accessor for a DEFINED id, then
  // the definition is removed, then the structural lookup is asked about the very
  // same (domain, name) pair. If the two lookups collapsed into one answer, a page
  // in a deleted environment would be classified as Personal and treated as a
  // stock tab.
  std::string error;
  EnvironmentDraft ghost;
  ghost.id = "p32-ghost";
  ghost.name = "P32 Ghost";
  store->Delete("p32-ghost", &error);
  store->Create(ghost, &error);
  const content::StoragePartitionConfig ghost_config =
      store->GetStoragePartitionConfig("p32-ghost");
  const std::string structural_while_defined =
      store->GetEnvironmentIdForConfig(ghost_config);
  store->Delete("p32-ghost", &error);
  const std::string structural_after_delete =
      store->GetEnvironmentIdForConfig(ghost_config);
  const ResolvedEnvironmentLookup lookup_after_delete =
      store->ResolveForEnvironment("p32-ghost");

  Report(structural_while_defined == "p32-ghost" &&
             structural_after_delete == "p32-ghost" &&
             !lookup_after_delete.ok() &&
             lookup_after_delete.kind == StoreLookupKind::kUnknown,
         "D3-structural-identity-survives-deletion",
         "while_defined='" + structural_while_defined +
             "' after_delete='" + structural_after_delete + "' resolve_ok=" +
             (lookup_after_delete.ok() ? "true" : "false"));
}

// ---------------------------------------------------------------------------
// E. Store identity / persistence state
// ---------------------------------------------------------------------------

void TestPersistStateContract(EnvironmentStore* store,
                             base::OnceClosure on_done) {
  // The key contract from docs/parra/53 §2: a successful write must NOT report
  // kPersisted unless a CommitPendingWrite() reply has actually run. Right after a
  // CRUD call the state must be kMemoryUpdated, never kPersisted.
  std::string error;
  EnvironmentDraft draft;
  draft.id = "p32-persist";
  draft.name = "P32 Persist";
  store->Delete("p32-persist", &error);
  const StoreResult created = store->Create(draft, &error);
  const PersistState after_create = store->persist_state();
  Report(created == StoreResult::kOk &&
             after_create != PersistState::kPersisted,
         "E1-write-is-not-claimed-persisted",
         Describe(created) + " persist_state=" +
             PersistStateToString(after_create));

  // After an explicit commit the state must become kPersisted.
  //
  // The completion is driven ASYNCHRONOUSLY rather than through a nested RunLoop:
  // CommitPendingWrite's reply is posted back to this sequence from the file task
  // runner, and a nested loop on the same sequence would keep the reply queued
  // behind the loop itself, so the flush would never be observed and the state
  // would look stuck. This is a property of the prefs machinery, not a bug in the
  // store.
  store->FlushForTesting(base::BindOnce(
      [](EnvironmentStore* self, base::OnceClosure done) {
        Report(self->persist_state() == PersistState::kPersisted,
               "E2-explicit-flush-reaches-persisted",
               "persist_state=" +
                   std::string(PersistStateToString(self->persist_state())));
        if (done)
          std::move(done).Run();
      },
      store, std::move(on_done)));
}

// ---------------------------------------------------------------------------

class P32SelfTest {
 public:
  explicit P32SelfTest(Profile* profile) : profile_(profile) {}

  void Run() {
    EnvironmentStore* store = EnvironmentStore::GetForProfile(profile_);
    if (!store) {
      Report(false, "P0-store-exists", "no EnvironmentStore for this profile");
      Finish();
      return;
    }
    Report(true, "P0-store-exists",
           "environments=" + base::NumberToString(store->environments().size()) +
               " read_only=" + (store->read_only() ? "true" : "false"));

    // A: format and validation, independent of live browser state.
    TestSerializationRoundTrip();
    TestOrderPreserved();
    TestUnknownFieldsSurvive();
    TestTooNewSchemaIsReadOnly();
    TestCorruptIsDetected();
    TestPartialInvalidKeepsGoodRecords();
    TestInvalidHostMappingRejected();

    // B..E: live store.
    TestCrud(store);
    TestDisabledSemantics(store);
    TestFailClosed(store);

    // E is asynchronous: the flush completion arrives on this sequence after the
    // file task runner has run the write. Cleanup and the summary therefore happen
    // in its completion callback rather than after this call returns.
    TestPersistStateContract(
        store, base::BindOnce(&P32SelfTest::OnPersistChecked,
                              base::Unretained(this), store));
  }

 private:
  void OnPersistChecked(EnvironmentStore* store) {
    // Leave the profile as we found it for the ids this test created.
    std::string ignored;
    store->Delete("p32-create", &ignored);
    store->Delete("p32-persist", &ignored);
    store->FlushForTesting(base::OnceClosure());
    Finish();
  }

  void Finish() {
    LOG(INFO) << "PARRA-P32 SUMMARY checks=" << g_checks
              << " failures=" << g_failures << " result="
              << (g_failures == 0 ? "ALL-PASS" : "FAILURES");
    LOG(INFO) << "PARRA-P32 EXITING";
    if (BrowserWindowInterface* browser =
            GetLastActiveBrowserWindowInterfaceWithAnyProfile()) {
      browser->GetWindow()->Close();
    }
  }

  raw_ptr<Profile> profile_;
};

// ---------------------------------------------------------------------------
// Auxiliary restart modes
// ---------------------------------------------------------------------------

// Creates one definition, flushes it to disk, reports what it created, and exits.
//
// `spec` is "<id>#<host rules>". The flush is what makes this meaningful: without
// it the definition would only be in memory and the next process would see
// nothing, which is exactly the failure the persistence contract guards against.
//
// `#` rather than `|` as the separator: these switches are passed through
// cmd.exe, where `|` is a pipe operator and would split the argument before the
// browser ever sees it.
void RunSeedMode(EnvironmentStore* store, const std::string& spec) {
  const size_t separator = spec.find('#');
  const std::string id = separator == std::string::npos
                             ? spec
                             : spec.substr(0, separator);
  const std::string rules = separator == std::string::npos
                                ? std::string()
                                : spec.substr(separator + 1);

  EnvironmentDraft draft;
  draft.id = id;
  draft.name = "Seeded " + id;
  draft.host_mappings = rules;

  std::string error;
  // Idempotent: a second seed of the same id updates it rather than failing, so
  // an interrupted experiment can be re-run.
  StoreResult result = store->Create(draft, &error);
  if (result == StoreResult::kDuplicateId) {
    EnvironmentPatch patch;
    patch.name = draft.name;
    patch.host_mappings = rules;
    result = store->Update(id, patch, &error);
  }
  Report(result == StoreResult::kOk, "S1-seed-created",
         Describe(result) + " id='" + id + "' rules='" + rules + "'");

  store->FlushForTesting(base::BindOnce(
      [](EnvironmentStore* self) {
        Report(self->persist_state() == PersistState::kPersisted,
               "S2-seed-flushed",
               "persist_state=" +
                   std::string(PersistStateToString(self->persist_state())));
        LOG(INFO) << "PARRA-P32 SEED-DONE";
        if (BrowserWindowInterface* browser =
                GetLastActiveBrowserWindowInterfaceWithAnyProfile()) {
          browser->GetWindow()->Close();
        }
      },
      store));
}

// Flips the `enabled` flag of one stored definition and exits.
//
// `spec` is "<id>#<0|1>". `enabled` is selection-layer state that lives in the
// profile's own preference, so the cross-profile half of the isolation proof
// needs one process per profile: neither process can see the other's pref.
void RunSetEnabledMode(EnvironmentStore* store, const std::string& spec) {
  const size_t separator = spec.find('#');
  const std::string id = separator == std::string::npos
                             ? spec
                             : spec.substr(0, separator);
  const bool enabled = separator != std::string::npos &&
                       spec.substr(separator + 1) == "1";

  if (store->Find(id) == nullptr) {
    Report(false, "E1-set-enabled-target-exists", "id='" + id + "' MISSING");
    LOG(INFO) << "PARRA-P32 SETENABLED-DONE";
    if (BrowserWindowInterface* browser =
            GetLastActiveBrowserWindowInterfaceWithAnyProfile()) {
      browser->GetWindow()->Close();
    }
    return;
  }

  std::string error;
  const StoreResult result = store->SetEnabled(id, enabled, &error);

  // Resolve AFTER the flip: a disabled environment must be refused with
  // kDisabled, and a re-enabled one must resolve again. Either way the
  // partition the definition maps to may never change, because `enabled` is
  // selection state and not storage identity.
  const ResolvedEnvironmentLookup resolved = store->ResolveForEnvironment(id);
  const content::StoragePartitionConfig config =
      store->GetStoragePartitionConfig(id);

  Report(result == StoreResult::kOk, "E1-set-enabled-applied",
         Describe(result) + " id='" + id + "' enabled=" +
             (enabled ? "true" : "false"));

  Report(resolved.ok() == enabled, "E2-resolve-matches-enabled",
         "enabled=" + std::string(enabled ? "true" : "false") +
             " resolve_ok=" + (resolved.ok() ? "true" : "false") +
             " resolve_kind_int=" +
             base::NumberToString(static_cast<int>(resolved.kind)));

  Report(config.partition_domain() == "parra" && config.partition_name() == id,
         "E3-partition-identity-stable",
         "partition={domain='" + config.partition_domain() +
             "' name='" + config.partition_name() + "'}");

  // Persist before exiting: a second process on the same profile must observe the
  // flipped flag, and that is the whole point of the cross-profile comparison.
  store->FlushForTesting(base::BindOnce(
      [](EnvironmentStore* self) {
        Report(self->persist_state() == PersistState::kPersisted,
               "E4-set-enabled-flushed",
               "persist_state=" +
                   std::string(PersistStateToString(self->persist_state())));
        LOG(INFO) << "PARRA-P32 SETENABLED-DONE";
        if (BrowserWindowInterface* browser =
                GetLastActiveBrowserWindowInterfaceWithAnyProfile()) {
          browser->GetWindow()->Close();
        }
      },
      store));
}

// Reads back what a previous process stored.
//
// `spec` is a comma separated list of ids that MUST exist. Anything missing is a
// failure: a definition that silently disappeared would leave its tabs unable to
// resolve, and would only be noticed when a page lands in the wrong place.
void RunVerifyMode(EnvironmentStore* store, const std::string& spec) {
  const std::vector<std::string> expected = base::SplitString(
      spec, ",", base::TRIM_WHITESPACE, base::SPLIT_WANT_ALL);

  int present = 0;
  std::string detail;
  for (const std::string& id : expected) {
    const Environment* environment = store->Find(id);
    if (environment) {
      ++present;
      detail += " [" + id + ": name='" + environment->name + "' rules='" +
                environment->host_mappings + "' enabled=" +
                (environment->enabled ? "true" : "false") + "]";
    } else {
      detail += " [" + id + ": MISSING]";
    }
  }

  Report(present == static_cast<int>(expected.size()),
         "V1-definitions-survived-restart",
         "present=" + base::NumberToString(present) + "/" +
             base::NumberToString(expected.size()) + detail);

  // Announce the full stored order so an experiment can check that restart did
  // not reorder the list (order is the display order, and there is no other
  // ordering field).
  std::string order;
  for (const Environment& environment : store->environments()) {
    if (!order.empty())
      order += ",";
    order += environment.id;
  }
  LOG(INFO) << "PARRA-P32 ORDER=" << order;

  // Report the store identity so a two-profile experiment can prove the two
  // processes did not share one instance.
  LOG(INFO) << "PARRA-P32 STORE_ADDR="
            << base::NumberToString(reinterpret_cast<uintptr_t>(store));

  LOG(INFO) << "PARRA-P32 VERIFY-DONE";
  if (BrowserWindowInterface* browser =
          GetLastActiveBrowserWindowInterfaceWithAnyProfile()) {
    browser->GetWindow()->Close();
  }
}

// Implementation of the open mode. Kept here so every P3.2 harness entry point
// lives in one anonymous namespace and the header exposes one function per mode.
// Opens one tab in a STORED environment and leaves the browser running.
// entry point per mode.
//
// This exists so the network half of the isolation proof can be run without a UI:
// the definitions come from prefs (written by the seed mode), and the tab is
// created through the normal navigation path so the environment tag and the
// StoragePartition are decided by the ordinary routing code -- not by anything
// specific to this test.
//
// If the stored definition were not actually in effect, the page would land in the
// default partition and the CDP inspection would see the wrong backend. That is
// exactly the failure this mode is designed to make visible.
void OpenInStoredEnvironment() {
  const base::CommandLine* command_line =
      base::CommandLine::ForCurrentProcess();
  if (!command_line->HasSwitch(switches::kParraPocP32Open))
    return;

  const std::string spec =
      command_line->GetSwitchValueASCII(switches::kParraPocP32Open);
  const size_t separator = spec.find('#');
  const std::string id =
      separator == std::string::npos ? spec : spec.substr(0, separator);
  const std::string url =
      separator == std::string::npos ? std::string() : spec.substr(separator + 1);

  BrowserWindowInterface* browser =
      GetLastActiveBrowserWindowInterfaceWithAnyProfile();
  if (!browser || !browser->GetProfile()) {
    LOG(ERROR) << "PARRA-P32 no browser window/profile";
    return;
  }
  Profile* profile = browser->GetProfile();

  base::SingleThreadTaskRunner::GetCurrentDefault()->PostDelayedTask(
      FROM_HERE,
      base::BindOnce(
          [](BrowserWindowInterface* window, Profile* profile, std::string id,
             std::string url) {
            EnvironmentStore* store = EnvironmentStore::GetForProfile(profile);
            if (!store) {
              LOG(ERROR) << "PARRA-P32-OPEN no store";
              return;
            }
            const ResolvedEnvironmentLookup resolved =
                store->ResolveForEnvironment(id);
            if (!resolved.ok()) {
              // Refusing here is the correct outcome: this mode exists to prove
              // that a STORED environment is honoured, so an unresolvable id must
              // not be papered over with a default-partition tab.
              LogEnvironmentGuardRefusal("p32_open", id, resolved.failure);
              return;
            }

            LOG(INFO) << "PARRA-P32-OPEN env='" << resolved.environment_id
                      << "' partition=" << resolved.config << " url='" << url
                      << "' rules='" << store->GetHostMappingRulesForPartitionPath(
                             EnvironmentStore::GetRelativePartitionPathForConfig(
                                 resolved.config))
                      << "'";

            NavigateParams params(window, GURL(url),
                                  ui::PAGE_TRANSITION_TYPED);
            params.disposition = WindowOpenDisposition::kNewForegroundTab;
            // Name the environment explicitly: this mode's whole purpose is to
            // open a tab in a specific STORED environment, not to inherit from
            // whatever tab happens to be active.
            params.parra_environment_id = id;
            Navigate(&params);
          },
          browser, profile, id, url),
      base::Seconds(2));
}

}  // namespace

void MaybeRunP32Open() {
  OpenInStoredEnvironment();
}

void MaybeRunP32SelfTest() {
  // Two auxiliary modes exist so that "does it survive a restart" can be answered
  // by two separate PROCESSES rather than by re-reading memory:
  //
  //   --parra-poc-p32-seed="<id>#<rules>"        create + flush + exit
  //   --parra-poc-p32-verify="<id>,<id>,..."     read back, compare, exit
  //   --parra-poc-p32-set-enabled="<id>#<0|1>"   flip `enabled` + flush + exit
  //
  // A single-process test cannot prove persistence: it would only prove that the
  // in-memory map still holds what was just put there. These modes are the
  // evidence for docs/parra/57 §2 and docs/parra/60 §4.
  const base::CommandLine* command_line =
      base::CommandLine::ForCurrentProcess();

  if (command_line->HasSwitch(switches::kParraPocP32Seed) ||
      command_line->HasSwitch(switches::kParraPocP32Verify) ||
      command_line->HasSwitch(switches::kParraPocP32SetEnabled)) {
    BrowserWindowInterface* seed_browser =
        GetLastActiveBrowserWindowInterfaceWithAnyProfile();
    if (!seed_browser || !seed_browser->GetProfile()) {
      LOG(ERROR) << "PARRA-P32 no browser window/profile";
      return;
    }
    Profile* seed_profile = seed_browser->GetProfile();

    // Exactly one aux mode runs per process; the order here mirrors the
    // precedence used to pick the switch value below.
    enum class AuxMode { kSeed, kVerify, kSetEnabled };
    AuxMode mode = AuxMode::kSeed;
    const char* switch_name = switches::kParraPocP32Seed;
    if (command_line->HasSwitch(switches::kParraPocP32Verify)) {
      mode = AuxMode::kVerify;
      switch_name = switches::kParraPocP32Verify;
    } else if (command_line->HasSwitch(switches::kParraPocP32SetEnabled)) {
      mode = AuxMode::kSetEnabled;
      switch_name = switches::kParraPocP32SetEnabled;
    }

    base::SingleThreadTaskRunner::GetCurrentDefault()->PostDelayedTask(
        FROM_HERE,
        base::BindOnce(
            [](Profile* profile, AuxMode aux_mode, std::string value) {
              EnvironmentStore* store = EnvironmentStore::GetForProfile(profile);
              if (!store) {
                Report(false, "AUX-store-exists", "no store");
                return;
              }
              switch (aux_mode) {
                case AuxMode::kSeed:
                  RunSeedMode(store, value);
                  break;
                case AuxMode::kVerify:
                  RunVerifyMode(store, value);
                  break;
                case AuxMode::kSetEnabled:
                  RunSetEnabledMode(store, value);
                  break;
              }
            },
            seed_profile, mode,
            command_line->GetSwitchValueASCII(switch_name)),
        base::Seconds(4));
    return;
  }

  if (!command_line->HasSwitch(switches::kParraPocP32SelfTest)) {
    return;
  }

  BrowserWindowInterface* browser =
      GetLastActiveBrowserWindowInterfaceWithAnyProfile();
  if (!browser || !browser->GetProfile()) {
    LOG(ERROR) << "PARRA-P32 no browser window/profile";
    return;
  }

  // Delayed so the profile's services (and therefore the store) are fully built,
  // and so any startup tab creation has already happened.
  base::SingleThreadTaskRunner::GetCurrentDefault()->PostDelayedTask(
      FROM_HERE,
      base::BindOnce(
          [](Profile* profile) {
            P32SelfTest test(profile);
            test.Run();
          },
          browser->GetProfile()),
      base::Seconds(5));
}

}  // namespace parra
