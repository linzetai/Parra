// Copyright 2026 The Parra Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "services/network/host_override_proxy_resolution_service.h"

#include <memory>
#include <string>

#include "base/functional/callback.h"
#include "net/base/net_errors.h"
#include "net/base/network_anonymization_key.h"
#include "net/log/net_log_with_source.h"
#include "net/proxy_resolution/proxy_info.h"
#include "net/proxy_resolution/proxy_resolution_request.h"
#include "testing/gtest/include/gtest/gtest.h"
#include "url/gurl.h"

namespace network {
namespace {

class CountingProxyResolutionService final
    : public net::ProxyResolutionService {
 public:
  int ResolveProxy(
      const GURL& url,
      const std::string& method,
      const net::NetworkAnonymizationKey& network_anonymization_key,
      net::handles::NetworkHandle target_network,
      net::ProxyInfo* results,
      net::CompletionOnceCallback callback,
      std::unique_ptr<net::ProxyResolutionRequest>* request,
      const net::NetLogWithSource& net_log,
      net::RequestPriority priority) override {
    ++resolve_calls;
    results->UseNamedProxy("http://127.0.0.1:1");
    return net::OK;
  }

  void ReportSuccess(const net::ProxyInfo& proxy_info) override {}
  void SetProxyDelegate(net::ProxyDelegate* delegate) override {}
  void OnShutdown() override {}
  void ClearBadProxiesCache() override {}
  const net::ProxyRetryInfoMap& proxy_retry_info() const override {
    return retry_info_;
  }
  base::DictValue GetProxyNetLogValues() override { return {}; }
  bool CastToConfiguredProxyResolutionService(
      net::ConfiguredProxyResolutionService** service) override {
    *service = nullptr;
    return false;
  }

  int resolve_calls = 0;

 private:
  net::ProxyRetryInfoMap retry_info_;
};

int Resolve(HostOverrideProxyResolutionService* service,
            const GURL& url,
            net::ProxyInfo* result) {
  std::unique_ptr<net::ProxyResolutionRequest> request;
  return service->ResolveProxy(url, "GET", net::NetworkAnonymizationKey(),
                               net::handles::kInvalidNetworkHandle, result,
                               net::CompletionOnceCallback(), &request,
                               net::NetLogWithSource(), net::DEFAULT_PRIORITY);
}

TEST(HostOverrideProxyResolutionServiceTest, MappedUrlUsesDirectBeforeProxy) {
  auto inner = std::make_unique<CountingProxyResolutionService>();
  auto* inner_ptr = inner.get();
  HostOverrideProxyResolutionService service(
      std::move(inner),
      "map kdocs.cn 10.13.34.11, map www.kdocs.cn 10.13.34.11",
      mojom::HostOverridePolicy::kAllowed, "", 7, 42);

  net::ProxyInfo result;
  EXPECT_EQ(net::OK, Resolve(&service, GURL("https://www.kdocs.cn/index.html"),
                             &result));
  EXPECT_TRUE(result.is_direct());
  EXPECT_EQ(0, inner_ptr->resolve_calls);
  EXPECT_EQ(7u, service.applied_revision());
  EXPECT_EQ(42u, service.generation());
}

TEST(HostOverrideProxyResolutionServiceTest, SameHostSnapshots) {
  auto proxy_a = std::make_unique<CountingProxyResolutionService>();
  auto* proxy_a_ptr = proxy_a.get();
  auto proxy_b = std::make_unique<CountingProxyResolutionService>();
  auto* proxy_b_ptr = proxy_b.get();
  HostOverrideProxyResolutionService environment_a(
      std::move(proxy_a), "map sheet.test 127.0.0.1:8443",
      mojom::HostOverridePolicy::kAllowed, "", 4, 11);
  HostOverrideProxyResolutionService environment_b(
      std::move(proxy_b), "map sheet.test 127.0.0.2:8443",
      mojom::HostOverridePolicy::kAllowed, "", 9, 12);

  net::ProxyInfo result_a;
  net::ProxyInfo result_b;
  const GURL same_url("https://sheet.test:8443/");
  EXPECT_EQ(net::OK, Resolve(&environment_a, same_url, &result_a));
  EXPECT_EQ(net::OK, Resolve(&environment_b, same_url, &result_b));
  EXPECT_TRUE(result_a.is_direct());
  EXPECT_TRUE(result_b.is_direct());
  EXPECT_EQ(0, proxy_a_ptr->resolve_calls);
  EXPECT_EQ(0, proxy_b_ptr->resolve_calls);
  EXPECT_EQ(4u, environment_a.applied_revision());
  EXPECT_EQ(9u, environment_b.applied_revision());
  EXPECT_EQ(11u, environment_a.generation());
  EXPECT_EQ(12u, environment_b.generation());
}

TEST(HostOverrideProxyResolutionServiceTest,
     UnmappedAndExcludedUrlsUseOriginalProxyResolver) {
  auto inner = std::make_unique<CountingProxyResolutionService>();
  auto* inner_ptr = inner.get();
  HostOverrideProxyResolutionService service(
      std::move(inner), "map *.kdocs.cn 10.13.34.11, exclude excluded.kdocs.cn",
      mojom::HostOverridePolicy::kAllowed, "", 1, 1);

  net::ProxyInfo result;
  const GURL url("https://example.test/");
  const int status = Resolve(&service, url, &result);
  EXPECT_EQ(net::OK, status);
  EXPECT_FALSE(result.is_direct());
  EXPECT_EQ(1, inner_ptr->resolve_calls);

  net::ProxyInfo excluded_result;
  EXPECT_EQ(net::OK, Resolve(&service, GURL("https://excluded.kdocs.cn/"),
                             &excluded_result));
  EXPECT_FALSE(excluded_result.is_direct());
  EXPECT_EQ(2, inner_ptr->resolve_calls);
}

TEST(HostOverrideProxyResolutionServiceTest, PolicyConflictFailsClosed) {
  auto inner = std::make_unique<CountingProxyResolutionService>();
  auto* inner_ptr = inner.get();
  HostOverrideProxyResolutionService service(
      std::move(inner), "map kdocs.cn 10.13.34.11",
      mojom::HostOverridePolicy::kBlockedByPolicy, "managed proxy policy", 3,
      5);

  net::ProxyInfo result;
  EXPECT_EQ(net::ERR_BLOCKED_BY_ADMINISTRATOR,
            Resolve(&service, GURL("https://kdocs.cn/index.html"), &result));
  EXPECT_EQ(0, inner_ptr->resolve_calls);
}

TEST(HostOverrideProxyResolutionServiceTest,
     ResolverFailureMappingDoesNotFallBackToProxy) {
  auto inner = std::make_unique<CountingProxyResolutionService>();
  auto* inner_ptr = inner.get();
  HostOverrideProxyResolutionService service(
      std::move(inner), "map rejected.test ^NOTFOUND",
      mojom::HostOverridePolicy::kAllowed, "", 4, 7);

  net::ProxyInfo result;
  EXPECT_EQ(net::OK,
            Resolve(&service, GURL("https://rejected.test/"), &result));
  EXPECT_TRUE(result.is_direct());
  EXPECT_EQ(0, inner_ptr->resolve_calls);
}

TEST(HostOverrideProxyResolutionServiceTest, MandatoryPacFailsClosed) {
  auto inner = std::make_unique<CountingProxyResolutionService>();
  auto* inner_ptr = inner.get();
  HostOverrideProxyResolutionService service(
      std::move(inner), "map kdocs.cn 10.13.34.11",
      mojom::HostOverridePolicy::kMandatoryProxy, "mandatory PAC", 3, 5);

  net::ProxyInfo result;
  EXPECT_EQ(net::ERR_MANDATORY_PROXY_CONFIGURATION_FAILED,
            Resolve(&service, GURL("https://kdocs.cn/index.html"), &result));
  EXPECT_EQ(0, inner_ptr->resolve_calls);
}

TEST(HostOverrideProxyResolutionServiceTest,
     RuntimePolicyUpdateBlocksMappedRequestsWithoutProxyFallback) {
  auto inner = std::make_unique<CountingProxyResolutionService>();
  auto* inner_ptr = inner.get();
  HostOverrideProxyResolutionService service(
      std::move(inner), "map policy.test 10.0.0.1",
      mojom::HostOverridePolicy::kAllowed, "", 2, 8);

  service.SetPolicy(mojom::HostOverridePolicy::kBlockedByPolicy,
                    "ProxySettings policy changed");
  net::ProxyInfo mapped_result;
  EXPECT_EQ(net::ERR_BLOCKED_BY_ADMINISTRATOR,
            Resolve(&service, GURL("https://policy.test/"), &mapped_result));
  EXPECT_EQ(0, inner_ptr->resolve_calls);

  net::ProxyInfo unmapped_result;
  EXPECT_EQ(net::OK, Resolve(&service, GURL("https://not-mapped.test/"),
                             &unmapped_result));
  EXPECT_FALSE(unmapped_result.is_direct());
  EXPECT_EQ(1, inner_ptr->resolve_calls);
}

TEST(HostOverrideProxyResolutionServiceTest, LiveUpdatesAreRevisionGuarded) {
  auto inner = std::make_unique<CountingProxyResolutionService>();
  HostOverrideProxyResolutionService service(
      std::move(inner), "map a.test 10.0.0.1",
      mojom::HostOverridePolicy::kAllowed, "", 1, 9);

  EXPECT_TRUE(service.SetHostMappingRules("map b.test 10.0.0.2", 2));
  EXPECT_FALSE(service.SetHostMappingRules("map c.test 10.0.0.3", 1));

  net::ProxyInfo old_host_result;
  EXPECT_EQ(net::OK,
            Resolve(&service, GURL("https://a.test/"), &old_host_result));
  EXPECT_FALSE(old_host_result.is_direct());

  net::ProxyInfo new_host_result;
  EXPECT_EQ(net::OK,
            Resolve(&service, GURL("https://b.test/"), &new_host_result));
  EXPECT_TRUE(new_host_result.is_direct());
  EXPECT_EQ(2u, service.applied_revision());
}

}  // namespace
}  // namespace network
