// Copyright 2026 The Parra Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CHROME_BROWSER_UI_WEBUI_PARRA_PARRA_PAGE_HANDLER_H_
#define CHROME_BROWSER_UI_WEBUI_PARRA_PARRA_PAGE_HANDLER_H_

#include <memory>
#include <stdint.h>
#include <string>

#include "base/callback_list.h"
#include "base/memory/raw_ptr.h"
#include "base/memory/weak_ptr.h"
#include "chrome/browser/parra/environment_store.h"
#include "chrome/browser/ui/webui/parra/parra.mojom.h"
#include "mojo/public/cpp/bindings/pending_receiver.h"
#include "mojo/public/cpp/bindings/pending_remote.h"
#include "mojo/public/cpp/bindings/receiver.h"
#include "mojo/public/cpp/bindings/remote.h"
#include "services/network/public/mojom/network_context.mojom-forward.h"

class Profile;
namespace content {
class StoragePartition;
}

// Implements chrome://parra's Mojo interface.
//
// DESIGN RULE: THIS CLASS DOES NOT REIMPLEMENT STORE VALIDATION.
//
// EnvironmentStore remains the source of truth for validation, reserved ids,
// and read-only refusal. This handler translates those results for Mojo and
// owns the browser-facing work the store cannot perform: checking open tabs
// before deletion and applying saved host mappings to the live NetworkContext.
// Keeping that work here avoids giving the profile-scoped store dependencies on
// windows or network services. The handler also provides two page-facing
// responsibilities:
//
//   * TRANSLATION. StoreResult/PersistState become Mojo enums, and store
//     definitions become EnvironmentInfo with the derived facts the page needs
//     (is_personal, tab_count).
//   * FAN-OUT. The store's observer is forwarded to the page as
//     OnEnvironmentsChanged(), so a change made in the toolbar switcher or in
//     another window appears here without a reload.
//
// ON THE DELETE PRECONDITION: EnvironmentStore::Delete() deliberately does NOT
// check for open tabs -- it has no view of any TabStripModel, and giving a
// Profile-scoped service a window dependency would be the wrong direction. Its
// comment says the CALLER performs the check. This handler is that caller, so
// the check lives in DeleteEnvironment() and is the one place the page cannot
// bypass.
//
// LIFETIME: owned by ParraUI, one per page. The store subscription is a member,
// so it unregisters when the handler dies (page navigation, window close) and
// the store can never notify a destroyed object.
class ParraPageHandler : public parra::mojom::PageHandler {
 public:
  ParraPageHandler(mojo::PendingReceiver<parra::mojom::PageHandler> receiver,
                   mojo::PendingRemote<parra::mojom::Page> page,
                   Profile* profile);
  ~ParraPageHandler() override;

  ParraPageHandler(const ParraPageHandler&) = delete;
  ParraPageHandler& operator=(const ParraPageHandler&) = delete;

  // parra::mojom::PageHandler:
  void GetEnvironments(GetEnvironmentsCallback callback) override;
  void GetExternalLinkPolicy(GetExternalLinkPolicyCallback callback) override;
  void SetExternalLinkPolicy(int32_t policy,
                             const std::string& environment_id,
                             SetExternalLinkPolicyCallback callback) override;
  void CreateEnvironment(const std::string& id,
                         const std::string& name,
                         const std::string& color,
                         const std::string& icon,
                         const std::string& host_mappings,
                         parra::mojom::HostMappingFormat host_mapping_format,
                         CreateEnvironmentCallback callback) override;
  void UpdateEnvironment(const std::string& id,
                         bool has_name,
                         const std::string& name,
                         bool has_color,
                         const std::string& color,
                         bool has_icon,
                         const std::string& icon,
                         UpdateEnvironmentCallback callback) override;
  void SetHostMappings(const std::string& id,
                       const std::string& host_mappings,
                       parra::mojom::HostMappingFormat host_mapping_format,
                       SetHostMappingsCallback callback) override;
  void SetEnabled(const std::string& id,
                  bool enabled,
                  SetEnabledCallback callback) override;
  void DeleteEnvironment(const std::string& id,
                         DeleteEnvironmentCallback callback) override;

 private:
  // Called by the store subscription. Forwards to the page so a change made
  // elsewhere appears without a reload.
  void OnEnvironmentChanged(const parra::EnvironmentChange& change);

  // Builds one EnvironmentInfo. Returns a pointer because the generated Mojo
  // type is move-only and its callback signature takes an Owned pointer: a
  // by-value return cannot be assigned into a mojom struct field, so every
  // caller would have to wrap it anyway.
  parra::mojom::EnvironmentInfoPtr BuildInfo(
      const parra::Environment& environment,
      bool is_personal) const;

  // The reserved Personal environment as the page sees it. Personal is implicit
  // (never a stored record), so it is built from
  // EnvironmentStore::PersonalEnvironment rather than from the list.
  parra::mojom::EnvironmentInfoPtr BuildPersonalInfo() const;

  // Counts tabs in `environment_id` across every browser window of this
  // profile. Returns 0 when no window exists yet. The delete rule is global, so
  // this must not be limited to one window.
  int CountTabsInEnvironment(const std::string& environment_id) const;

  // Translation helpers.
  static parra::mojom::PersistStatus ToMojo(parra::PersistState state);
  static parra::mojom::StoreOutcome ToMojo(parra::StoreResult result);
  static parra::mojom::HostMappingFormat ToMojo(
      parra::HostMappingFormat format);
  static parra::HostMappingFormat FromMojo(
      parra::mojom::HostMappingFormat format);

  // Builds the WriteResult for one store call, including the post-write
  // definition so the page can re-render without a second (racable) round trip.
  parra::mojom::WriteResultPtr MakeWriteResult(
      parra::StoreResult result,
      const std::string& error,
      const std::string& environment_id,
      bool host_mapping_write,
      parra::mojom::HostMappingApplyStatus host_mapping_apply_status =
          parra::mojom::HostMappingApplyStatus::kNotRequested,
      uint64_t desired_revision = 0,
      uint64_t applied_revision = 0,
      uint64_t network_context_generation = 0,
      const std::string& routing_policy_reason = std::string());

  void OnHostMappingRulesApplied(
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
      const std::string& policy_reason);
  void OnHostMappingHostCacheCleared(
      content::StoragePartition* partition,
      base::FilePath relative_partition_path,
      std::string environment_id,
      uint64_t desired_revision,
      uint64_t applied_revision,
      uint64_t network_context_generation,
      parra::mojom::HostMappingApplyStatus apply_status,
      std::string policy_reason,
      SetHostMappingsCallback callback);
  void OnHostMappingConnectionsClosed(
      content::StoragePartition* partition,
      base::FilePath relative_partition_path,
      std::string environment_id,
      uint64_t desired_revision,
      uint64_t applied_revision,
      uint64_t network_context_generation,
      parra::mojom::HostMappingApplyStatus apply_status,
      std::string policy_reason,
      SetHostMappingsCallback callback);
  void OnHostMappingHttpCacheCleared(
      base::FilePath relative_partition_path,
      std::string environment_id,
      uint64_t desired_revision,
      uint64_t applied_revision,
      uint64_t network_context_generation,
      parra::mojom::HostMappingApplyStatus apply_status,
      std::string policy_reason,
      SetHostMappingsCallback callback);

  const parra::Environment* FindOrNull(const std::string& id) const;

  raw_ptr<Profile> profile_;
  raw_ptr<parra::EnvironmentStore> store_;

  mojo::Receiver<parra::mojom::PageHandler> receiver_;
  mojo::Remote<parra::mojom::Page> page_;

  base::CallbackListSubscription store_subscription_;

  base::WeakPtrFactory<ParraPageHandler> weak_factory_{this};
};

#endif  // CHROME_BROWSER_UI_WEBUI_PARRA_PARRA_PAGE_HANDLER_H_
