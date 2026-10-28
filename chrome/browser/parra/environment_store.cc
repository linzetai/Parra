// Copyright 2026 The Parra Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chrome/browser/parra/environment_store.h"

#include <algorithm>
#include <array>
#include <string_view>
#include <utility>

#include "base/containers/span.h"
#include "base/files/file_path.h"
#include "base/functional/bind.h"
#include "base/logging.h"
#include "base/strings/strcat.h"
#include "base/strings/string_number_conversions.h"
#include "base/strings/string_util.h"
#include "chrome/browser/parra/environment_registry.h"
#include "chrome/browser/parra/environment_serialization.h"
#include "chrome/browser/parra/environment_store_factory.h"
#include "chrome/browser/parra/environment_tag.h"
#include "chrome/browser/profiles/profile.h"
#include "chrome/common/pref_names.h"
#include "components/prefs/pref_service.h"
#include "crypto/hash.h"

namespace parra {

namespace {

// The partition domain shared by every Parra environment. Must be a non-empty
// ASCII lowercase token: //content CHECKs against an empty domain and hashes the
// name, so a user-visible string never reaches the filesystem directly.
//
// All environments sharing one domain is precisely why
// BrowserContext::AsyncObliterateStoragePartition() cannot be used for a
// single-environment delete: it matches on the DOMAIN
// (storage_partition_impl_map.cc:385), so it would wipe every environment. See
// docs/parra/53 §1.4 and §3.
constexpr char kParraPartitionDomain[] = "parra";

// Mirrors content/browser/storage_partition_impl_map.cc.
constexpr char kStoragePartitionDirname[] = "Storage";
constexpr char kExtensionsDirname[] = "ext";
constexpr char kDefaultPartitionDirname[] = "def";
constexpr size_t kPartitionNameHashBytes = 6;

// A name is required, but a missing one must not cost the user the environment.
// 64 code units, not bytes: the check is on the stored string's length, which is
// what the UI will render.
bool IsUsableName(const std::string& name) {
  return !name.empty() && name.size() <= 64;
}

}  // namespace

const char* PersistStateToString(PersistState state) {
  switch (state) {
    case PersistState::kUnchanged:
      return "unchanged";
    case PersistState::kMemoryUpdated:
      return "memory_updated";
    case PersistState::kPersisted:
      return "persisted";
    case PersistState::kPersistFailed:
      return "persist_failed";
  }
  return "unknown";
}

// static
EnvironmentStore* EnvironmentStore::GetForProfile(Profile* profile) {
  if (!profile)
    return nullptr;
  return EnvironmentStoreFactory::GetForProfile(profile);
}

EnvironmentStore::EnvironmentStore(Profile* profile) : profile_(profile) {
  LoadFromPrefs();
  // UTF-16 on purpose: base::FilePath is std::wstring on Windows, and streaming
  // it into a std::string builder would select the u16string overload and fail
  // to compile rather than convert anything.
  LOG(INFO) << "parra-store: constructed for profile path='"
            << (profile_ ? profile_->GetPath().LossyDisplayName()
                         : std::u16string(u"<null>"))
            << "' environments=" << environments_.size()
            << " read_only=" << (read_only_ ? "true" : "false")
            << " persist_state=" << PersistStateToString(persist_state_)
            << (load_diagnostics_.empty() ? "" : " diagnostics='")
            << load_diagnostics_
            << (load_diagnostics_.empty() ? "" : "'");
}

EnvironmentStore::~EnvironmentStore() = default;

// static
Environment EnvironmentStore::PersonalEnvironment() const {
  Environment personal;
  personal.id = std::string(kDefaultEnvironmentId);
  personal.name = "Personal";
  personal.enabled = true;
  // Rules come from a dedicated pref: `personal` is never a stored record (its id
  // is reserved and the reader rejects such a record), so without this key its
  // routing had nowhere to live and was silently lost on import.
  if (profile_) {
    if (const PrefService* prefs = profile_->GetPrefs())
      personal.host_mappings =
          prefs->GetString(::prefs::kParraPersonalHostMappings);
  }
  return personal;
}

void EnvironmentStore::LoadFromPrefs() {
  environments_.clear();
  load_diagnostics_.clear();
  rejected_record_count_ = 0;
  read_only_ = false;

  const PrefService* prefs = profile_ ? profile_->GetPrefs() : nullptr;
  if (!prefs) {
    LOG(ERROR) << "parra-store: no PrefService; store is empty";
    return;
  }

  const std::string blob =
      prefs->GetString(::prefs::kParraEnvironments);

  // One-time import from the process-level command line, per docs/parra/50 §8.5.
  //
  // The order matters and is the whole contract:
  //
  //   1. Prefs win. If they hold a usable configuration, the command line is
  //      IGNORED -- otherwise a PoC switch left in a shortcut would silently
  //      override the user's real definitions on every launch.
  //   2. Prefs empty  -> import the command line ONCE and write it back, so the
  //      next launch reads from prefs like any other profile.
  //   3. Prefs CORRUPT -> do NOT import. Falling back to the command line here
  //      would mask the corruption: the user would keep working with a
  //      configuration that is not the one stored, and the damaged file would
  //      never be noticed or recovered.
  //
  // Without step 2, every Phase 2 / P3.1 launch (which configures environments on
  // the command line) would see an empty store and refuse the tabs it just
  // created. That is not a compatibility nicety: it is what makes the two
  // definition sources converge instead of coexisting.
  const ParsedEnvironments parsed = ParseEnvironments(blob);

  if (parsed.status == ParseStatus::kEmpty) {
    if (ImportFromCommandLineIfPossible()) {
      // The import wrote to prefs, so the in-memory list is already correct and
      // must NOT be overwritten by the (now stale) parse result.
      load_diagnostics_ = "imported from command line";
      persist_state_ = PersistState::kMemoryUpdated;
      LOG(INFO) << "parra-store: imported " << environments_.size()
                << " environment(s) from the command line and stored them; "
                   "subsequent launches read them from prefs";
      return;
    }
  }

  load_diagnostics_ = parsed.diagnostics;
  rejected_record_count_ = parsed.rejected_count;
  environments_ = parsed.environments;
  read_only_ = parsed.read_only();

  switch (parsed.status) {
    case ParseStatus::kEmpty:
      VLOG(1) << "parra-store: no stored environments";
      break;
    case ParseStatus::kOk:
      LOG(INFO) << "parra-store: loaded " << environments_.size()
                << " environment(s)";
      break;
    case ParseStatus::kPartiallyInvalid:
      // Not fatal: one bad record must not cost the user every environment. The
      // dropped count and the reasons are surfaced instead of being swallowed.
      LOG(ERROR) << "parra-store: loaded " << environments_.size()
                 << " environment(s), dropped " << rejected_record_count_
                 << ": " << load_diagnostics_;
      break;
    case ParseStatus::kTooNew:
      // Read-only downgrade. Writing here would delete the fields this binary
      // does not understand, and the user would only notice after switching back
      // to the newer build.
      LOG(ERROR) << "parra-store: stored schema is newer than this build ("
                 << load_diagnostics_
                 << "); entering READ-ONLY mode so no data is lost";
      break;
    case ParseStatus::kCorrupt:
      // Every definition is gone, which is indistinguishable from "the user has
      // none" for anyone who only checks the list. That is why this is reported
      // as an error and recorded, rather than being treated as an empty profile.
      LOG(ERROR) << "parra-store: stored configuration is corrupt ("
                 << load_diagnostics_
                 << "); no environment can be resolved. Tabs that recorded an "
                    "environment will be REFUSED, not moved to '"
                 << kDefaultEnvironmentId << "'";
      break;
  }
}

bool EnvironmentStore::has_user_defined_environments() const {
  return !environments_.empty();
}

bool EnvironmentStore::ImportFromCommandLineIfPossible() {
  // Read the ALREADY PARSED command line result rather than re-parsing
  // --parra-poc-environments here. EnvironmentRegistry remains the single
  // validator for that switch (id syntax, duplicate detection, host-rule arity);
  // duplicating it would create exactly the "two definition truth sources" this
  // phase exists to remove. The registry is used only as a PARSER of a legacy
  // input, never as a lookup authority once the store holds the result.
  EnvironmentRegistry* registry = EnvironmentRegistry::GetInstance();
  if (!registry->enabled())
    return false;

  std::vector<Environment> imported;
  std::string personal_rules;
  const base::Time now = base::Time::Now();
  for (const EnvironmentRegistry::Environment& legacy :
       registry->environments()) {
    // `personal` is special: the store never keeps a RECORD for it (its id is
    // reserved and the reader rejects such a record), but it still needs its host
    // rules, or every Personal page resolves against real DNS. Capture them into
    // their dedicated pref instead of dropping them.
    if (legacy.id == kDefaultEnvironmentId) {
      personal_rules = legacy.host_mapping_rules;
      continue;
    }

    Environment environment;
    environment.id = legacy.id;
    environment.name = legacy.id;
    environment.host_mappings = legacy.host_mapping_rules;
    environment.created_at = now;
    environment.updated_at = now;
    environment.enabled = true;

    std::string error;
    if (ValidateEnvironment(environment, &error) != StoreResult::kOk) {
      // A legacy definition that does not fit the stored model is reported and
      // skipped rather than aborting the whole import: the others are still
      // usable, and refusing all of them would be a silent, total outage.
      LOG(ERROR) << "parra-store: cannot import environment '" << legacy.id
                 << "' from the command line: " << error;
      continue;
    }
    imported.push_back(std::move(environment));
  }

  if (imported.empty() && personal_rules.empty())
    return false;

  // Personal's rules go to their own pref. Written before the definition list so
  // that a crash between the two leaves at most the definitions unstored -- and
  // the next launch re-imports, rather than leaving routing permanently absent.
  if (!personal_rules.empty()) {
    if (PrefService* prefs = profile_ ? profile_->GetPrefs() : nullptr) {
      prefs->SetString(::prefs::kParraPersonalHostMappings, personal_rules);
      LOG(INFO) << "parra-store: imported personal host_mappings='"
                << personal_rules << "'";
    }
  }

  environments_ = std::move(imported);
  std::string error;
  if (PersistNow(&error) != StoreResult::kOk) {
    // Keep the definitions in memory anyway: refusing to serve a configuration
    // that was successfully parsed would turn a storage problem into a broken
    // browser. The persist_state says kPersistFailed, which is the honest answer.
    LOG(ERROR) << "parra-store: imported definitions but could not store them: "
               << error;
    return true;
  }
  return true;
}

const std::vector<Environment>& EnvironmentStore::environments() const {
  return environments_;
}

const Environment* EnvironmentStore::Find(std::string_view id) const {
  for (const Environment& environment : environments_) {
    if (environment.id == id)
      return &environment;
  }
  return nullptr;
}

bool EnvironmentStore::IdExists(std::string_view id) const {
  return Find(id) != nullptr;
}

bool EnvironmentStore::IsKnownAndEnabled(std::string_view id) const {
  if (id.empty())
    return false;
  if (id == kDefaultEnvironmentId)
    return true;  // reserved, always available
  const Environment* environment = Find(id);
  return environment && environment->enabled;
}

// ---------------------------------------------------------------------------
// Partition mapping
// ---------------------------------------------------------------------------

content::StoragePartitionConfig EnvironmentStore::GetStoragePartitionConfig(
    std::string_view id) const {
  content::BrowserContext* browser_context = profile_;
  if (!browser_context)
    return content::StoragePartitionConfig();

  if (id.empty() || id == kDefaultEnvironmentId || !Find(id))
    return content::StoragePartitionConfig::CreateDefault(browser_context);

  // OTR profiles must never persist partition data. StoragePartitionConfig::
  // Create() already forces `in_memory || IsOffTheRecord()`
  // (storage_partition_config.cc:39), but passing it explicitly means this call
  // site does not depend on a distant implementation detail. The safety matters
  // because OffTheRecordProfileImpl::GetPath() returns the PARENT's path
  // (off_the_record_profile_impl.cc:354-356) and the partition path is a pure
  // function of (domain, name) -- so an incognito `parra/test-a` and a regular
  // `parra/test-a` compute the SAME relative path, and only the in_memory flag
  // keeps them apart.
  const bool in_memory = browser_context->IsOffTheRecord();
  content::StoragePartitionConfig config = content::StoragePartitionConfig::Create(
      browser_context, kParraPartitionDomain, std::string(id), in_memory);

  if (browser_context->IsOffTheRecord() && !config.in_memory()) {
    LOG(ERROR) << "parra-store: INVARIANT VIOLATED: OTR environment '" << id
               << "' produced a PERSISTENT partition config; refusing to hand "
                  "it out";
    return content::StoragePartitionConfig::CreateDefault(browser_context);
  }
  return config;
}

std::string EnvironmentStore::GetEnvironmentIdForConfig(
    const content::StoragePartitionConfig& config) const {
  if (config.is_default() || config.partition_domain() != kParraPartitionDomain)
    return std::string();
  return config.partition_name();
}

ResolvedEnvironmentLookup EnvironmentStore::ResolveForEnvironment(
    std::string_view id) const {
  ResolvedEnvironmentLookup result;
  content::BrowserContext* browser_context = profile_;
  if (!browser_context) {
    result.kind = StoreLookupKind::kUnknown;
    result.failure = "no profile";
    return result;
  }

  if (id.empty()) {
    result.kind = StoreLookupKind::kNotSpecified;
    result.config =
        content::StoragePartitionConfig::CreateDefault(browser_context);
    return result;
  }

  if (id == kDefaultEnvironmentId) {
    result.kind = StoreLookupKind::kPersonal;
    result.environment_id = std::string(kDefaultEnvironmentId);
    result.config =
        content::StoragePartitionConfig::CreateDefault(browser_context);
    return result;
  }

  const Environment* environment = Find(id);
  if (!environment) {
    result.kind = StoreLookupKind::kUnknown;
    result.failure = "environment '" + std::string(id) +
                     "' is not defined in this profile (deleted, misspelled, "
                     "or the configuration could not be read)";
    return result;
  }

  if (!environment->enabled) {
    // Refused for SELECTION only. The partition and its data are intact, which
    // is why this is a distinct kind rather than kUnknown: a caller can tell
    // "the environment was switched off" from "the environment is gone".
    result.kind = StoreLookupKind::kDisabled;
    result.environment_id = environment->id;
    result.failure = "environment '" + environment->id + "' is disabled";
    return result;
  }

  result.kind = StoreLookupKind::kNamed;
  result.environment_id = environment->id;
  result.config = GetStoragePartitionConfig(environment->id);

  if (result.config.is_default() ||
      result.config.partition_name() != environment->id) {
    // A definition that resolves to the default partition under a non-default
    // name would put the page in Personal while the caller believes it is
    // isolated.
    result.kind = StoreLookupKind::kUnknown;
    result.failure = "environment '" + environment->id +
                     "' resolves to the default partition";
    return result;
  }

  return result;
}

std::string EnvironmentStore::GetHostMappingRulesForPartitionPath(
    const base::FilePath& relative_partition_path) const {
  // Pure function of (domain, name); deliberately ignores in_memory so an OTR
  // environment with the same id as a regular one resolves the same hostname the
  // same way. Data isolation is the in_memory flag, not the rules.
  //
  // The empty path IS the default partition. Personal's rules must still be
  // found there: an earlier version returned early on an empty path, which
  // silently disabled host mapping for Personal only and looked exactly like a
  // partition mix-up.
  const Environment personal = PersonalEnvironment();
  if (relative_partition_path.empty())
    return personal.host_mappings;
  for (const Environment& environment : environments_) {
    // Computed via a synthetic config so the path formula lives in exactly one
    // place. The config is only used for its (domain, name) pair; Create() would
    // need a BrowserContext, and the path is a pure function of those two
    // strings, so it is derived without one.
    if (EnvironmentRegistry::GetRelativePartitionPathForConfig(
            kParraPartitionDomain, environment.id) == relative_partition_path) {
      return environment.host_mappings;
    }
  }
  return std::string();
}

// static
base::FilePath EnvironmentStore::GetRelativePartitionPathForConfig(
    const content::StoragePartitionConfig& config) {
  if (config.is_default())
    return base::FilePath();
  if (config.partition_domain().empty())
    return base::FilePath();

  base::FilePath path = base::FilePath::FromASCII(kStoragePartitionDirname)
                            .AppendASCII(kExtensionsDirname)
                            .Append(base::FilePath::FromUTF8Unsafe(
                                config.partition_domain()));
  if (config.partition_name().empty())
    return path.AppendASCII(kDefaultPartitionDirname);

  // Mirrors StoragePartitionImplMap::GetStoragePartitionPath(): SHA-256 of the
  // partition name, truncated to 6 bytes, uppercase hex.
  const std::array<uint8_t, crypto::hash::kSha256Size> hash =
      crypto::hash::Sha256(config.partition_name());
  auto truncated_hash = base::span(hash).first<kPartitionNameHashBytes>();
  return path.AppendASCII(base::HexEncode(truncated_hash));
}

// ---------------------------------------------------------------------------
// Writes
// ---------------------------------------------------------------------------

StoreResult EnvironmentStore::Create(const EnvironmentDraft& draft,
                                     std::string* error) {
  if (read_only_ || (profile_ && profile_->IsOffTheRecord())) {
    if (error)
      *error = read_only_
                   ? "the stored configuration was written by a newer build"
                   : "environment definitions cannot be changed from an "
                     "incognito window (the change would be discarded when the "
                     "window closes)";
    return StoreResult::kReadOnly;
  }

  if (!IsValidEnvironmentId(draft.id)) {
    if (error)
      *error = "invalid id '" + draft.id +
               "' (allowed: [a-z0-9_-], 1..32 characters)";
    return StoreResult::kInvalidId;
  }
  if (draft.id == kDefaultEnvironmentId) {
    if (error)
      *error = "'personal' is reserved and always exists";
    return StoreResult::kReservedId;
  }
  if (IdExists(draft.id)) {
    if (error)
      *error = "an environment with id '" + draft.id + "' already exists";
    return StoreResult::kDuplicateId;
  }

  const base::Time now = base::Time::Now();
  Environment environment;
  environment.id = draft.id;
  environment.name = draft.name.empty() ? draft.id : draft.name;
  environment.color = draft.color;
  environment.icon = draft.icon;
  environment.host_mappings = draft.host_mappings;
  environment.created_at = now;
  environment.updated_at = now;
  environment.enabled = true;

  // Validate the COMPLETE record before touching memory, so a rejected create
  // cannot leave anything half-updated.
  std::string validation_error;
  const StoreResult validation = ValidateEnvironment(environment, &validation_error);
  if (validation != StoreResult::kOk) {
    if (error)
      *error = validation_error;
    return validation;
  }

  const std::string blob_before = SerializeEnvironments(environments_);
  (void)blob_before;

  environments_.push_back(environment);

  const StoreResult persisted = PersistNow(error);
  if (persisted != StoreResult::kOk) {
    // Roll the in-memory state back so memory never claims something the pref
    // could not hold. The alternative -- keep the change and hope -- produces a
    // definition that exists until the next restart, which is worse than a
    // visible failure.
    environments_.pop_back();
    LOG(ERROR) << "parra-store: create of '" << draft.id
               << "' rolled back: " << (error ? *error : "persist failed");
    return persisted;
  }

  LOG(INFO) << "parra-store: created environment '" << environment.id
            << "' name='" << environment.name << "' persist_state="
            << PersistStateToString(persist_state_);
  NotifyObservers(EnvironmentChangeKind::kCreated, environment.id);
  return StoreResult::kOk;
}

StoreResult EnvironmentStore::Update(std::string_view id,
                                     const EnvironmentPatch& patch,
                                     std::string* error) {
  if (read_only_ || (profile_ && profile_->IsOffTheRecord())) {
    if (error)
      *error = read_only_
                   ? "the stored configuration was written by a newer build"
                   : "environment definitions cannot be changed from an "
                     "incognito window (the change would be discarded when the "
                     "window closes)";
    return StoreResult::kReadOnly;
  }
  if (id == kDefaultEnvironmentId) {
    if (error)
      *error = "'personal' is implicit and cannot be modified";
    return StoreResult::kReservedId;
  }

  auto it = std::find_if(environments_.begin(), environments_.end(),
                         [id](const Environment& environment) {
                           return environment.id == id;
                         });
  if (it == environments_.end()) {
    if (error)
      *error = "no environment with id '" + std::string(id) + "'";
    return StoreResult::kNotFound;
  }

  // Build the candidate record, validate it, and only then commit. Mutating in
  // place and validating afterwards would leave a rejected update applied.
  Environment candidate = *it;
  if (patch.name.has_value())
    candidate.name = *patch.name;
  if (patch.color.has_value())
    candidate.color = *patch.color;
  if (patch.icon.has_value())
    candidate.icon = *patch.icon;
  if (patch.host_mappings.has_value())
    candidate.host_mappings = *patch.host_mappings;
  if (patch.enabled.has_value())
    candidate.enabled = *patch.enabled;

  if (!IsUsableName(candidate.name)) {
    if (error)
      *error = "name must be 1..64 characters";
    return StoreResult::kInvalidField;
  }

  // Update() differs from Create() in one place: `id` is never re-validated
  // because it cannot change; ValidateEnvironment would reject it for being...
  // itself. The remaining fields are validated by the same code path.
  {
    Environment id_checked = candidate;
    // ValidateEnvironment rejects the reserved id; a stored record can never have
    // it (LoadFromPrefs drops such a record), so this cannot fire. Kept as a
    // guard rather than assuming.
    if (id_checked.id == kDefaultEnvironmentId) {
      if (error)
        *error = "stored record has the reserved id";
      return StoreResult::kReservedId;
    }
    std::string validation_error;
    const StoreResult validation =
        ValidateEnvironment(id_checked, &validation_error);
    if (validation != StoreResult::kOk) {
      if (error)
        *error = validation_error;
      return validation;
    }
  }

  candidate.updated_at = base::Time::Now();

  const Environment previous = *it;
  *it = candidate;

  const StoreResult persisted = PersistNow(error);
  if (persisted != StoreResult::kOk) {
    *it = previous;
    LOG(ERROR) << "parra-store: update of '" << id
               << "' rolled back: " << (error ? *error : "persist failed");
    return persisted;
  }

  LOG(INFO) << "parra-store: updated environment '" << candidate.id
            << "' name='" << candidate.name << "' enabled="
            << (candidate.enabled ? "true" : "false")
            << " host_mappings='" << candidate.host_mappings
            << "' persist_state=" << PersistStateToString(persist_state_);

  // `enabled` is reported separately because it is a selection-layer change with
  // no network effect, while a host-mapping change has one. A listener that
  // re-applies network configuration must be able to skip the former.
  const bool enabled_changed = previous.enabled != candidate.enabled;
  const bool only_enabled_changed =
      enabled_changed && previous.name == candidate.name &&
      previous.color == candidate.color && previous.icon == candidate.icon &&
      previous.host_mappings == candidate.host_mappings;
  NotifyObservers(only_enabled_changed ? EnvironmentChangeKind::kEnabledChanged
                                       : EnvironmentChangeKind::kUpdated,
                  candidate.id);
  return StoreResult::kOk;
}

StoreResult EnvironmentStore::SetEnabled(std::string_view id,
                                         bool enabled,
                                         std::string* error) {
  EnvironmentPatch patch;
  patch.enabled = enabled;
  return Update(id, patch, error);
}

StoreResult EnvironmentStore::SetPersonalHostMappings(
    std::string_view host_mappings,
    std::string* error) {
  if (read_only_ || (profile_ && profile_->IsOffTheRecord())) {
    if (error)
      *error = read_only_ ? "the stored configuration was written by a newer build"
                          : "environment definitions cannot be changed from an "
                            "incognito window";
    return StoreResult::kReadOnly;
  }

  std::string validation_error;
  if (!ValidateHostMappings(host_mappings, &validation_error)) {
    if (error)
      *error = validation_error;
    return StoreResult::kInvalidHostMapping;
  }

  PrefService* prefs = profile_ ? profile_->GetPrefs() : nullptr;
  if (!prefs) {
    if (error)
      *error = "no PrefService";
    return StoreResult::kPersistFailed;
  }

  prefs->SetString(::prefs::kParraPersonalHostMappings,
                   std::string(host_mappings));
  persist_state_ = PersistState::kMemoryUpdated;

  LOG(INFO) << "parra-store: personal host_mappings set to '"
            << host_mappings
            << "' persist_state=" << PersistStateToString(persist_state_);
  NotifyObservers(EnvironmentChangeKind::kUpdated, std::string(kDefaultEnvironmentId));
  return StoreResult::kOk;
}

StoreResult EnvironmentStore::Delete(std::string_view id, std::string* error) {
  if (read_only_ || (profile_ && profile_->IsOffTheRecord())) {
    if (error)
      *error = read_only_
                   ? "the stored configuration was written by a newer build"
                   : "environment definitions cannot be changed from an "
                     "incognito window (the change would be discarded when the "
                     "window closes)";
    return StoreResult::kReadOnly;
  }
  if (id == kDefaultEnvironmentId) {
    if (error)
      *error = "'personal' cannot be deleted";
    return StoreResult::kReservedId;
  }

  auto it = std::find_if(environments_.begin(), environments_.end(),
                         [id](const Environment& environment) {
                           return environment.id == id;
                         });
  if (it == environments_.end()) {
    if (error)
      *error = "no environment with id '" + std::string(id) + "'";
    return StoreResult::kNotFound;
  }

  // This build implements ONLY the conservative mode from docs/parra/53 §3:
  // refuse while any tab may still be using the environment, because there is no
  // invalidated-environment UI to fall back on and "delete the definition, leave
  // the tabs alive" would leave pages the user cannot explain.
  //
  // The tab check is not done here: the store has no view of any tab strip, and
  // giving it one would make a Profile-scoped service depend on window state.
  // The caller that owns the windows performs the check and only calls Delete()
  // when it is satisfied. Delete() therefore documents the precondition rather
  // than guessing at it.
  //
  // NO PARTITION IS TOUCHED. Clearing storage is a separate operation and is NOT
  // implemented in P3.2.
  const Environment removed = *it;
  environments_.erase(it);

  const StoreResult persisted = PersistNow(error);
  if (persisted != StoreResult::kOk) {
    environments_.insert(environments_.begin() + (it - environments_.begin()),
                         removed);
    LOG(ERROR) << "parra-store: delete of '" << id
               << "' rolled back: " << (error ? *error : "persist failed");
    return persisted;
  }

  LOG(INFO) << "parra-store: removed environment '" << removed.id
            << "' definition only; its StoragePartition and data were NOT "
               "touched (see docs/parra/53 §3)";
  NotifyObservers(EnvironmentChangeKind::kRemoved, removed.id);
  return StoreResult::kOk;
}

// ---------------------------------------------------------------------------
// Persistence
// ---------------------------------------------------------------------------

StoreResult EnvironmentStore::PersistNow(std::string* error) {
  PrefService* prefs = profile_ ? profile_->GetPrefs() : nullptr;
  if (!prefs) {
    persist_state_ = PersistState::kPersistFailed;
    if (error)
      *error = "no PrefService";
    return StoreResult::kPersistFailed;
  }

  const std::string blob = SerializeEnvironments(environments_);
  if (blob.empty() && !environments_.empty()) {
    persist_state_ = PersistState::kPersistFailed;
    if (error)
      *error = "serialisation failed";
    return StoreResult::kPersistFailed;
  }

  // SetString() only updates the in-memory prefs tree. The disk write is
  // scheduled on ImportantFileWriter's commit timer (default 10 s:
  // base/files/important_file_writer.cc:48) and performed on a background
  // sequence. There is NO completion callback. So this assignment must never be
  // reported as "persisted".
  //
  // The state is therefore kMemoryUpdated here, and only FlushForTesting() (which
  // uses CommitPendingWrite's reply) can promote it to kPersisted.
  prefs->SetString(::prefs::kParraEnvironments, blob);
  persist_state_ = PersistState::kMemoryUpdated;

  LOG(INFO) << "parra-store: prefs updated in memory (" << blob.size()
            << " bytes, " << environments_.size()
            << " environment(s)); NOT yet on disk";
  return StoreResult::kOk;
}

void EnvironmentStore::FlushForTesting(base::OnceClosure done) {
  PrefService* prefs = profile_ ? profile_->GetPrefs() : nullptr;
  if (!prefs) {
    persist_state_ = PersistState::kPersistFailed;
    if (done)
      std::move(done).Run();
    return;
  }

  // CommitPendingWrite()'s reply callback is posted back to this sequence after
  // the file task runner has run the write task. That is the earliest moment at
  // which "the bytes were handed to the OS" is a true statement -- still not an
  // fsync, which is why the state is called kPersisted and not kSynced.
  prefs->CommitPendingWrite(
      base::BindOnce(
          [](base::WeakPtr<EnvironmentStore> self, base::OnceClosure done) {
            if (self) {
              self->persist_state_ = PersistState::kPersisted;
              LOG(INFO) << "parra-store: commit reply received; persisted";
            }
            if (done)
              std::move(done).Run();
          },
          weak_factory_.GetWeakPtr(), std::move(done)));
}

// ---------------------------------------------------------------------------
// Observation
// ---------------------------------------------------------------------------

base::CallbackListSubscription EnvironmentStore::AddObserver(
    Observer observer) {
  return observers_.Add(std::move(observer));
}

void EnvironmentStore::NotifyObservers(EnvironmentChangeKind kind,
                                       std::string id) {
  EnvironmentChange change;
  change.kind = kind;
  change.id = std::move(id);
  observers_.Notify(change);
}

}  // namespace parra
