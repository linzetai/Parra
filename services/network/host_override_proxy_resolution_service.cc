// Copyright 2026 The Parra Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "services/network/host_override_proxy_resolution_service.h"

#include <utility>
#include <vector>

#include "base/logging.h"
#include "base/strings/string_number_conversions.h"
#include "base/strings/string_split.h"
#include "net/base/host_port_pair.h"
#include "net/base/net_errors.h"
#include "net/base/proxy_delegate.h"
#include "net/log/net_log_with_source.h"
#include "net/proxy_resolution/configured_proxy_resolution_service.h"
#include "net/proxy_resolution/proxy_info.h"
#include "url/gurl.h"

namespace network {

HostOverrideProxyResolutionService::HostOverrideProxyResolutionService(
    std::unique_ptr<net::ProxyResolutionService> proxy_resolution_service,
    std::string host_mapping_rules,
    mojom::HostOverridePolicy policy,
    std::string policy_reason,
    uint64_t revision,
    uint64_t generation)
    : proxy_resolution_service_(std::move(proxy_resolution_service)),
      policy_(policy),
      policy_reason_(std::move(policy_reason)),
      revision_(revision),
      generation_(generation) {
  CHECK(proxy_resolution_service_);
  CHECK(SetHostMappingRules(host_mapping_rules, revision));
}

HostOverrideProxyResolutionService::~HostOverrideProxyResolutionService() =
    default;

bool HostOverrideProxyResolutionService::SetHostMappingRules(
    std::string_view rules,
    uint64_t revision) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  if (revision < revision_) {
    LOG(WARNING) << "parra-host-override: ignored stale rules revision "
                 << revision << " < " << revision_;
    return false;
  }
  net::HostMappingRules parsed_rules;
  if (!ParseRules(rules, &parsed_rules)) {
    return false;
  }
  host_mapping_rules_ = parsed_rules;
  revision_ = revision;
  LOG(INFO) << "parra-host-override: installed resolver-equivalent rules; "
            << "revision=" << revision_ << " mappings='" << rules << "'";
  return true;
}

bool HostOverrideProxyResolutionService::ValidateHostMappingRules(
    std::string_view rules) {
  net::HostMappingRules parsed_rules;
  return ParseRules(rules, &parsed_rules);
}

void HostOverrideProxyResolutionService::SetPolicy(
    mojom::HostOverridePolicy policy,
    std::string reason) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  policy_ = policy;
  policy_reason_ = std::move(reason);
  LOG(INFO) << "parra-host-override: policy updated to "
            << static_cast<int>(policy_) << " reason='" << policy_reason_
            << "' revision=" << revision_;
}

int HostOverrideProxyResolutionService::ResolveProxy(
    const GURL& url,
    const std::string& method,
    const net::NetworkAnonymizationKey& network_anonymization_key,
    net::handles::NetworkHandle target_network,
    net::ProxyInfo* results,
    net::CompletionOnceCallback callback,
    std::unique_ptr<net::ProxyResolutionRequest>* request,
    const net::NetLogWithSource& net_log,
    net::RequestPriority priority) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);

  if (!IsMappedUrl(url)) {
    return proxy_resolution_service_->ResolveProxy(
        url, method, network_anonymization_key, target_network, results,
        std::move(callback), request, net_log, priority);
  }

  request->reset();
  if (policy_ == mojom::HostOverridePolicy::kAllowed) {
    results->UseDirect();
    LOG(INFO) << "parra-host-override: DIRECT selected for " << url.host()
              << " revision=" << revision_ << " generation=" << generation_;
    return net::OK;
  }

  LOG(ERROR) << "parra-host-override: DIRECT refused for " << url.host()
             << " policy=" << static_cast<int>(policy_) << " reason='"
             << policy_reason_ << "' revision=" << revision_
             << " generation=" << generation_;
  if (policy_ == mojom::HostOverridePolicy::kMandatoryProxy) {
    return net::ERR_MANDATORY_PROXY_CONFIGURATION_FAILED;
  }
  return net::ERR_BLOCKED_BY_ADMINISTRATOR;
}

void HostOverrideProxyResolutionService::ReportSuccess(
    const net::ProxyInfo& proxy_info) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  proxy_resolution_service_->ReportSuccess(proxy_info);
}

void HostOverrideProxyResolutionService::SetProxyDelegate(
    net::ProxyDelegate* delegate) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  proxy_resolution_service_->SetProxyDelegate(delegate);
}

void HostOverrideProxyResolutionService::OnShutdown() {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  proxy_resolution_service_->OnShutdown();
}

void HostOverrideProxyResolutionService::ClearBadProxiesCache() {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  proxy_resolution_service_->ClearBadProxiesCache();
}

const net::ProxyRetryInfoMap&
HostOverrideProxyResolutionService::proxy_retry_info() const {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  return proxy_resolution_service_->proxy_retry_info();
}

base::DictValue HostOverrideProxyResolutionService::GetProxyNetLogValues() {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  base::DictValue values = proxy_resolution_service_->GetProxyNetLogValues();
  values.Set("parra_host_override_revision", base::NumberToString(revision_));
  values.Set("parra_host_override_generation",
             base::NumberToString(generation_));
  values.Set("parra_host_override_policy", static_cast<int>(policy_));
  return values;
}

bool HostOverrideProxyResolutionService::CastToConfiguredProxyResolutionService(
    net::ConfiguredProxyResolutionService**
        configured_proxy_resolution_service) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  return proxy_resolution_service_->CastToConfiguredProxyResolutionService(
      configured_proxy_resolution_service);
}

bool HostOverrideProxyResolutionService::ParseRules(
    std::string_view rules,
    net::HostMappingRules* parsed_rules) {
  parsed_rules->SetRulesFromString("");
  if (rules.empty()) {
    return true;
  }
  for (std::string_view rule : base::SplitStringPiece(
           rules, ",", base::TRIM_WHITESPACE, base::SPLIT_WANT_NONEMPTY)) {
    if (!parsed_rules->AddRuleFromString(rule)) {
      return false;
    }
  }
  return true;
}

bool HostOverrideProxyResolutionService::IsMappedUrl(const GURL& url) const {
  if (!url.is_valid() || !url.IsStandard() || !url.has_host()) {
    return false;
  }
  net::HostPortPair host_port = net::HostPortPair::FromURL(url);
  return host_mapping_rules_.RewriteHost(&host_port);
}

}  // namespace network
