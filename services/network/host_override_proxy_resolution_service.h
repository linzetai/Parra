// Copyright 2026 The Parra Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef SERVICES_NETWORK_HOST_OVERRIDE_PROXY_RESOLUTION_SERVICE_H_
#define SERVICES_NETWORK_HOST_OVERRIDE_PROXY_RESOLUTION_SERVICE_H_

#include <memory>
#include <string>

#include "base/component_export.h"
#include "base/sequence_checker.h"
#include "net/base/host_mapping_rules.h"
#include "net/proxy_resolution/proxy_resolution_service.h"
#include "services/network/public/mojom/network_context.mojom.h"

namespace network {

// Applies an Environment's host mapping before Chromium's configured proxy
// service. Requests that do not match are delegated without modification.
// The wrapper owns, rather than replaces, Chromium's existing resolver and its
// ProxyDelegate.
class COMPONENT_EXPORT(NETWORK_SERVICE) HostOverrideProxyResolutionService final
    : public net::ProxyResolutionService {
 public:
  HostOverrideProxyResolutionService(
      std::unique_ptr<net::ProxyResolutionService> proxy_resolution_service,
      std::string host_mapping_rules,
      mojom::HostOverridePolicy policy,
      std::string policy_reason,
      uint64_t revision,
      uint64_t generation);

  HostOverrideProxyResolutionService(
      const HostOverrideProxyResolutionService&) = delete;
  HostOverrideProxyResolutionService& operator=(
      const HostOverrideProxyResolutionService&) = delete;

  ~HostOverrideProxyResolutionService() override;

  // Replaces the mapping snapshot synchronously on the NetworkContext sequence.
  // Returns false without changing state if `rules` cannot be parsed.
  bool SetHostMappingRules(std::string_view rules, uint64_t revision);
  static bool ValidateHostMappingRules(std::string_view rules);

  void SetPolicy(mojom::HostOverridePolicy policy, std::string reason);
  uint64_t applied_revision() const { return revision_; }
  uint64_t generation() const { return generation_; }
  void set_generation(uint64_t generation) { generation_ = generation; }

  int ResolveProxy(
      const GURL& url,
      const std::string& method,
      const net::NetworkAnonymizationKey& network_anonymization_key,
      net::handles::NetworkHandle target_network,
      net::ProxyInfo* results,
      net::CompletionOnceCallback callback,
      std::unique_ptr<net::ProxyResolutionRequest>* request,
      const net::NetLogWithSource& net_log,
      net::RequestPriority priority) override;
  void ReportSuccess(const net::ProxyInfo& proxy_info) override;
  void SetProxyDelegate(net::ProxyDelegate* delegate) override;
  void OnShutdown() override;
  void ClearBadProxiesCache() override;
  const net::ProxyRetryInfoMap& proxy_retry_info() const override;
  base::DictValue GetProxyNetLogValues() override;
  [[nodiscard]] bool CastToConfiguredProxyResolutionService(
      net::ConfiguredProxyResolutionService**
          configured_proxy_resolution_service) override;

 private:
  static bool ParseRules(std::string_view rules,
                         net::HostMappingRules* parsed_rules);

  bool IsMappedUrl(const GURL& url) const;

  std::unique_ptr<net::ProxyResolutionService> proxy_resolution_service_;
  net::HostMappingRules host_mapping_rules_;
  mojom::HostOverridePolicy policy_;
  std::string policy_reason_;
  uint64_t revision_ = 0;
  uint64_t generation_ = 0;

  SEQUENCE_CHECKER(sequence_checker_);
};

}  // namespace network

#endif  // SERVICES_NETWORK_HOST_OVERRIDE_PROXY_RESOLUTION_SERVICE_H_
