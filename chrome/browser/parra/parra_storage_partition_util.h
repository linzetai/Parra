// Copyright 2026 The Parra Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CHROME_BROWSER_PARRA_PARA_STORAGE_PARTITION_UTIL_H_
#define CHROME_BROWSER_PARRA_PARA_STORAGE_PARTITION_UTIL_H_

#include <string_view>

#include "base/files/file_path.h"
#include "content/public/browser/storage_partition_config.h"

class Profile;

namespace parra {

// True when the Parra PoC command line switches were accepted. When false, all
// Parra integration points must behave exactly like upstream Chromium.
bool IsParraPocEnabled();

// Returns the StoragePartitionConfig that should be used for a new tab whose
// owning environment is `environment_id`.
//
// Returns the default config when the PoC is disabled, when the id is the
// reserved `personal` environment, or when the id is unknown. Never returns a
// Parra partition by accident.
content::StoragePartitionConfig GetStoragePartitionConfigForEnvironment(
    Profile* profile,
    std::string_view environment_id);

// Returns the environment id that owns `config`, or an empty string for the
// default partition / unknown partitions.
std::string_view GetEnvironmentIdForStoragePartition(
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
  // every tab created outside Parra.
  kDefaultPartition,
  // Use a dedicated fixed partition; `config` is filled in.
  kDedicatedPartition,
  // The id is unknown. Restore must NOT proceed with the saved URL.
  kUnknownEnvironment,
};

struct ResolvedEnvironment {
  EnvironmentResolution status = EnvironmentResolution::kDefaultPartition;
  content::StoragePartitionConfig config;
};

// Resolves an environment id read from SessionTab::extra_data.
//
// Unlike GetStoragePartitionConfigForEnvironment() this NEVER fails open: an
// unrecognized id yields kUnknownEnvironment so restore can refuse the load.
ResolvedEnvironment ResolveEnvironmentForRestore(
    Profile* profile,
    std::string_view environment_id);

// Returns the net::HostMappingRules string for the partition identified by
// `relative_partition_path` (the argument that
// ContentBrowserClient::ConfigureNetworkContextParams() receives).
//
// Returns an empty string for the default partition, for non-Parra partitions,
// or when the PoC is disabled, in which case callers must leave the parameter
// unset so the global switch (if any) still applies.
std::string GetHostMappingRulesForPartitionPath(
    const base::FilePath& relative_partition_path);

// Logs the effective routing for a navigation when --parra-poc-verbose is set.
// `environment_id` may be empty for non-Parra pages.
void LogParraNavigationDiagnostics(std::string_view environment_id,
                                   const std::string& url);

}  // namespace parra

#endif  // CHROME_BROWSER_PARRA_PARA_STORAGE_PARTITION_UTIL_H_
