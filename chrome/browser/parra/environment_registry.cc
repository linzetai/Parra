// Copyright 2026 The Parra Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chrome/browser/parra/environment_registry.h"

#include <array>
#include <utility>

#include "base/command_line.h"
#include "base/containers/span.h"
#include "base/logging.h"
#include "base/no_destructor.h"
#include "base/strings/string_number_conversions.h"
#include "base/strings/string_split.h"
#include "base/strings/string_util.h"
#include "chrome/browser/parra/parra_switches.h"
#include "chrome/browser/profiles/profile.h"
#include "crypto/hash.h"

namespace parra {

namespace {

// Must stay in sync with content/browser/storage_partition_impl_map.cc.
constexpr char kStoragePartitionDirname[] = "Storage";
constexpr char kExtensionsDirname[] = "ext";
constexpr char kDefaultPartitionDirname[] = "def";
constexpr size_t kPartitionNameHashBytes = 6;

// The partition domain used for all Parra environments. Must be a non-empty
// ASCII lowercase token; //content CHECKs on empty domains and hashes the
// name, so user-visible strings never reach the filesystem directly.
constexpr char kParraPartitionDomain[] = "parra";

// Reserved environment id that maps to the default (upstream) partition.
constexpr char kPersonalEnvironmentId[] = "personal";

// Restricts environment ids to characters that are safe in logs, on the
// command line, and inside a hashed directory name. Rejects anything else so a
// typo cannot silently create an unexpected storage location.
bool IsValidEnvironmentId(std::string_view id) {
  if (id.empty())
    return false;
  for (char c : id) {
    const bool ok = base::IsAsciiLower(c) || base::IsAsciiDigit(c) || c == '_' ||
                    c == '-';
    if (!ok)
      return false;
  }
  return true;
}

}  // namespace

// static
EnvironmentRegistry* EnvironmentRegistry::GetInstance() {
  static base::NoDestructor<EnvironmentRegistry> instance;
  return instance.get();
}

EnvironmentRegistry::EnvironmentRegistry() = default;
EnvironmentRegistry::~EnvironmentRegistry() = default;

bool EnvironmentRegistry::InitializeFromCommandLine() {
  const base::CommandLine* command_line =
      base::CommandLine::ForCurrentProcess();

  enabled_ = false;
  environments_.clear();

  if (!command_line->HasSwitch(switches::kParraPocEnvironments))
    return false;

  const std::string environments_value =
      command_line->GetSwitchValueASCII(switches::kParraPocEnvironments);
  const std::string host_rules_value =
      command_line->GetSwitchValueASCII(switches::kParraPocHostRules);

  // SPLIT_WANT_ALL is required so that an empty host-rules entry (`;;`) keeps
  // its position and means "no remapping for this environment".
  const std::vector<std::string> environment_specs = base::SplitString(
      environments_value, ";", base::TRIM_WHITESPACE, base::SPLIT_WANT_ALL);
  const std::vector<std::string> host_rules = base::SplitString(
      host_rules_value, ";", base::TRIM_WHITESPACE, base::SPLIT_WANT_ALL);

  if (host_rules.size() > 1 && host_rules.size() != environment_specs.size()) {
    LOG(ERROR) << "parra: --" << switches::kParraPocHostRules << " has "
               << host_rules.size() << " entries but --"
               << switches::kParraPocEnvironments << " has "
               << environment_specs.size()
               << "; refusing to start the PoC with ambiguous routing";
    return false;
  }

  std::vector<Environment> parsed;
  parsed.reserve(environment_specs.size());

  for (size_t i = 0; i < environment_specs.size(); ++i) {
    const std::string& spec = environment_specs[i];
    if (spec.empty())
      continue;

    // Format: <id>|<url>
    const size_t separator = spec.find('|');
    if (separator == std::string::npos) {
      LOG(ERROR) << "parra: malformed environment spec '" << spec
                 << "', expected <id>|<url>";
      return false;
    }

    Environment environment;
    environment.id = spec.substr(0, separator);
    environment.url = spec.substr(separator + 1);

    if (!IsValidEnvironmentId(environment.id)) {
      LOG(ERROR) << "parra: invalid environment id '" << environment.id
                 << "' (allowed: [a-z0-9_-])";
      return false;
    }
    if (environment.url.empty()) {
      LOG(ERROR) << "parra: environment '" << environment.id
                 << "' has an empty url";
      return false;
    }

    if (host_rules.size() == environment_specs.size())
      environment.host_mapping_rules = host_rules[i];

    parsed.push_back(std::move(environment));
  }

  if (parsed.empty()) {
    LOG(ERROR) << "parra: no valid environments configured";
    return false;
  }

  for (size_t i = 0; i < parsed.size(); ++i) {
    for (size_t j = i + 1; j < parsed.size(); ++j) {
      if (parsed[i].id == parsed[j].id) {
        LOG(ERROR) << "parra: duplicate environment id '" << parsed[i].id
                   << "'; two environments cannot share a StoragePartition";
        return false;
      }
    }
  }

  environments_ = std::move(parsed);
  enabled_ = true;

  for (const Environment& environment : environments_) {
    VLOG(1) << "parra: environment '" << environment.id << "' url='"
            << environment.url << "' rules='" << environment.host_mapping_rules
            << "'";
  }
  return true;
}

const EnvironmentRegistry::Environment* EnvironmentRegistry::Find(
    std::string_view id) const {
  for (const Environment& environment : environments_) {
    if (environment.id == id)
      return &environment;
  }
  return nullptr;
}

content::StoragePartitionConfig EnvironmentRegistry::GetStoragePartitionConfig(
    Profile* profile,
    std::string_view environment_id) const {
  content::BrowserContext* browser_context = profile;
  if (environment_id.empty() || environment_id == kPersonalEnvironmentId ||
      !Find(environment_id)) {
    return content::StoragePartitionConfig::CreateDefault(browser_context);
  }

  // OTR profiles must never persist partition data.
  //
  // StoragePartitionConfig::Create() already forces `in_memory ||
  // IsOffTheRecord()` (storage_partition_config.cc:39), so passing false here is
  // currently harmless. It is passed explicitly anyway, for two reasons:
  //
  //   1. The safety of an OTR profile then does not depend on a distant
  //      implementation detail that nothing in this file points at. If that
  //      coercion were ever narrowed, this call site would not silently start
  //      writing an incognito environment's cookies into the parent profile's
  //      Storage/ext/parra directory -- which is the same directory the regular
  //      profile uses, because OffTheRecordProfileImpl::GetPath() returns the
  //      PARENT's path (off_the_record_profile_impl.cc:354-356) and the
  //      partition path is a pure function of (domain, name).
  //   2. It documents the intent at the point where the decision is made.
  const bool in_memory = browser_context->IsOffTheRecord();
  content::StoragePartitionConfig config = content::StoragePartitionConfig::Create(
      browser_context,
      /*partition_domain=*/kParraPartitionDomain,
      /*partition_name=*/std::string(environment_id),
      /*in_memory=*/in_memory);

  // Invariant, checked rather than assumed: a config handed out for an OTR
  // profile must be in-memory. This is the only assertion that would catch a
  // future regression in the line above.
  if (browser_context->IsOffTheRecord() && !config.in_memory()) {
    LOG(ERROR) << "parra-partition: INVARIANT VIOLATED: OTR environment '"
               << environment_id
               << "' produced a PERSISTENT partition config; refusing to hand "
                  "it out (incognito data would land in the parent profile's "
                  "Storage directory)";
    return content::StoragePartitionConfig::CreateDefault(browser_context);
  }

  return config;
}

bool EnvironmentRegistry::IsParraPartition(
    const content::StoragePartitionConfig& config) const {
  return !config.is_default() &&
         config.partition_domain() == kParraPartitionDomain;
}

std::string EnvironmentRegistry::GetEnvironmentIdForConfig(
    const content::StoragePartitionConfig& config) const {
  if (!IsParraPartition(config))
    return std::string();
  if (!Find(config.partition_name()))
    return std::string();
  return config.partition_name();
}

// static
base::FilePath EnvironmentRegistry::GetRelativePartitionPathForConfig(
    const content::StoragePartitionConfig& config) {
  if (config.is_default())
    return base::FilePath();
  return GetRelativePartitionPathForConfig(config.partition_domain(),
                                           config.partition_name());
}

// static
base::FilePath EnvironmentRegistry::GetRelativePartitionPathForConfig(
    const std::string& partition_domain,
    const std::string& partition_name) {
  if (partition_domain.empty())
    return base::FilePath();

  base::FilePath path = base::FilePath::FromASCII(kStoragePartitionDirname)
                            .AppendASCII(kExtensionsDirname)
                            .Append(base::FilePath::FromUTF8Unsafe(
                                partition_domain));

  if (partition_name.empty())
    return path.AppendASCII(kDefaultPartitionDirname);

  // Mirrors StoragePartitionImplMap::GetStoragePartitionPath(): SHA-256 of the
  // partition name, truncated to 6 bytes, uppercase hex.
  const std::array<uint8_t, crypto::hash::kSha256Size> hash =
      crypto::hash::Sha256(partition_name);
  auto truncated_hash = base::span(hash).first<kPartitionNameHashBytes>();
  return path.AppendASCII(base::HexEncode(truncated_hash));
}

std::string EnvironmentRegistry::GetHostMappingRulesForPartitionPath(
    const base::FilePath& relative_partition_path) const {
  // This is a pure function of (domain, name) and deliberately ignores
  // in_memory: an OTR environment with the same id as a regular one must get the
  // same routing, otherwise the same URL would reach different backends depending
  // on whether the window is incognito. Data isolation is handled by the
  // in_memory flag on the StoragePartitionConfig, not by the rules.
  //
  // The empty path IS the default partition. An environment that maps to the
  // default partition (the reserved `personal` environment) still needs its
  // host rules, otherwise its tabs cannot resolve the test hostname at all and
  // fail with DNS_PROBE_FINISHED_NXDOMAIN while every other environment works.
  //
  // This used to return early on an empty path, which silently disabled host
  // mapping for Personal only -- a bug that looked like a partition mix-up.
  for (const Environment& environment : environments_) {
    // NOTE: deliberately not going through StoragePartitionConfig::Create(),
    // which dereferences a BrowserContext. The relative path is a pure
    // function of (domain, name), so compute it directly.
    const bool uses_default_partition =
        environment.id == kPersonalEnvironmentId;
    const bool path_matches =
        uses_default_partition
            ? relative_partition_path.empty()
            : GetRelativePartitionPathForConfig(kParraPartitionDomain,
                                                environment.id) ==
                  relative_partition_path;
    if (path_matches)
      return environment.host_mapping_rules;
  }
  return std::string();
}

}  // namespace parra
