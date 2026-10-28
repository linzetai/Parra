// Copyright 2026 The Parra Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CHROME_BROWSER_PARRA_ENVIRONMENT_STORE_H_
#define CHROME_BROWSER_PARRA_ENVIRONMENT_STORE_H_

#include <string>
#include <string_view>
#include <vector>

#include "base/callback_list.h"
#include "base/memory/raw_ptr.h"
#include "base/memory/weak_ptr.h"
#include "base/observer_list.h"
#include "base/time/time.h"
#include "chrome/browser/parra/environment.h"
#include "components/keyed_service/core/keyed_service.h"
#include "content/public/browser/storage_partition_config.h"

class PrefService;
class Profile;

namespace parra {

// Persistence state, kept SEPARATE from whether the definition is usable.
//
// The important distinction the previous design got wrong: "the in-memory
// configuration changed" and "the bytes reached the disk" are different events
// separated by up to 10 seconds (base::ImportantFileWriter's default commit
// interval) and by a background task. PrefService::SetString() has no completion
// callback, so a caller CANNOT infer persistence from it.
//
// See docs/parra/53 §2 and 56 §3 for the source evidence.
enum class PersistState {
  // Never written by this process. A fresh or freshly-parsed profile.
  kUnchanged,
  // SetString() returned: the value is in the in-memory prefs tree only. A crash
  // here loses this change.
  kMemoryUpdated,
  // CommitPendingWrite() was called and its reply callback ran, which means the
  // write task finished on the file sequence. The bytes were handed to the OS.
  kPersisted,
  // The write path reported failure, or the pref service is read-only.
  kPersistFailed,
};

const char* PersistStateToString(PersistState state);

// What caused a change notification.
enum class EnvironmentChangeKind {
  kCreated,
  kUpdated,
  kEnabledChanged,
  kRemoved,
  // The persisted blob turned out to be corrupt or unreadable at load time, so
  // every definition in it is gone. Distinct from kRemoved because the cause is a
  // configuration problem, not a user action.
  kLoadFailed,
};

// A change notification. Deliberately carries the id rather than a copy of the
// definition: a listener that wants the definition asks the store, so there is
// only ever one version of it.
struct EnvironmentChange {
  EnvironmentChangeKind kind = EnvironmentChangeKind::kUpdated;
  // Empty for kLoadFailed, which can affect every environment at once.
  std::string id;
};

// The per-Profile authority for what environments exist.
//
// OWNERSHIP
//   * One instance per Profile, created and destroyed by the KeyedService system
//     through EnvironmentStoreFactory.
//   * The store OWNS the definitions. Nothing else keeps a second copy: the
//     EnvironmentTag on a WebContents records only an id, and
//     WindowEnvironmentState records only which id a window is showing.
//   * Process-wide state is deliberately NOT used. A single global registry
//     cannot answer "which environments does THIS profile have", and two profiles
//     with different definitions for the same id would silently share one answer.
//
// LIFETIME AND THREADING
//   * Constructed on the UI thread. Every method is UI-thread only.
//   * All reads are served from memory, populated synchronously in the
//     constructor. This is a hard requirement, not an optimisation: the first
//     StoragePartition of a profile asks for host mapping rules from
//     ContentBrowserClient::ConfigureNetworkContextParams(), which is a
//     synchronous call with no way to wait for an async load. An async store
//     would answer that question with "no rules", and the page would resolve to
//     real DNS instead of the intended backend.
class EnvironmentStore : public KeyedService {
 public:
  // Returns this profile's store, or null when the profile has none (never
  // created yet, or shut down). Never creates.
  static EnvironmentStore* GetForProfile(Profile* profile);

  EnvironmentStore(const EnvironmentStore&) = delete;
  EnvironmentStore& operator=(const EnvironmentStore&) = delete;

  ~EnvironmentStore() override;

  // ---- reads (all in-memory, all O(1) or O(n) with n <= 64) ----------------

  // True when this profile has at least one user-defined, non-Personal
  // environment. This is a CAPABILITY answer: false does not mean "broken", it
  // means the profile is an ordinary browser.
  bool has_user_defined_environments() const;

  // The reserved Personal environment. Always present, never stored, never
  // removable. Returned by value because it has no stored representation.
  //
  // Its host mappings come from a dedicated pref (prefs::kParraPersonalHostMappings)
  // because Personal is never a stored record: see
  // EnvironmentStore::SetPersonalHostMappings().
  Environment PersonalEnvironment() const;

  // All stored definitions, in stored (= display) order. Excludes Personal.
  const std::vector<Environment>& environments() const;

  // Finds a stored definition. Returns null for unknown ids AND for `personal`,
  // which is not stored -- callers that must handle Personal use
  // ResolveForEnvironment() instead.
  const Environment* Find(std::string_view id) const;

  // True when `id` names a stored definition that is currently enabled. Personal
  // answers true (it cannot be disabled).
  bool IsKnownAndEnabled(std::string_view id) const;

  // ---- partition mapping -------------------------------------------------
  //
  // These exist so the low layer (navigator, session restore, network context)
  // can ask the store instead of the process-wide registry. The mapping itself is
  // unchanged from Phase 2 -- same domain, same partition name == id -- because
  // changing it would move every existing profile's data.

  // The StoragePartitionConfig for `id`, or a default config when `id` is
  // Personal. Unknown ids also return a default config: this is the FAIL-OPEN
  // accessor, kept only for callers that cannot report an error. Everything that
  // can report must use ResolveForEnvironment().
  content::StoragePartitionConfig GetStoragePartitionConfig(
      std::string_view id) const;

  // The fail-closed resolution: `ok()` decides whether a caller may proceed.
  //
  // This is the store's equivalent of P3.1's ResolvePartitionForEnvironment(), and
  // it keeps the same three-way distinction:
  //   empty id       -> kNotSpecified, ok()          (a stock Chromium navigation)
  //   "personal"     -> kPersonal,    ok()          (always available)
  //   stored+enabled -> kNamed,       ok()
  //   stored+disabled-> kDisabled,    !ok()
  //   anything else  -> kUnknown,     !ok()
  //
  // The disabled case is new in P3.2 and is the reason this cannot be expressed
  // with a bool: a disabled environment still HAS a partition and its tabs keep
  // running in it, but it must not be chosen for new tabs or activation.
  ResolvedEnvironmentLookup ResolveForEnvironment(std::string_view id) const;

  // The environment id structurally encoded in `config`, whether or not the
  // environment is still defined. Empty for the default partition.
  //
  // Deliberately independent of the definition list: a page living in
  // `parra/gone` must be reported as living in 'gone', not as Personal, or every
  // caller treats a foreign page as a stock tab.
  std::string GetEnvironmentIdForConfig(
      const content::StoragePartitionConfig& config) const;

  // The host-mapping string for the partition identified by
  // `relative_partition_path` (the argument ContentBrowserClient's
  // ConfigureNetworkContextParams receives). Empty for the default partition when
  // Person has no rules, for unknown partitions, and when the store is absent.
  std::string GetHostMappingRulesForPartitionPath(
      const base::FilePath& relative_partition_path) const;

  // Computes the relative partition path for (domain, name). Pure function,
  // mirrors content's StoragePartitionImplMap::GetStoragePartitionPath().
  static base::FilePath GetRelativePartitionPathForConfig(
      const content::StoragePartitionConfig& config);

  // ---- writes ------------------------------------------------------------

  // Creates an environment. `draft.id` becomes the immutable id.
  //
  // Validation happens against the COMPLETE resulting configuration before
  // anything is mutated, so a rejected call cannot leave the store half-updated.
  StoreResult Create(const EnvironmentDraft& draft, std::string* error);

  // Applies `patch` to `id`. Fields absent from the patch are untouched.
  StoreResult Update(std::string_view id,
                     const EnvironmentPatch& patch,
                     std::string* error);

  // Convenience wrapper over Update() for the one-field case.
  StoreResult SetEnabled(std::string_view id, bool enabled, std::string* error);

  // Sets the host-mapping rules for the reserved `personal` environment.
  //
  // `personal` maps to the DEFAULT StoragePartition, so these rules affect the
  // whole profile's default network context -- every tab that is not in a named
  // environment. That is why this is a separate call rather than an Update():
  // the blast radius is different from any other environment.
  //
  // Needed because Personal is implicit (never a stored record), so its routing
  // had no place to live and the command-line import silently dropped it.
  StoreResult SetPersonalHostMappings(std::string_view host_mappings,
                                      std::string* error);

  // Removes the definition. DOES NOT touch any StoragePartition: see
  // docs/parra/53 §3 (D2). This build refuses when the environment still has
  // tabs, because there is no invalidated-environment UI to fall back on.
  StoreResult Delete(std::string_view id, std::string* error);

  // ---- persistence -------------------------------------------------------

  // The last known persistence state. kPersisted is only ever set after a
  // CommitPendingWrite() reply, never after SetString().
  PersistState persist_state() const { return persist_state_; }

  // True when the stored blob was written by a newer binary. Every write returns
  // kReadOnly in that state.
  bool read_only() const { return read_only_; }

  // Forces the pending write out and reports completion through `done`.
  //
  // This is the ONLY way to reach PersistState::kPersisted. Exposed because
  // verification (and any future "save" button) needs a definite point at which
  // the bytes are on disk; SchedulePersist alone cannot provide one.
  void FlushForTesting(base::OnceClosure done);

  // ---- observation -------------------------------------------------------

  using Observer = base::RepeatingCallback<void(const EnvironmentChange&)>;
  base::CallbackListSubscription AddObserver(Observer observer);

  // ---- diagnostics -------------------------------------------------------

  // One-line-per-problem summary of what happened at load time. Empty when the
  // stored configuration was clean. Used by the verification scripts and by a
  // future diagnostics page.
  const std::string& load_diagnostics() const { return load_diagnostics_; }

  // Number of records dropped by validation at load time.
  int rejected_record_count() const { return rejected_record_count_; }

  // Public for the factory.
  explicit EnvironmentStore(Profile* profile);

 private:
  // Re-reads `prefs::kParraEnvironments` into memory. Called once from the
  // constructor.
  void LoadFromPrefs();

  // Writes the current in-memory definitions to the pref. Does NOT wait for the
  // disk: see PersistState.
  StoreResult PersistNow(std::string* error);

  // True when `id` is already used. Case sensitive: ids are lowercase by
  // construction.
  bool IdExists(std::string_view id) const;

  // Imports --parra-poc-environments into this store exactly once, when prefs hold
  // no usable configuration. Returns true when something was imported.
  //
  // Uses EnvironmentRegistry as a PARSER only (see docs/parra/50 §8.5); once the
  // import completes, the registry is no longer consulted for definitions in this
  // profile.
  bool ImportFromCommandLineIfPossible();

  void NotifyObservers(EnvironmentChangeKind kind, std::string id);

  raw_ptr<Profile> profile_;

  std::vector<Environment> environments_;

  PersistState persist_state_ = PersistState::kUnchanged;

  bool read_only_ = false;

  std::string load_diagnostics_;
  int rejected_record_count_ = 0;

  base::RepeatingCallbackList<void(const EnvironmentChange&)> observers_;

  // Lets the CommitPendingWrite() reply outlive a store that is being torn down
  // at profile shutdown: the write task is already on the file sequence and its
  // reply would otherwise touch freed memory.
  base::WeakPtrFactory<EnvironmentStore> weak_factory_{this};
};

}  // namespace parra

#endif  // CHROME_BROWSER_PARRA_ENVIRONMENT_STORE_H_
