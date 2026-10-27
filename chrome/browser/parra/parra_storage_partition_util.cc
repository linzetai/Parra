// Copyright 2026 The Parra Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chrome/browser/parra/parra_storage_partition_util.h"

#include <string>

#include "base/command_line.h"
#include "base/logging.h"
#include "chrome/browser/parra/environment_registry.h"
#include "chrome/browser/parra/parra_switches.h"
#include "chrome/browser/profiles/profile.h"

namespace parra {

bool IsParraPocEnabled() {
  return EnvironmentRegistry::GetInstance()->enabled();
}

content::StoragePartitionConfig GetStoragePartitionConfigForEnvironment(
    Profile* profile,
    std::string_view environment_id) {
  content::BrowserContext* browser_context = profile;
  EnvironmentRegistry* registry = EnvironmentRegistry::GetInstance();
  if (!registry->enabled())
    return content::StoragePartitionConfig::CreateDefault(browser_context);

  return registry->GetStoragePartitionConfig(profile, environment_id);
}

std::string_view GetEnvironmentIdForStoragePartition(
    const content::StoragePartitionConfig& config) {
  EnvironmentRegistry* registry = EnvironmentRegistry::GetInstance();
  if (!registry->enabled())
    return std::string_view();

  const std::string id = registry->GetEnvironmentIdForConfig(config);
  if (id.empty())
    return std::string_view();

  // Return a view into the registry's stable storage rather than into the
  // temporary above. Registry environments live for the process lifetime.
  for (const EnvironmentRegistry::Environment& environment :
       registry->environments()) {
    if (environment.id == id)
      return environment.id;
  }
  return std::string_view();
}

bool IsEnvironmentStoragePartition(
    const content::StoragePartitionConfig& config) {
  EnvironmentRegistry* registry = EnvironmentRegistry::GetInstance();
  if (!registry->enabled())
    return false;
  return registry->IsParraPartition(config);
}

std::string GetHostMappingRulesForPartitionPath(
    const base::FilePath& relative_partition_path) {
  EnvironmentRegistry* registry = EnvironmentRegistry::GetInstance();
  if (!registry->enabled())
    return std::string();

  return registry->GetHostMappingRulesForPartitionPath(relative_partition_path);
}

void LogParraNavigationDiagnostics(std::string_view environment_id,
                                   const std::string& url) {
  const base::CommandLine* command_line =
      base::CommandLine::ForCurrentProcess();
  if (!command_line->HasSwitch(switches::kParraPocVerbose))
    return;

  LOG(INFO) << "parra: navigate env='" << environment_id << "' url=" << url;
}

}  // namespace parra
