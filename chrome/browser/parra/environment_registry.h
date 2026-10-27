// Copyright 2026 The Parra Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CHROME_BROWSER_PARRA_ENVIRONMENT_REGISTRY_H_
#define CHROME_BROWSER_PARRA_ENVIRONMENT_REGISTRY_H_

#include <string>
#include <string_view>
#include <vector>

#include "base/files/file_path.h"
#include "content/public/browser/browser_context.h"
#include "content/public/browser/storage_partition_config.h"

class Profile;

namespace parra {

// PoC-only, process-wide registry mapping stable Environment IDs to
// StoragePartitionConfigs and HostMappingRules.
//
// This is deliberately in-memory only and is populated once at startup from
// command line switches (see parra_switches.h). It is NOT the production
// EnvironmentRegistry described in docs/parra/02-architecture-decision-record.md
// (which has persistence, rename, reorder, etc.).
//
// Threading: InitializeFromCommandLine() must run on the UI thread during
// browser startup. All other methods are read-only afterwards and may be
// called from the UI thread.
class EnvironmentRegistry {
 public:
  struct Environment {
    // Stable identifier. Also used as the StoragePartitionConfig
    // `partition_name`. Sanitized on input to `[a-z0-9_-]`, so it is safe as a
    // directory component once hashed by //content.
    std::string id;
    // net::HostMappingRules syntax, e.g. "map sheet.test 127.0.0.1:8443".
    // Empty means "no remapping".
    std::string host_mapping_rules;
    // URL to open for this environment at startup.
    std::string url;
  };

  static EnvironmentRegistry* GetInstance();

  EnvironmentRegistry(const EnvironmentRegistry&) = delete;
  EnvironmentRegistry& operator=(const EnvironmentRegistry&) = delete;

  // Parses the PoC command line switches. Safe to call more than once; the
  // last successful call wins. Returns false if the switches are absent or
  // malformed, in which case the registry stays disabled and no Parra code
  // path activates.
  bool InitializeFromCommandLine();

  // True when at least one environment was configured.
  bool enabled() const { return enabled_; }

  // All configured environments, in declaration order.
  const std::vector<Environment>& environments() const { return environments_; }

  // Looks up an environment by id. Returns nullptr if not found.
  const Environment* Find(std::string_view id) const;

  // Returns the StoragePartitionConfig for `environment_id`.
  //
  // The reserved id `personal` maps to the default StoragePartition so the PoC
  // keeps native Chrome behavior for a comparison baseline environment.
  // Unknown ids also map to the default partition (fail open to upstream
  // behavior rather than creating an unintended partition).
  content::StoragePartitionConfig GetStoragePartitionConfig(
      Profile* profile,
      std::string_view environment_id) const;

  // Returns true if `config` is a Parra-created (non-default) partition.
  bool IsParraPartition(const content::StoragePartitionConfig& config) const;

  // Converts a StoragePartitionConfig back to its environment id. Returns an
  // empty string for the default partition or for unknown partitions.
  std::string GetEnvironmentIdForConfig(
      const content::StoragePartitionConfig& config) const;

  // Returns the host mapping rules for a partition, given only the
  // `relative_partition_path` handed to
  // ContentBrowserClient::ConfigureNetworkContextParams().
  //
  // This is the bridge for ProfileNetworkContextService, which cannot see the
  // environment id. Returns an empty string when the path is the default
  // partition or does not match any environment.
  std::string GetHostMappingRulesForPartitionPath(
      const base::FilePath& relative_partition_path) const;

  // Computes the `relative_partition_path` that //content uses for `config`.
  // Mirrors StoragePartitionImplMap::GetStoragePartitionPath().
  static base::FilePath GetRelativePartitionPathForConfig(
      const content::StoragePartitionConfig& config);

  // Same as above, but from the raw (domain, name) pair so callers without a
  // BrowserContext can use it. StoragePartitionConfig::Create() dereferences
  // the BrowserContext, so it must not be used only to compute a path.
  static base::FilePath GetRelativePartitionPathForConfig(
      const std::string& partition_domain,
      const std::string& partition_name);

  size_t size() const { return environments_.size(); }

  // Public so that base::NoDestructor can instantiate the singleton. Nothing
  // else should construct an EnvironmentRegistry.
  EnvironmentRegistry();
  ~EnvironmentRegistry();

 private:

  bool enabled_ = false;
  std::vector<Environment> environments_;
};

}  // namespace parra

#endif  // CHROME_BROWSER_PARRA_ENVIRONMENT_REGISTRY_H_
