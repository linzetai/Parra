// Copyright 2026 The Parra Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CHROME_BROWSER_PARRA_ENVIRONMENT_SERIALIZATION_H_
#define CHROME_BROWSER_PARRA_ENVIRONMENT_SERIALIZATION_H_

#include <string>
#include <vector>

#include "chrome/browser/parra/environment.h"
#include "chrome/browser/parra/host_mapping_compiler.h"

namespace parra {

// Current on-disk schema. Bump when a change is not backward compatible; see
// docs/parra/50 §8.1 for the rules that decide whether a bump is required.
inline constexpr int kEnvironmentSchemaVersion = 2;

// How a stored blob was interpreted.
enum class ParseStatus {
  // Parsed, schema version understood, every record valid.
  kOk,
  // The pref was absent or empty. Not an error: a fresh profile.
  kEmpty,
  // The blob is not valid JSON, or its top level is not an object.
  kCorrupt,
  // Valid JSON but written by a NEWER binary. The known fields may be read, but
  // nothing may be written back: writing would silently drop the fields this
  // binary does not understand.
  kTooNew,
  // Valid JSON, version understood, but SOME records failed validation. The valid
  // ones are returned; see `rejected_count` and `diagnostics`. Deliberately not a
  // total failure: a single bad record must not cost the user every environment.
  kPartiallyInvalid,
};

const char* ParseStatusToString(ParseStatus status);

struct ParsedEnvironments {
  ParseStatus status = ParseStatus::kEmpty;

  // The records that survived validation, in stored order. Order is preserved
  // because it is the display order and there is no other ordering field.
  std::vector<Environment> environments;

  // Number of records dropped by validation. Non-zero only for
  // kPartiallyInvalid.
  int rejected_count = 0;

  // Human-readable reason(s), one per line, for the log and for diagnostics.
  std::string diagnostics;

  // True when the store must refuse to write. Set for kTooNew.
  bool read_only() const { return status == ParseStatus::kTooNew; }

  // True when the caller may treat the returned records as authoritative.
  bool usable() const {
    return status == ParseStatus::kOk ||
           status == ParseStatus::kPartiallyInvalid ||
           status == ParseStatus::kEmpty;
  }
};

// Serialises `environments` to the canonical JSON form.
//
// Deterministic: the same input always produces the same string, keys are
// emitted in a fixed order, and absent optional fields are omitted rather than
// written as empty strings. Determinism matters because the value is stored in a
// pref, and a value that churns on every write would make "did anything change?"
// unanswerable.
std::string SerializeEnvironments(const std::vector<Environment>& environments);

// Parses a stored blob.
//
// Unknown top-level and per-record fields are PRESERVED in `unknown_fields` so a
// later write cannot silently drop a newer binary's data. (This is what makes the
// read-only downgrade honest: the fields survive a round trip even if this binary
// does not act on them.)
ParsedEnvironments ParseEnvironments(const std::string& json);

// Validates a single record. Returns StoreResult::kOk when it may be stored.
//
// This is the SAME validation used on the write path and on the read path, so a
// value that was accepted once cannot become invalid on the next load for a
// different reason.
StoreResult ValidateEnvironment(const Environment& environment,
                                std::string* error);

// Validates an id against the allowed pattern. Public because Create() needs it
// before an Environment exists.
bool IsValidEnvironmentId(std::string_view id);

// Validates a host-mapping string by feeding it to the SAME parser the network
// stack uses (net::HostMappingRules). Using a hand-written grammar check would
// accept strings that net silently drops, which turns "saved" into "saved but
// ignored".
//
// `error` receives the offending rule text when validation fails.
bool ValidateHostMappings(std::string_view host_mappings, std::string* error);

// Validates a source string in its explicitly selected grammar.
bool ValidateHostMappings(HostMappingFormat format,
                          std::string_view host_mappings,
                          std::string* error);

// The reserved environment that maps to the default StoragePartition. Declared in
// environment_tag.h; re-exported here so callers that only include this header do
// not have to know that.
constexpr std::string_view kPersonalEnvironmentIdValue = "personal";

}  // namespace parra

#endif  // CHROME_BROWSER_PARRA_ENVIRONMENT_SERIALIZATION_H_
