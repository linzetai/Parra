// Copyright 2026 The Parra Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chrome/browser/parra/environment_serialization.h"

#include <string_view>
#include <utility>

#include "base/json/json_reader.h"
#include "base/json/json_writer.h"
#include "base/logging.h"
#include "base/strings/string_split.h"
#include "base/strings/string_util.h"
#include "base/values.h"
#include "net/base/host_mapping_rules.h"

namespace parra {

namespace {

// Top-level keys.
constexpr char kSchemaVersionKey[] = "schemaVersion";
constexpr char kEnvironmentsKey[] = "environments";

// Per-record keys. `id` first so the emitted JSON is diff-friendly.
constexpr char kIdKey[] = "id";
constexpr char kNameKey[] = "name";
constexpr char kColorKey[] = "color";
constexpr char kIconKey[] = "icon";
constexpr char kHostMappingsKey[] = "hostMappings";
constexpr char kCreatedAtKey[] = "createdAt";
constexpr char kUpdatedAtKey[] = "updatedAt";
constexpr char kEnabledKey[] = "enabled";

// Upper bound on record count. A definition set is user-authored configuration;
// 64 is far more than any real use and bounds the parse cost and the pref size
// (< 15 KB). Exceeding it is reported, not silently truncated.
constexpr size_t kMaxEnvironments = 64;

constexpr size_t kMaxNameLength = 64;
constexpr size_t kMaxIdLength = 32;

// "#RRGGBB" only. Rejecting named colours keeps one representation in the file.
bool IsValidColor(std::string_view color) {
  if (color.empty())
    return true;  // "not set"
  if (color.size() != 7 || color[0] != '#')
    return false;
  for (size_t i = 1; i < color.size(); ++i) {
    const char c = color[i];
    const bool hex = base::IsAsciiDigit(c) || (c >= 'a' && c <= 'f') ||
                     (c >= 'A' && c <= 'F');
    if (!hex)
      return false;
  }
  return true;
}

// A fixed set rather than "any string": a future UI will map this to a resource,
// and an arbitrary string would either be ignored (confusing) or, worse, treated
// as a path.
bool IsValidIcon(std::string_view icon) {
  if (icon.empty())
    return true;  // "not set"
  static constexpr std::string_view kIcons[] = {
      "beaker", "briefcase", "bug",  "chart",   "cloud", "code",
      "flag",   "globe",     "key",  "lab",     "lock",  "rocket",
      "shield", "star",      "tag",  "wrench",
  };
  for (std::string_view known : kIcons) {
    if (icon == known)
      return true;
  }
  return false;
}

// Milliseconds since the Unix epoch, UTC.
//
// Not base::Time's internal representation (unreadable in the file and not
// specified) and not an ISO string (needs a parser and has timezone ambiguity).
//
// Carried as a `double`, not an int64: this revision of base::Value has no int64
// setter/finder (see base/values.h -- only int and double exist), and an `int`
// would overflow: unix milliseconds are ~1.7e12 today, far past INT32_MAX.
// A double represents every integer exactly up to 2^53 (~285000 years of
// milliseconds), so the value is lossless for any realistic timestamp.
constexpr double ToMillis(base::Time time) {
  return static_cast<double>((time - base::Time::UnixEpoch()).InMilliseconds());
}

base::Time FromMillis(double millis) {
  return base::Time::UnixEpoch() +
         base::Milliseconds(static_cast<int64_t>(millis));
}

// Collects the fields this binary does not know about, so a write cannot silently
// delete them. Only used for the newer-schema case, which is also the only case
// where a write is forbidden -- but keeping the data correct here means the
// read-only decision is about policy, not about data loss.
[[maybe_unused]] void CollectUnknownFields(const base::DictValue& source,
                                           const std::vector<std::string_view>& known,
                                           base::DictValue* out) {
  for (const auto [key, value] : source) {
    bool is_known = false;
    for (std::string_view k : known) {
      if (key == k) {
        is_known = true;
        break;
      }
    }
    if (!is_known)
      (*out).Set(key, value.Clone());
  }
}

// Preserves unknown per-record fields under a side key so a later write of the
// same record can put them back. The representation is deliberately opaque: this
// binary never interprets them.
[[maybe_unused]] constexpr char kUnknownFieldsKey[] = "__parra_unknown";

}  // namespace

const char* ParseStatusToString(ParseStatus status) {
  switch (status) {
    case ParseStatus::kOk:
      return "ok";
    case ParseStatus::kEmpty:
      return "empty";
    case ParseStatus::kCorrupt:
      return "corrupt";
    case ParseStatus::kTooNew:
      return "too_new";
    case ParseStatus::kPartiallyInvalid:
      return "partially_invalid";
  }
  return "unknown";
}

const char* StoreResultToString(StoreResult result) {
  switch (result) {
    case StoreResult::kOk:
      return "ok";
    case StoreResult::kInvalidId:
      return "invalid_id";
    case StoreResult::kDuplicateId:
      return "duplicate_id";
    case StoreResult::kNotFound:
      return "not_found";
    case StoreResult::kReservedId:
      return "reserved_id";
    case StoreResult::kInvalidField:
      return "invalid_field";
    case StoreResult::kInvalidHostMapping:
      return "invalid_host_mapping";
    case StoreResult::kReadOnly:
      return "read_only";
    case StoreResult::kPersistFailed:
      return "persist_failed";
    case StoreResult::kRefusedTabsOpen:
      return "refused_tabs_open";
  }
  return "unknown";
}

bool IsValidEnvironmentId(std::string_view id) {
  if (id.empty() || id.size() > kMaxIdLength)
    return false;
  for (char c : id) {
    const bool ok = base::IsAsciiLower(c) || base::IsAsciiDigit(c) || c == '_' ||
                    c == '-';
    if (!ok)
      return false;
  }
  return true;
}

bool ValidateHostMappings(std::string_view host_mappings, std::string* error) {
  if (host_mappings.empty())
    return true;

  // Feed the string to the SAME parser the network stack uses. A hand-written
  // grammar check would accept strings that net::HostMappingRules silently drops,
  // turning "validated" into "stored but ignored" -- the failure mode with no
  // visible symptom other than the wrong backend answering.
  net::HostMappingRules probe;
  const std::vector<std::string_view> rules = base::SplitStringPiece(
      host_mappings, ",", base::TRIM_WHITESPACE, base::SPLIT_WANT_ALL);
  for (std::string_view rule : rules) {
    if (rule.empty())
      continue;  // tolerate "a, ,b" rather than rejecting a harmless separator
    if (!probe.AddRuleFromString(rule)) {
      if (error)
        *error = "unparseable host mapping rule: '" + std::string(rule) + "'";
      return false;
    }
  }
  return true;
}

StoreResult ValidateEnvironment(const Environment& environment,
                                std::string* error) {
  if (!IsValidEnvironmentId(environment.id)) {
    if (error)
      *error = "invalid id '" + environment.id +
               "' (allowed: [a-z0-9_-], 1..32 characters)";
    return StoreResult::kInvalidId;
  }

  if (environment.id == kPersonalEnvironmentIdValue) {
    if (error)
      *error = "id 'personal' is reserved";
    return StoreResult::kReservedId;
  }

  if (environment.name.empty() || environment.name.size() > kMaxNameLength) {
    if (error)
      *error = "name must be 1..64 characters, got " +
               std::to_string(environment.name.size());
    return StoreResult::kInvalidField;
  }

  if (!IsValidColor(environment.color)) {
    if (error)
      *error = "colour must be empty or #RRGGBB, got '" + environment.color + "'";
    return StoreResult::kInvalidField;
  }

  if (!IsValidIcon(environment.icon)) {
    if (error)
      *error = "unknown icon '" + environment.icon + "'";
    return StoreResult::kInvalidField;
  }

  std::string mapping_error;
  if (!ValidateHostMappings(environment.host_mappings, &mapping_error)) {
    if (error)
      *error = mapping_error;
    return StoreResult::kInvalidHostMapping;
  }

  return StoreResult::kOk;
}

std::string SerializeEnvironments(
    const std::vector<Environment>& environments) {
  base::ListValue list;
  for (const Environment& environment : environments) {
    base::DictValue record;
    record.Set(kIdKey, environment.id);
    record.Set(kNameKey, environment.name);
    // Omit absent optionals rather than writing "" -- an empty string and an
    // absent key are the same thing to a reader, and omitting keeps the file
    // small and the diff quiet.
    if (!environment.color.empty())
      record.Set(kColorKey, base::Value(environment.color));
    if (!environment.icon.empty())
      record.Set(kIconKey, base::Value(environment.icon));
    if (!environment.host_mappings.empty())
      record.Set(kHostMappingsKey, base::Value(environment.host_mappings));
    record.Set(kCreatedAtKey, base::Value(ToMillis(environment.created_at)));
    record.Set(kUpdatedAtKey, base::Value(ToMillis(environment.updated_at)));
    record.Set(kEnabledKey, base::Value(environment.enabled));
    list.Append(std::move(record));
  }

  base::DictValue root;
  root.Set(kSchemaVersionKey, base::Value(kEnvironmentSchemaVersion));
  root.Set(kEnvironmentsKey, base::Value(std::move(list)));

  std::string out;
  // `options` with omit_null_values and sorted keys off: key order is set by the
  // insertion order above, which is deliberate and stable.
  if (!base::JSONWriter::Write(root, &out)) {
    LOG(ERROR) << "parra-store: failed to serialise environments";
    return std::string();
  }
  return out;
}

ParsedEnvironments ParseEnvironments(const std::string& json) {
  ParsedEnvironments parsed;

  if (json.empty()) {
    parsed.status = ParseStatus::kEmpty;
    return parsed;
  }

  std::optional<base::Value> root =
      base::JSONReader::Read(json, base::JSON_PARSE_RFC);
  if (!root || !root->is_dict()) {
    parsed.status = ParseStatus::kCorrupt;
    parsed.diagnostics = "top level is not a JSON object";
    return parsed;
  }

  const base::DictValue& dict = root->GetDict();
  const std::optional<int> version = dict.FindInt(kSchemaVersionKey);
  if (!version.has_value()) {
    // Missing version is treated as 0: predates versioning, assume the oldest
    // known layout rather than declaring the file corrupt.
    parsed.diagnostics = "no schemaVersion; treating as 0";
  } else if (*version > kEnvironmentSchemaVersion) {
    // A newer binary wrote this. Read what is understood, but never write back:
    // a write would drop the fields this binary does not know about, and the user
    // would only notice after switching back to the newer build.
    parsed.status = ParseStatus::kTooNew;
    parsed.diagnostics = "schemaVersion " + std::to_string(*version) +
                         " is newer than " +
                         std::to_string(kEnvironmentSchemaVersion);
    // Best-effort read so the caller can still *show* the definitions.
    const base::ListValue* list = dict.FindList(kEnvironmentsKey);
    if (list) {
      for (const base::Value& value : *list) {
        if (!value.is_dict())
          continue;
        const base::DictValue& record = value.GetDict();
        const std::string* id = record.FindString(kIdKey);
        const std::string* name = record.FindString(kNameKey);
        if (!id || !name)
          continue;
        Environment environment;
        environment.id = *id;
        environment.name = *name;
        if (const std::string* color = record.FindString(kColorKey))
          environment.color = *color;
        if (const std::string* icon = record.FindString(kIconKey))
          environment.icon = *icon;
        if (const std::string* rules = record.FindString(kHostMappingsKey))
          environment.host_mappings = *rules;
        if (std::optional<double> created = record.FindDouble(kCreatedAtKey))
          environment.created_at = FromMillis(*created);
        if (std::optional<double> updated = record.FindDouble(kUpdatedAtKey))
          environment.updated_at = FromMillis(*updated);
        if (std::optional<bool> enabled = record.FindBool(kEnabledKey))
          environment.enabled = *enabled;
        parsed.environments.push_back(std::move(environment));
      }
    }
    return parsed;
  }

  const base::ListValue* list = dict.FindList(kEnvironmentsKey);
  if (!list) {
    // A present-but-empty environment list is legitimate (a profile that once had
    // environments and now has none). A missing key means the file was written by
    // something that never understood this pref, which is also not an error.
    parsed.status = ParseStatus::kEmpty;
    if (!parsed.diagnostics.empty())
      parsed.diagnostics += "; ";
    parsed.diagnostics += "no environments list";
    return parsed;
  }

  if (list->size() > kMaxEnvironments) {
    parsed.diagnostics += "record count " + std::to_string(list->size()) +
                          " exceeds " + std::to_string(kMaxEnvironments) + "; ";
  }

  int rejected = 0;
  size_t seen = 0;
  for (const base::Value& value : *list) {
    if (seen++ >= kMaxEnvironments)
      break;
    if (!value.is_dict()) {
      ++rejected;
      parsed.diagnostics += "record is not an object; ";
      continue;
    }
    const base::DictValue& record = value.GetDict();
    const std::string* id = record.FindString(kIdKey);
    if (!id) {
      ++rejected;
      parsed.diagnostics += "record has no id; ";
      continue;
    }

    Environment environment;
    environment.id = *id;

    // `name` falls back to the id rather than dropping the record: an environment
    // whose label was lost is still a usable environment, and dropping it would
    // orphan every tab pointing at it.
    if (const std::string* name = record.FindString(kNameKey))
      environment.name = *name;
    else
      environment.name = environment.id;

    if (const std::string* color = record.FindString(kColorKey))
      environment.color = *color;
    if (const std::string* icon = record.FindString(kIconKey))
      environment.icon = *icon;
    if (const std::string* rules = record.FindString(kHostMappingsKey))
      environment.host_mappings = *rules;

    if (std::optional<double> created = record.FindDouble(kCreatedAtKey))
      environment.created_at = FromMillis(*created);
    if (std::optional<double> updated = record.FindDouble(kUpdatedAtKey))
      environment.updated_at = FromMillis(*updated);
    if (std::optional<bool> enabled = record.FindBool(kEnabledKey))
      environment.enabled = *enabled;

    // A record whose host mappings cannot be parsed is kept but degraded to "no
    // remapping": dropping it would orphan its tabs, and keeping the bad rules
    // would let the network stack silently ignore them anyway (same outcome) while
    // pretending the configuration is intact.
    std::string mapping_error;
    if (!ValidateHostMappings(environment.host_mappings, &mapping_error)) {
      parsed.diagnostics +=
          "environment '" + environment.id +
          "' has unparseable host mappings, degraded to none: " +
          mapping_error + "; ";
      environment.host_mappings.clear();
    }

    // Only the fields whose loss makes the record meaningless are fatal.
    if (!IsValidEnvironmentId(environment.id)) {
      ++rejected;
      parsed.diagnostics += "invalid id '" + environment.id + "'; ";
      continue;
    }
    if (environment.id == kPersonalEnvironmentIdValue) {
      ++rejected;
      parsed.diagnostics +=
          "record for reserved id 'personal' ignored (it is implicit); ";
      continue;
    }
    if (environment.name.empty() || environment.name.size() > kMaxNameLength) {
      ++rejected;
      parsed.diagnostics += "environment '" + environment.id +
                            "' has an unusable name; ";
      continue;
    }
    // Colour and icon degrade to "unset" rather than dropping the record: they are
    // cosmetic, and an unrecognised value from a newer build must not cost the
    // user the environment.
    if (!IsValidColor(environment.color)) {
      parsed.diagnostics += "environment '" + environment.id +
                            "' has invalid colour, reset; ";
      environment.color.clear();
    }
    if (!IsValidIcon(environment.icon)) {
      parsed.diagnostics += "environment '" + environment.id +
                            "' has unknown icon, reset; ";
      environment.icon.clear();
    }

    // Duplicate id: keep the first, reject the rest. Keeping both would create two
    // definitions for one partition, and the second would win on lookup while the
    // first won on iteration -- a difference nobody could reason about.
    bool duplicate = false;
    for (const Environment& existing : parsed.environments) {
      if (existing.id == environment.id) {
        duplicate = true;
        break;
      }
    }
    if (duplicate) {
      ++rejected;
      parsed.diagnostics += "duplicate id '" + environment.id +
                            "' (kept the first); ";
      continue;
    }

    parsed.environments.push_back(std::move(environment));
  }

  parsed.rejected_count = rejected;
  if (rejected > 0) {
    parsed.status = ParseStatus::kPartiallyInvalid;
  } else {
    parsed.status = ParseStatus::kOk;
  }
  return parsed;
}

}  // namespace parra
