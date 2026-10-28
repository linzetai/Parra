// Copyright 2026 The Parra Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CHROME_BROWSER_PARRA_ENVIRONMENT_H_
#define CHROME_BROWSER_PARRA_ENVIRONMENT_H_

#include <optional>
#include <string>

#include "base/time/time.h"
#include "content/public/browser/storage_partition_config.h"

namespace parra {

// One environment definition, as a value.
//
// Deliberately free of any Chromium-dependency beyond base::Time: this is the
// type that gets serialised, validated and compared, so it must be possible to
// reason about it (and test it) without a browser, a profile or a
// StoragePartition in the picture.
//
// INVARIANT: `id` is also the StoragePartition `partition_name`. See
// docs/parra/49 §3.2 for why that must never be broken:
//   * EnvironmentRegistry::GetStoragePartitionConfig() uses the id directly;
//   * GetEnvironmentIdForConfig() reverse-maps partition_name -> id;
//   * EnvironmentTabRouter asserts "tag == actual partition".
// Introducing a separate partition_name would need a bidirectional map, and any
// disagreement in that map produces exactly the "tag says A, partition is B"
// state Phase 2 spent its whole budget eliminating.
//
// Therefore `id` is IMMUTABLE. A user-visible rename changes `name` only.
struct Environment {
  // Stable identifier. Also the StoragePartitionConfig partition_name.
  // Validated against ^[a-z0-9_-]{1,32}$ on the way in.
  std::string id;

  // User-visible label. May be changed; may not be empty; not required unique.
  std::string name;

  // "#RRGGBB" or empty for "no colour set". A string rather than SkColor so the
  // low layer does not gain a //ui or //third_party/skia dependency.
  std::string color;

  // A fixed identifier from a known set, or empty. Never a path, never a URL:
  // an arbitrary string here would be interpreted by a future UI as a resource
  // location.
  std::string icon;

  // net::HostMappingRules syntax, comma separated between rules, e.g.
  //   "map sheet.test 127.0.0.1:8443, exclude localhost"
  //
  // Stored exactly as the NetworkContext will consume it so there is no
  // translation layer that could disagree with what actually takes effect.
  // Empty means "no remapping". See docs/parra/50 §3.3.
  std::string host_mappings;

  // When the environment was created. Written once.
  base::Time created_at;

  // When any field last changed. Refreshed on every successful write.
  base::Time updated_at;

  // Whether the environment may be chosen for activation or for a new tab.
  //
  // This is a SELECTION-LAYER switch only. It must NEVER change the
  // StoragePartition, the host mappings or the storage location of an existing
  // tab: see docs/parra/49 §3.4. A boolean a user can toggle at any moment must
  // not be able to re-route live network traffic.
  bool enabled = true;
};

// The fields a caller may supply when creating an environment. Separate from
// Environment so that timestamps have exactly one writer (the store) and so that
// `id` cannot be supplied on update.
struct EnvironmentDraft {
  std::string id;
  std::string name;
  std::string color;
  std::string icon;
  std::string host_mappings;
};

// A partial update. Every field is optional, and "present but empty" is
// distinguishable from "not provided" -- which is why these are std::optional
// rather than empty strings standing in for absence.
//
// `id` is deliberately absent: renaming an environment is not expressible, so it
// cannot be done by accident or by a future caller that did not read the header.
struct EnvironmentPatch {
  std::optional<std::string> name;
  std::optional<std::string> color;
  std::optional<std::string> icon;
  std::optional<std::string> host_mappings;
  std::optional<bool> enabled;
};

// The outcome of a store write.
//
// Not a bool: a caller has to distinguish "you asked for something invalid" from
// "that environment does not exist" from "this profile is read-only", because the
// user-facing consequence and the log line differ for each. Collapsing them into
// false is how a rejection becomes indistinguishable from a no-op.
enum class StoreResult {
  kOk,
  // The id does not match the allowed pattern.
  kInvalidId,
  // The id is already in use.
  kDuplicateId,
  // No environment with that id exists in this profile.
  kNotFound,
  // The id names a reserved environment (currently only `personal`).
  kReservedId,
  // A field failed validation (name length, colour syntax, icon set).
  kInvalidField,
  // host_mappings could not be parsed by net::HostMappingRules.
  kInvalidHostMapping,
  // The stored configuration is newer than this binary understands, so nothing
  // may be written back.
  kReadOnly,
  // The write could not be serialised or handed to the prefs layer.
  kPersistFailed,
  // Only relevant for Delete: the environment still has tabs, and this build does
  // not implement the invalidated-environment UI. See docs/parra/53 §3.
  kRefusedTabsOpen,
};

// Human-readable form of `result`, for logs. Stable strings: the verification
// scripts grep for them.
const char* StoreResultToString(StoreResult result);

// Why a store lookup answered the way it did.
//
// Named distinctly from EnvironmentResolution (parra_storage_partition_util.h),
// which classifies a SESSION-RESTORE outcome and has a different set of values.
// Two enums with the same name in one namespace would be an ODR conflict, and
// merging them would force one caller to handle the other's states.
enum class StoreLookupKind {
  // The caller did not name an environment (empty id). The ONLY case that may use
  // the default partition, and the only case a stock Chromium navigation hits.
  kNotSpecified,
  // The reserved `personal` environment: always available, default partition.
  kPersonal,
  // A stored, enabled environment with its own partition.
  kNamed,
  // A stored environment that is currently disabled. It HAS a partition and its
  // existing tabs keep running in it, but it may not be chosen.
  kDisabled,
  // Non-empty id with no stored definition.
  kUnknown,
};

// The result of resolving an environment id through the store.
//
// `ok()` is the only thing a caller may use to decide whether to proceed. A
// refusal deliberately leaves `config` at its default value so that a caller which
// checks only `config` (instead of `ok()`) cannot accidentally continue into the
// default partition.
struct ResolvedEnvironmentLookup {
  bool ok() const { return failure.empty(); }

  StoreLookupKind kind = StoreLookupKind::kNotSpecified;

  // The environment this refers to: "personal" for kPersonal, the id for
  // kNamed/kDisabled, empty for kNotSpecified and kUnknown.
  std::string environment_id;

  // Meaningful only when ok().
  content::StoragePartitionConfig config;

  // Non-empty means the caller MUST refuse. Never set for kNotSpecified.
  std::string failure;
};

}  // namespace parra

#endif  // CHROME_BROWSER_PARRA_ENVIRONMENT_H_
