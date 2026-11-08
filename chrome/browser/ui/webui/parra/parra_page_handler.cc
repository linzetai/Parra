// Copyright 2026 The Parra Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chrome/browser/ui/webui/parra/parra_page_handler.h"

#include <utility>

#include "base/functional/bind.h"
#include "base/logging.h"
#include "base/strings/stringprintf.h"
#include "chrome/browser/parra/environment_index_mapper.h"
#include "chrome/browser/parra/parra_storage_partition_util.h"
#include "chrome/browser/profiles/profile.h"
#include "chrome/common/pref_names.h"
#include "components/prefs/pref_service.h"
// GetAllBrowserWindowInterfaces(), not BrowserCollection::GetBrowsers(): the
// latter is a protected member of BrowserCollection (subclasses implement it,
// clients are only meant to observe), so it is not callable from here. This
// free function is the documented way to enumerate every window, and it is an
// explicit snapshot, which is what this counting pass wants -- a window
// appearing mid-count must not change the answer half way through.
#include "chrome/browser/ui/browser_window/public/browser_window_interface.h"
#include "chrome/browser/ui/browser_window/public/browser_window_interface_iterator.h"
#include "chrome/browser/ui/tabs/tab_strip_model.h"
#include "content/public/browser/storage_partition.h"
#include "content/public/browser/web_contents.h"
#include "services/network/public/mojom/clear_data_filter.mojom.h"
#include "services/network/public/mojom/network_context.mojom.h"

namespace {

// Personal is the reserved, implicit environment: it has no stored record, it
// always exists, and its host mappings live in their own pref because there is
// nowhere else to put them. Mirrors parra::kDefaultEnvironmentId.
constexpr char kPersonalId[] = "personal";

}  // namespace

ParraPageHandler::ParraPageHandler(
    mojo::PendingReceiver<parra::mojom::PageHandler> receiver,
    mojo::PendingRemote<parra::mojom::Page> page,
    Profile* profile)
    : profile_(profile),
      store_(parra::EnvironmentStore::GetForProfile(profile)),
      receiver_(this, std::move(receiver)),
      page_(std::move(page)) {
  if (!store_) {
    return;
  }

  // Subscribed here rather than in GetEnvironments() so a change that happens
  // before the page's first call is still delivered. The subscription is a
  // member: it dies with this handler, so the store cannot notify a destroyed
  // object.
  store_subscription_ = store_->AddObserver(base::BindRepeating(
      &ParraPageHandler::OnEnvironmentChanged, base::Unretained(this)));
}

ParraPageHandler::~ParraPageHandler() = default;

void ParraPageHandler::OnEnvironmentChanged(
    const parra::EnvironmentChange& change) {
  if (page_) {
    page_->OnEnvironmentsChanged();
  }
}

parra::mojom::PersistStatus ParraPageHandler::ToMojo(
    parra::PersistState state) {
  switch (state) {
    case parra::PersistState::kUnchanged:
      return parra::mojom::PersistStatus::kUnchanged;
    case parra::PersistState::kMemoryUpdated:
      return parra::mojom::PersistStatus::kMemoryUpdated;
    case parra::PersistState::kPersisted:
      return parra::mojom::PersistStatus::kPersisted;
    case parra::PersistState::kPersistFailed:
      return parra::mojom::PersistStatus::kPersistFailed;
  }
  return parra::mojom::PersistStatus::kPersistFailed;
}

parra::mojom::StoreOutcome ParraPageHandler::ToMojo(
    parra::StoreResult result) {
  switch (result) {
    case parra::StoreResult::kOk:
      return parra::mojom::StoreOutcome::kOk;
    case parra::StoreResult::kInvalidId:
      return parra::mojom::StoreOutcome::kInvalidId;
    case parra::StoreResult::kDuplicateId:
      return parra::mojom::StoreOutcome::kDuplicateId;
    case parra::StoreResult::kNotFound:
      return parra::mojom::StoreOutcome::kNotFound;
    case parra::StoreResult::kReservedId:
      return parra::mojom::StoreOutcome::kReservedId;
    case parra::StoreResult::kInvalidField:
      return parra::mojom::StoreOutcome::kInvalidField;
    case parra::StoreResult::kInvalidHostMapping:
      return parra::mojom::StoreOutcome::kInvalidHostMapping;
    case parra::StoreResult::kReadOnly:
      return parra::mojom::StoreOutcome::kReadOnly;
    case parra::StoreResult::kPersistFailed:
      return parra::mojom::StoreOutcome::kPersistFailed;
    case parra::StoreResult::kRefusedTabsOpen:
      return parra::mojom::StoreOutcome::kRefusedTabsOpen;
  }
  return parra::mojom::StoreOutcome::kPersistFailed;
}

parra::mojom::HostMappingFormat ParraPageHandler::ToMojo(
    parra::HostMappingFormat format) {
  switch (format) {
    case parra::HostMappingFormat::kStandardHosts:
      return parra::mojom::HostMappingFormat::kStandardHosts;
    case parra::HostMappingFormat::kWhistleHost:
      return parra::mojom::HostMappingFormat::kWhistleHost;
    case parra::HostMappingFormat::kChromiumMap:
      return parra::mojom::HostMappingFormat::kChromiumMap;
    case parra::HostMappingFormat::kUnsupported:
      return parra::mojom::HostMappingFormat::kUnsupported;
  }
  return parra::mojom::HostMappingFormat::kUnsupported;
}

parra::HostMappingFormat ParraPageHandler::FromMojo(
    parra::mojom::HostMappingFormat format) {
  switch (format) {
    case parra::mojom::HostMappingFormat::kStandardHosts:
      return parra::HostMappingFormat::kStandardHosts;
    case parra::mojom::HostMappingFormat::kWhistleHost:
      return parra::HostMappingFormat::kWhistleHost;
    case parra::mojom::HostMappingFormat::kChromiumMap:
      return parra::HostMappingFormat::kChromiumMap;
    case parra::mojom::HostMappingFormat::kUnsupported:
      return parra::HostMappingFormat::kUnsupported;
  }
  return parra::HostMappingFormat::kUnsupported;
}

void ParraPageHandler::GetEnvironments(GetEnvironmentsCallback callback) {
  auto result = parra::mojom::EnvironmentListResult::New();
  if (!store_) {
    result->read_only = true;
    result->persist_status = parra::mojom::PersistStatus::kPersistFailed;
    result->load_diagnostics = "no environment store for this profile";
    std::move(callback).Run(std::move(result));
    return;
  }

  result->environments.push_back(BuildPersonalInfo());
  for (const parra::Environment& environment : store_->environments()) {
    result->environments.push_back(BuildInfo(environment, false));
  }
  result->read_only = store_->read_only();
  result->persist_status = ToMojo(store_->persist_state());
  result->load_diagnostics = store_->load_diagnostics();
  std::move(callback).Run(std::move(result));
}

void ParraPageHandler::GetExternalLinkPolicy(
    GetExternalLinkPolicyCallback callback) {
  PrefService* prefs = profile_ ? profile_->GetPrefs() : nullptr;
  std::move(callback).Run(
      prefs ? prefs->GetInteger(::prefs::kParraExternalLinkPolicy) : 0,
      prefs ? prefs->GetString(::prefs::kParraExternalLinkEnvironment)
            : std::string(kPersonalId));
}

void ParraPageHandler::SetExternalLinkPolicy(
    int32_t policy,
    const std::string& environment_id,
    SetExternalLinkPolicyCallback callback) {
  PrefService* prefs = profile_ ? profile_->GetPrefs() : nullptr;
  if (!prefs || policy < 0 || policy > 2) {
    std::move(callback).Run(false, "无效的外部链接策略。");
    return;
  }
  if (policy == 2 && environment_id != kPersonalId) {
    const parra::Environment* environment =
        store_ ? store_->Find(environment_id) : nullptr;
    if (!environment || !environment->enabled) {
      std::move(callback).Run(false, "指定环境不存在或已停用。");
      return;
    }
  }
  prefs->SetInteger(::prefs::kParraExternalLinkPolicy, policy);
  prefs->SetString(::prefs::kParraExternalLinkEnvironment,
                   policy == 2 ? environment_id : kPersonalId);
  prefs->CommitPendingWrite();
  std::move(callback).Run(true, std::string());
}

parra::mojom::EnvironmentInfoPtr ParraPageHandler::BuildInfo(
    const parra::Environment& environment,
    bool is_personal) const {
  auto info = parra::mojom::EnvironmentInfo::New();
  info->id = environment.id;
  info->name = environment.name;
  info->color = environment.color;
  info->icon = environment.icon;
  info->host_mappings = environment.host_mappings;
  info->host_mapping_format = ToMojo(environment.host_mapping_format);
  info->enabled = environment.enabled;
  info->is_personal = is_personal;
  info->tab_count = CountTabsInEnvironment(environment.id);
  return info;
}

parra::mojom::EnvironmentInfoPtr ParraPageHandler::BuildPersonalInfo() const {
  const parra::Environment personal =
      store_ ? store_->PersonalEnvironment() : parra::Environment();
  return BuildInfo(personal, true);
}

int ParraPageHandler::CountTabsInEnvironment(
    const std::string& environment_id) const {
  int count = 0;
  for (BrowserWindowInterface* window : GetAllBrowserWindowInterfaces()) {
    if (!window || window->GetProfile() != profile_) {
      continue;
    }
    TabStripModel* model = window->GetTabStripModel();
    if (!model) {
      continue;
    }
    for (int index = 0; index < model->count(); ++index) {
      content::WebContents* contents = model->GetWebContentsAt(index);
      if (contents &&
          parra::EnvironmentIndexMapper::GetEnvironmentForContents(contents) ==
              environment_id) {
        ++count;
      }
    }
  }
  return count;
}

parra::mojom::WriteResultPtr ParraPageHandler::MakeWriteResult(
    parra::StoreResult result,
    const std::string& error,
    const std::string& environment_id,
    bool host_mapping_write,
    parra::mojom::HostMappingApplyStatus host_mapping_apply_status,
    uint64_t desired_revision,
    uint64_t applied_revision,
    uint64_t network_context_generation,
    const std::string& routing_policy_reason) {
  auto out = parra::mojom::WriteResult::New();
  out->outcome = ToMojo(result);
  out->error = error;
  out->persist_status = store_ ? ToMojo(store_->persist_state())
                               : parra::mojom::PersistStatus::kPersistFailed;
  out->host_mapping_apply_status = host_mapping_apply_status;
  out->host_mapping_apply_failed =
      host_mapping_write && result == parra::StoreResult::kOk &&
      host_mapping_apply_status ==
          parra::mojom::HostMappingApplyStatus::kFailed;
  out->desired_host_mapping_revision = desired_revision;
  out->applied_host_mapping_revision = applied_revision;
  out->network_context_generation = network_context_generation;
  out->routing_policy_reason = routing_policy_reason;

  if (environment_id == kPersonalId) {
    out->environment = BuildPersonalInfo();
  } else if (const parra::Environment* environment =
                 store_ ? store_->Find(environment_id) : nullptr) {
    out->environment = BuildInfo(*environment, false);
  }
  return out;
}

void ParraPageHandler::CreateEnvironment(
    const std::string& id,
    const std::string& name,
    const std::string& color,
    const std::string& icon,
    const std::string& host_mappings,
    parra::mojom::HostMappingFormat host_mapping_format,
    CreateEnvironmentCallback callback) {
  if (!store_) {
    std::move(callback).Run(MakeWriteResult(
        parra::StoreResult::kReadOnly, "no environment store for this profile",
        id, /*host_mapping_write=*/false));
    return;
  }
  parra::EnvironmentDraft draft;
  draft.id = id;
  draft.name = name;
  draft.color = color;
  draft.icon = icon;
  draft.host_mappings = host_mappings;
  draft.host_mapping_format = FromMojo(host_mapping_format);
  std::string error;
  const parra::StoreResult result = store_->Create(draft, &error);
  std::move(callback).Run(
      MakeWriteResult(result, error, id, /*host_mapping_write=*/false));
}

void ParraPageHandler::UpdateEnvironment(
    const std::string& id,
    bool has_name,
    const std::string& name,
    bool has_color,
    const std::string& color,
    bool has_icon,
    const std::string& icon,
    UpdateEnvironmentCallback callback) {
  if (!store_) {
    std::move(callback).Run(MakeWriteResult(
        parra::StoreResult::kReadOnly, "no environment store for this profile",
        id, /*host_mapping_write=*/false));
    return;
  }
  parra::EnvironmentPatch patch;
  if (has_name) {
    patch.name = name;
  }
  if (has_color) {
    patch.color = color;
  }
  if (has_icon) {
    patch.icon = icon;
  }
  std::string error;
  const parra::StoreResult result = store_->Update(id, patch, &error);
  std::move(callback).Run(
      MakeWriteResult(result, error, id, /*host_mapping_write=*/false));
}

void ParraPageHandler::SetHostMappings(
    const std::string& id,
    const std::string& host_mappings,
    parra::mojom::HostMappingFormat host_mapping_format,
    SetHostMappingsCallback callback) {
  if (!store_) {
    std::move(callback).Run(MakeWriteResult(
        parra::StoreResult::kReadOnly, "no environment store for this profile",
        id, /*host_mapping_write=*/true));
    return;
  }

  std::string error;
  const parra::HostMappingFormat format = FromMojo(host_mapping_format);
  const parra::StoreResult result =
      id == kPersonalId
          ? store_->SetPersonalHostMappings(host_mappings, format, &error)
          : [&]() {
              parra::EnvironmentPatch patch;
              patch.host_mappings = host_mappings;
              patch.host_mapping_format = format;
              return store_->Update(id, patch, &error);
            }();
  if (result != parra::StoreResult::kOk) {
    std::move(callback).Run(
        MakeWriteResult(result, error, id, /*host_mapping_write=*/true));
    return;
  }

  const parra::PartitionResolution resolution =
      parra::ResolvePartitionForEnvironment(profile_, id);
  const base::FilePath relative_partition_path =
      parra::EnvironmentStore::GetRelativePartitionPathForConfig(
          resolution.config);
  const uint64_t desired_revision =
      store_->GetHostMappingRevisionForPartitionPath(relative_partition_path);
  if (resolution.lookup != parra::EnvironmentLookup::kPersonal &&
      resolution.lookup != parra::EnvironmentLookup::kNamed &&
      resolution.lookup != parra::EnvironmentLookup::kDisabled) {
    std::move(callback).Run(MakeWriteResult(
        parra::StoreResult::kOk,
        "规则已保存，但无法确定现有环境的网络上下文。", id,
        /*host_mapping_write=*/true,
        parra::mojom::HostMappingApplyStatus::kSavedPending,
        desired_revision));
    return;
  }

  content::StoragePartition* partition =
      profile_->GetStoragePartition(resolution.config, /*can_create=*/false);
  if (!partition || !partition->GetNetworkContext()) {
    std::move(callback).Run(MakeWriteResult(
        parra::StoreResult::kOk, std::string(), id,
        /*host_mapping_write=*/true,
        parra::mojom::HostMappingApplyStatus::kSavedPending,
        desired_revision));
    return;
  }

  const std::string compiled_rules =
      store_->GetHostMappingRulesForPartitionPath(relative_partition_path);
  partition->GetNetworkContext()->SetHostMappingRules(
      compiled_rules, desired_revision,
      base::BindOnce(&ParraPageHandler::OnHostMappingRulesApplied,
                     weak_factory_.GetWeakPtr(), partition,
                     relative_partition_path, id, desired_revision,
                     std::move(callback)));
}

void ParraPageHandler::OnHostMappingRulesApplied(
    content::StoragePartition* partition,
    base::FilePath relative_partition_path,
    std::string environment_id,
    uint64_t desired_revision,
    SetHostMappingsCallback callback,
    bool applied,
    const std::string& error,
    uint64_t applied_revision,
    uint64_t network_context_generation,
    network::mojom::HostOverridePolicy proxy_policy,
    const std::string& policy_reason) {
  if (!applied) {
    LOG(ERROR) << "parra-webui: live host mapping update failed for '"
               << environment_id << "': " << error;
    std::move(callback).Run(MakeWriteResult(
        parra::StoreResult::kOk,
        base::StringPrintf("配置已保存，但未能应用到 NetworkContext。详情：%s",
                           error.c_str()),
        environment_id, /*host_mapping_write=*/true,
        parra::mojom::HostMappingApplyStatus::kFailed, desired_revision,
        applied_revision, network_context_generation, policy_reason));
    return;
  }

  const uint64_t latest_revision =
      store_->GetHostMappingRevisionForPartitionPath(relative_partition_path);
  if (applied_revision != desired_revision ||
      latest_revision != desired_revision) {
    std::move(callback).Run(MakeWriteResult(
        parra::StoreResult::kOk, "新 revision 正在应用，当前状态待确认。",
        environment_id, /*host_mapping_write=*/true,
        parra::mojom::HostMappingApplyStatus::kSavedPending, latest_revision,
        applied_revision, network_context_generation, policy_reason));
    return;
  }

  const auto apply_status =
      proxy_policy == network::mojom::HostOverridePolicy::kAllowed
          ? parra::mojom::HostMappingApplyStatus::kAppliedToNetworkContext
          : (proxy_policy == network::mojom::HostOverridePolicy::kUnknown
                 ? parra::mojom::HostMappingApplyStatus::kUnknown
                 : parra::mojom::HostMappingApplyStatus::kPolicyBlocked);

  // Keep the live page intact; invalidate only this NetworkContext's DNS/cache
  // state and connections. Existing JavaScript and form state are untouched.
  partition->GetNetworkContext()->ClearHostCache(
      nullptr,
      base::BindOnce(&ParraPageHandler::OnHostMappingHostCacheCleared,
                     weak_factory_.GetWeakPtr(), partition,
                     relative_partition_path, environment_id, desired_revision,
                     applied_revision, network_context_generation, apply_status,
                     policy_reason, std::move(callback)));
}

void ParraPageHandler::OnHostMappingHostCacheCleared(
    content::StoragePartition* partition,
    base::FilePath relative_partition_path,
    std::string environment_id,
    uint64_t desired_revision,
    uint64_t applied_revision,
    uint64_t network_context_generation,
    parra::mojom::HostMappingApplyStatus apply_status,
    std::string policy_reason,
    SetHostMappingsCallback callback) {
  if (!store_) {
    return;
  }
  const uint64_t latest_revision =
      store_->GetHostMappingRevisionForPartitionPath(relative_partition_path);
  if (latest_revision != desired_revision) {
    std::move(callback).Run(MakeWriteResult(
        parra::StoreResult::kOk, "新 revision 正在应用，当前状态待确认。",
        environment_id, /*host_mapping_write=*/true,
        parra::mojom::HostMappingApplyStatus::kSavedPending, latest_revision,
        applied_revision, network_context_generation, policy_reason));
    return;
  }
  partition->GetNetworkContext()->CloseAllConnections(base::BindOnce(
      &ParraPageHandler::OnHostMappingConnectionsClosed,
      weak_factory_.GetWeakPtr(), partition, relative_partition_path,
      environment_id, desired_revision, applied_revision,
      network_context_generation, apply_status, std::move(policy_reason),
      std::move(callback)));
}

void ParraPageHandler::OnHostMappingConnectionsClosed(
    content::StoragePartition* partition,
    base::FilePath relative_partition_path,
    std::string environment_id,
    uint64_t desired_revision,
    uint64_t applied_revision,
    uint64_t network_context_generation,
    parra::mojom::HostMappingApplyStatus apply_status,
    std::string policy_reason,
    SetHostMappingsCallback callback) {
  if (!store_) {
    return;
  }
  if (store_->GetHostMappingRevisionForPartitionPath(relative_partition_path) !=
      desired_revision) {
    std::move(callback).Run(MakeWriteResult(
        parra::StoreResult::kOk, "新 revision 正在应用，当前状态待确认。",
        environment_id, /*host_mapping_write=*/true,
        parra::mojom::HostMappingApplyStatus::kSavedPending,
        store_->GetHostMappingRevisionForPartitionPath(relative_partition_path),
        applied_revision, network_context_generation, policy_reason));
    return;
  }
  partition->GetNetworkContext()->ClearHttpCacheLogically(
      base::Time(), base::Time::Max(), nullptr,
      base::BindOnce(&ParraPageHandler::OnHostMappingHttpCacheCleared,
                     weak_factory_.GetWeakPtr(), relative_partition_path,
                     environment_id, desired_revision, applied_revision,
                     network_context_generation, apply_status,
                     std::move(policy_reason), std::move(callback)));
}

void ParraPageHandler::OnHostMappingHttpCacheCleared(
    base::FilePath relative_partition_path,
    std::string environment_id,
    uint64_t desired_revision,
    uint64_t applied_revision,
    uint64_t network_context_generation,
    parra::mojom::HostMappingApplyStatus apply_status,
    std::string policy_reason,
    SetHostMappingsCallback callback) {
  if (!store_) {
    return;
  }
  const uint64_t latest_revision =
      store_->GetHostMappingRevisionForPartitionPath(relative_partition_path);
  if (latest_revision != desired_revision) {
    std::move(callback).Run(MakeWriteResult(
        parra::StoreResult::kOk, "新 revision 正在应用，当前状态待确认。",
        environment_id, /*host_mapping_write=*/true,
        parra::mojom::HostMappingApplyStatus::kSavedPending, latest_revision,
        applied_revision, network_context_generation, policy_reason));
    return;
  }
  const std::string status_message =
      apply_status == parra::mojom::HostMappingApplyStatus::kPolicyBlocked
          ? "映射已应用到 NetworkContext，但代理策略禁止 DIRECT；匹配请求会被拒绝。"
          : (apply_status == parra::mojom::HostMappingApplyStatus::kUnknown
                 ? "映射已应用到 NetworkContext，但代理策略来源未知；匹配请求会被拒绝。"
                 : std::string());
  std::move(callback).Run(MakeWriteResult(
      parra::StoreResult::kOk, status_message, environment_id,
      /*host_mapping_write=*/true, apply_status, desired_revision,
      applied_revision, network_context_generation, policy_reason));
}

void ParraPageHandler::SetEnabled(const std::string& id,
                                  bool enabled,
                                  SetEnabledCallback callback) {
  if (!store_) {
    std::move(callback).Run(MakeWriteResult(
        parra::StoreResult::kReadOnly, "no environment store for this profile",
        id, /*host_mapping_write=*/false));
    return;
  }

  // Personal is always enabled by definition: it has no record to store a flag
  // in, and a disabled Personal would leave the browser with no valid default.
  if (id == kPersonalId) {
    std::move(callback).Run(
        MakeWriteResult(parra::StoreResult::kReservedId,
                        "'personal' cannot be disabled; it is the environment "
                        "every window starts in",
                        id, /*host_mapping_write=*/false));
    return;
  }

  std::string error;
  const parra::StoreResult result = store_->SetEnabled(id, enabled, &error);
  std::move(callback).Run(MakeWriteResult(result, error, id,
                                          /*host_mapping_write=*/false));
}

void ParraPageHandler::DeleteEnvironment(const std::string& id,
                                         DeleteEnvironmentCallback callback) {
  if (!store_) {
    std::move(callback).Run(MakeWriteResult(
        parra::StoreResult::kReadOnly, "no environment store for this profile",
        id, /*host_mapping_write=*/false));
    return;
  }

  if (id == kPersonalId) {
    std::move(callback).Run(MakeWriteResult(parra::StoreResult::kReservedId,
                                            "'personal' cannot be deleted", id,
                                            /*host_mapping_write=*/false));
    return;
  }

  // THE PRECONDITION EnvironmentStore CANNOT CHECK ITSELF.
  //
  // Delete()'s comment is explicit: "The tab check is not done here: the store
  // has no view of any tab strip, and giving it one would make a Profile-scoped
  // service depend on window state. The caller that owns the windows performs
  // the check". This handler is that caller.
  //
  // Why refuse rather than delete: this build has no invalidated-environment
  // UI. A tab whose environment no longer exists would keep running in a
  // partition nobody can name, and its pages could not be explained to the
  // user. Refusing is the honest behaviour until that UI exists.
  const int tabs = CountTabsInEnvironment(id);
  if (tabs > 0) {
    std::move(callback).Run(MakeWriteResult(
        parra::StoreResult::kRefusedTabsOpen,
        base::StringPrintf("this environment still has %d tab(s) open; close "
                           "them before deleting "
                           "it",
                           tabs),
        id, /*host_mapping_write=*/false));
    return;
  }

  std::string error;
  const parra::StoreResult result = store_->Delete(id, &error);
  std::move(callback).Run(MakeWriteResult(result, error, id,
                                          /*host_mapping_write=*/false));
}
