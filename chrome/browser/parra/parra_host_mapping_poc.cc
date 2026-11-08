// Copyright 2026 The Parra Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// Parra P3.4 PoC-1: can a LIVE NetworkContext take new host mapping rules?
//
// THE QUESTION (docs/parra/77 搂B1, 78 搂2)
// ---------------------------------------
// Mapping rules are supplied only as NetworkContext CREATION params, and
// MappedHostResolver -- which owns them -- is inserted only when the initial
// rule string is non-empty (net/dns/host_resolver.cc:425). Two things must
// therefore be established before scheme A can be accepted:
//
//   1. Can an ALREADY CREATED context be updated, without destroying its
//      StoragePartition and without restarting the browser?
//   2. What happens for a context created with EMPTY rules and no mapping
//   layer?
//
// WHY THE ANSWER MUST COME FROM A RUN, NOT FROM READING
// ------------------------------------------------------
// A function called SetHostMappingRules() that returns void (or that is
// silently dropped by a wrapper) would look identical to a working one from the
// call site. The only trustworthy evidence is: change the rules, then observe
// which backend actually served a fresh request. So every step below asserts on
// an OBSERVED RESPONSE BODY, never on "the call returned".
//
// WHAT THIS DOES NOT PROVE
// ------------------------
// It does not prove the DNS cache/connection-pool semantics (that is PoC-2),
// and it does not prove behaviour through a proxy (that is PoC-3).

#include "chrome/browser/parra/parra_host_mapping_poc.h"

#include <memory>
#include <string>
#include <utility>

#include "base/command_line.h"
#include "base/functional/bind.h"
#include "base/functional/callback_helpers.h"
#include "base/location.h"
#include "base/logging.h"
#include "base/strings/string_number_conversions.h"
#include "base/strings/string_split.h"
#include "base/task/single_thread_task_runner.h"
#include "base/time/time.h"
#include "chrome/browser/parra/parra_switches.h"
#include "chrome/browser/profiles/profile.h"
#include "chrome/browser/ui/browser_window/public/browser_window_interface.h"
#include "chrome/browser/ui/browser_window/public/browser_window_interface_iterator.h"
#include "content/public/browser/browser_context.h"
#include "content/public/browser/storage_partition.h"
#include "content/public/browser/storage_partition_config.h"
#include "net/base/load_flags.h"
#include "net/traffic_annotation/network_traffic_annotation_test_helper.h"
#include "services/network/public/cpp/resource_request.h"
#include "services/network/public/cpp/simple_url_loader.h"
#include "services/network/public/mojom/clear_data_filter.mojom.h"
#include "services/network/public/mojom/network_context.mojom.h"
#include "url/gurl.h"

namespace parra {

namespace {

int g_poc_checks = 0;
int g_poc_failures = 0;

void PocReport(bool ok, const std::string& test, const std::string& detail) {
  ++g_poc_checks;
  if (!ok) {
    ++g_poc_failures;
  }
  LOG(INFO) << "PARRA-POC2 " << (ok ? "PASS" : "FAIL") << " " << test << " | "
            << detail;
}

// Extracts the environment marker from a response body.
//
// WHY THE BODY IS THE EVIDENCE: "which backend answered" is a fact about the
// bytes that came back. A configuration value, an applied=true flag or a log
// line describing intent can all be right while the request still reaches the
// OLD backend -- that discrepancy is exactly what connection reuse causes. Only
// the body distinguishes ENV-A from ENV-B from ENV-C.
//
// Returns "ENV-A"/"ENV-B"/"ENV-C", or an empty string when no marker is present
// (a truncated or non-test response).
std::string EnvironmentMarkerIn(const std::string& body) {
  for (const char* marker : {"ENV-A", "ENV-B", "ENV-C"}) {
    if (body.find(marker) != std::string::npos) {
      return marker;
    }
  }
  return std::string();
}

// Issues one request through the given StoragePartition and reports the body.
//
// WHY THE FACTORY COMES FROM THE PARTITION, NOT FROM THE NetworkContext: a
// NetworkContext exposes no URLLoaderFactory accessor, and going through the
// partition is what a real page does anyway -- the request therefore exercises
// the partition's own NetworkContext, which is the object whose rules are being
// changed.
//
// `load_flags` lets a caller opt into or out of the HTTP cache. Tests that ask
// "does a NEW request reach the new backend" want LOAD_DISABLE_CACHE so the
// answer is not a cached body from the previous backend; tests about reuse want
// the default.
void FetchThroughPartition(
    content::StoragePartition* partition,
    const GURL& url,
    int load_flags,
    base::OnceCallback<void(bool ok, std::string body, std::string error)>
        callback) {
  if (!partition) {
    std::move(callback).Run(false, std::string(), "no StoragePartition");
    return;
  }
  auto request = std::make_unique<network::ResourceRequest>();
  request->url = url;
  request->method = "GET";
  // No credentials: the test backends serve everyone, and omitting credentials
  // keeps the request from touching the cookie jar in ways that could mask a
  // routing change.
  request->credentials_mode = network::mojom::CredentialsMode::kOmit;
  request->load_flags = load_flags;

  auto loader = network::SimpleURLLoader::Create(std::move(request),
                                                 TRAFFIC_ANNOTATION_FOR_TESTS);
  loader->SetTimeoutDuration(base::Seconds(10));

  // The loader must outlive the download, so it is held in a shared_ptr that
  // the completion callback keeps alive.
  auto holder = std::make_shared<std::unique_ptr<network::SimpleURLLoader>>(
      std::move(loader));
  (*holder)->DownloadToString(
      partition->GetURLLoaderFactoryForBrowserProcess().get(),
      base::BindOnce(
          [](std::shared_ptr<std::unique_ptr<network::SimpleURLLoader>> keep,
             base::OnceCallback<void(bool, std::string, std::string)> cb,
             std::optional<std::string> body) {
            if (!body) {
              std::move(cb).Run(false, std::string(), "download failed");
              return;
            }
            std::move(cb).Run(true, *body, std::string());
          },
          std::move(holder), std::move(callback)),
      64 * 1024);
}

// Applies rules and reports whether the NetworkContext says it took effect.
void ApplyRules(
    network::mojom::NetworkContext* context,
    const std::string& rules,
    base::OnceCallback<void(bool applied, std::string error)> callback) {
  if (!context) {
    std::move(callback).Run(false, "no NetworkContext");
    return;
  }
  static uint64_t next_revision = 1;
  const uint64_t revision = next_revision++;
  context->SetHostMappingRules(
      rules, revision,
      base::BindOnce(
          [](base::OnceCallback<void(bool, std::string)> cb, bool applied,
             const std::string& error, uint64_t applied_revision,
             uint64_t network_context_generation,
             network::mojom::HostOverridePolicy policy,
             const std::string& policy_reason) {
            std::move(cb).Run(applied, error);
          },
          std::move(callback)));
}

// Runs the T1/T2 matrix, then reports.
//
// Written as an explicit state machine rather than a nest of callbacks so the
// ORDER of steps is visible: create -> observe -> update -> observe. Every
// observation is a real HTTP response whose body names the backend that served
// it; an applied=true flag alone is never accepted as evidence, because it says
// what the NetworkContext intends, not where the bytes actually came from.
class HostMappingPocRunner {
 public:
  static void Start(content::BrowserContext* browser_context) {
    (new HostMappingPocRunner(browser_context))
        ->StepCreateEmptyRulesPartition();
  }

 private:
  explicit HostMappingPocRunner(content::BrowserContext* browser_context)
      : browser_context_(browser_context) {}

  content::StoragePartition* PartitionFor(
      const content::StoragePartitionConfig& config) {
    return browser_context_->GetStoragePartition(config, /*can_create=*/true);
  }

  // Fetches through a partition and reports which backend answered.
  void FetchAndReport(content::StoragePartition* partition,
                      const std::string& label,
                      int load_flags,
                      base::OnceCallback<void(std::string marker)> callback) {
    FetchThroughPartition(
        partition, GURL("https://sheet.test:8443/poc"), load_flags,
        base::BindOnce(
            [](std::string label, base::OnceCallback<void(std::string)> cb,
               bool ok, std::string body, std::string error) {
              if (!ok) {
                LOG(INFO) << "PARRA-POC2 OBSERVE " << label
                          << " marker=<failed> error='" << error << "'";
                std::move(cb).Run(std::string());
                return;
              }
              const std::string marker = EnvironmentMarkerIn(body);
              LOG(INFO) << "PARRA-POC2 OBSERVE " << label << " marker='"
                        << (marker.empty() ? "<none>" : marker) << "'";
              std::move(cb).Run(marker);
            },
            label, std::move(callback)));
  }

  // ---------------------------------------------------------------------
  // T1: a partition created with NO rules must be able to take rules and then
  // actually reach the mapped backend.
  // ---------------------------------------------------------------------
  void StepCreateEmptyRulesPartition() {
    // "parra/poc2-empty" has no environment definition, so
    // GetHostMappingRulesForPartitionPath() returns an empty string and the
    // NetworkContext is created with an empty rule set. Before A1 that meant no
    // mapping layer existed and no update was possible.
    const content::StoragePartitionConfig config =
        content::StoragePartitionConfig::Create(
            browser_context_, /*partition_domain=*/"parra",
            /*partition_name=*/"poc2-empty", /*in_memory=*/false);
    empty_partition_ = PartitionFor(config);
    empty_context_ = empty_partition_->GetNetworkContext();
    if (!empty_partition_ || !empty_context_) {
      PocReport(false, "T1-create-context",
                "could not create the empty-rules partition");
      StepCrossEnvironmentBaseline();
      return;
    }
    PocReport(true, "T1-create-context",
              "partition='parra/poc2-empty' created with no environment "
              "definition, so its rule set started empty");

    // Prove the starting state: with no rules the test host cannot resolve, so
    // the request must FAIL. A success here would mean rules were already in
    // effect and the rest of T1 would prove nothing.
    FetchAndReport(
        empty_partition_, "T1-before-update", net::LOAD_DISABLE_CACHE,
        base::BindOnce(
            [](HostMappingPocRunner* self, std::string marker) {
              PocReport(marker.empty(), "T1-before-update",
                        "before any rules, the request did not reach a backend "
                        "(marker='" +
                            marker + "')");
              self->StepApplyRulesToEmpty();
            },
            this));
  }

  void StepApplyRulesToEmpty() {
    // Point it at ENV-A explicitly. 8443 is the ENV-A backend.
    const std::string rules = "map sheet.test 127.0.0.1:8443";
    ApplyRules(
        empty_context_, rules,
        base::BindOnce(
            [](HostMappingPocRunner* self, bool applied, std::string error) {
              PocReport(applied, "T1-apply-applied",
                        "applied=" + std::string(applied ? "true" : "false") +
                            " rules='map sheet.test 127.0.0.1:8443'"
                            " error=" +
                            error);
              if (!applied) {
                // No point continuing: the whole point of T1 is that the
                // update takes effect on a context created empty.
                self->StepCrossEnvironmentBaseline();
                return;
              }
              self->StepFetchAfterEmptyUpdate();
            },
            this));
  }

  void StepFetchAfterEmptyUpdate() {
    // Clear only the DNS cache; deliberately NOT the connection pool, so the
    // check also reveals whether a stale connection could serve the old
    // backend.
    empty_context_->ClearHostCache(nullptr, base::RepeatingClosure());
    FetchAndReport(empty_partition_, "T1-after-update", net::LOAD_DISABLE_CACHE,
                   base::BindOnce(
                       [](HostMappingPocRunner* self, std::string marker) {
                         // ENV-A is 127.0.0.1:8443. The response body must say
                         // ENV-A.
                         PocReport(marker == "ENV-A", "T1-after-update",
                                   "expected ENV-A, got '" +
                                       (marker.empty() ? "<failed>" : marker) +
                                       "'");
                         self->StepCrossEnvironmentBaseline();
                       },
                       this));
  }

  // ---------------------------------------------------------------------
  // T2: updating test-a must not disturb test-b, and both must be identified by
  // their own backend before AND after.
  // ---------------------------------------------------------------------
  void StepCrossEnvironmentBaseline() {
    const content::StoragePartitionConfig a =
        content::StoragePartitionConfig::Create(
            browser_context_, /*partition_domain=*/"parra",
            /*partition_name=*/"test-a", /*in_memory=*/false);
    const content::StoragePartitionConfig b =
        content::StoragePartitionConfig::Create(
            browser_context_, /*partition_domain=*/"parra",
            /*partition_name=*/"test-b", /*in_memory=*/false);
    test_a_partition_ = PartitionFor(a);
    test_b_partition_ = PartitionFor(b);
    test_a_context_ =
        test_a_partition_ ? test_a_partition_->GetNetworkContext() : nullptr;
    if (!test_a_partition_ || !test_b_partition_ || !test_a_context_) {
      PocReport(false, "T2-create-contexts", "could not create test-a/test-b");
      Finish();
      return;
    }

    // Baseline: test-a -> ENV-A (127.0.0.1:8443), test-b -> ENV-B
    // (127.0.0.2:8444). Set explicitly rather than relying on whatever the
    // command line configured, so the assertion does not depend on external
    // flags.
    ApplyRules(
        test_a_context_, "map sheet.test 127.0.0.1:8443",
        base::BindOnce(
            [](HostMappingPocRunner* self, bool applied, std::string error) {
              if (!applied) {
                PocReport(false, "T2-setup-test-a",
                          "applied=false error=" + error);
                self->Finish();
                return;
              }
              self->test_b_context_ =
                  self->test_b_partition_->GetNetworkContext();
              ApplyRules(self->test_b_context_, "map sheet.test 127.0.0.2:8444",
                         base::BindOnce(
                             [](HostMappingPocRunner* s, bool b_applied,
                                std::string b_error) {
                               if (!b_applied) {
                                 PocReport(false, "T2-setup-test-b",
                                           "applied=false error=" + b_error);
                                 s->Finish();
                                 return;
                               }
                               s->StepCrossEnvironmentBefore();
                             },
                             self));
            },
            this));
  }

  void StepCrossEnvironmentBefore() {
    test_a_context_->ClearHostCache(nullptr, base::RepeatingClosure());
    test_b_context_->ClearHostCache(nullptr, base::RepeatingClosure());
    FetchAndReport(
        test_a_partition_, "T2-test-a-before", net::LOAD_DISABLE_CACHE,
        base::BindOnce(
            [](HostMappingPocRunner* self, std::string a_marker) {
              self->test_a_before_ = a_marker;
              PocReport(a_marker == "ENV-A", "T2-test-a-before",
                        "expected ENV-A, got '" +
                            (a_marker.empty() ? "<failed>" : a_marker) + "'");
              self->StepCrossEnvironmentBeforeB();
            },
            this));
  }

  void StepCrossEnvironmentBeforeB() {
    FetchAndReport(
        test_b_partition_, "T2-test-b-before", net::LOAD_DISABLE_CACHE,
        base::BindOnce(
            [](HostMappingPocRunner* self, std::string b_marker) {
              self->test_b_before_ = b_marker;
              PocReport(b_marker == "ENV-B", "T2-test-b-before",
                        "expected ENV-B, got '" +
                            (b_marker.empty() ? "<failed>" : b_marker) + "'");
              self->StepRepointTestA();
            },
            this));
  }

  void StepRepointTestA() {
    // test-a: ENV-A -> ENV-C (127.0.0.3:8445). test-b is untouched.
    ApplyRules(
        test_a_context_, "map sheet.test 127.0.0.3:8445",
        base::BindOnce(
            [](HostMappingPocRunner* self, bool applied, std::string error) {
              PocReport(applied, "T2-repoint-test-a",
                        "applied=" + std::string(applied ? "true" : "false") +
                            " error=" + error);
              self->StepCrossEnvironmentAfter();
            },
            this));
  }

  void StepCrossEnvironmentAfter() {
    // THREE observations, deliberately kept separate, because they measure
    // three different things and conflating them is how "rules applied" gets
    // mistaken for "traffic moved".
    //
    //   (a) cache cleared, connections left alone
    //       -> shows whether a reused keep-alive connection can still serve the
    //       OLD
    //          backend after the rule changed.
    //   (b) + idle connections closed
    //       -> shows whether dropping the pool is what actually forces the new
    //          backend.
    //
    // This ordering is the whole point of PoC-2. The first run of T2 observed
    // ENV-A after applied=true, which is exactly the (a) case: the new rules
    // were in effect for NEW lookups, but the existing connection answered.
    test_a_context_->ClearHostCache(nullptr, base::RepeatingClosure());
    FetchAndReport(
        test_a_partition_, "T2-test-a-after-clearcache-only",
        net::LOAD_DISABLE_CACHE,
        base::BindOnce(
            [](HostMappingPocRunner* self, std::string a_marker) {
              self->test_a_cache_only_ = a_marker;
              LOG(INFO)
                  << "PARRA-POC2 NOTE after updating rules and clearing only "
                     "the DNS cache, test-a answered '"
                  << (a_marker.empty() ? "<failed>" : a_marker) << "'";
              self->StepCrossEnvironmentAfterCloseIdle();
            },
            this));
  }

  void StepCrossEnvironmentAfterCloseIdle() {
    test_a_context_->CloseIdleConnections(base::RepeatingClosure());
    FetchAndReport(
        test_a_partition_, "T2-test-a-after-closeidle", net::LOAD_DISABLE_CACHE,
        base::BindOnce(
            [](HostMappingPocRunner* self, std::string a_marker) {
              self->test_a_after_ = a_marker;
              // The assertion is on THIS step: once idle connections are
              // dropped, a new request must reach the newly mapped backend.
              PocReport(a_marker == "ENV-C", "T2-test-a-after-closeidle",
                        "expected ENV-C after CloseIdleConnections, got '" +
                            (a_marker.empty() ? "<failed>" : a_marker) + "'");
              self->StepCrossEnvironmentAfterB();
            },
            this));
  }

  void StepCrossEnvironmentAfterB() {
    FetchAndReport(
        test_b_partition_, "T2-test-b-after", net::LOAD_DISABLE_CACHE,
        base::BindOnce(
            [](HostMappingPocRunner* self, std::string b_marker) {
              self->test_b_after_ = b_marker;
              // The decisive isolation check: test-b must STILL be ENV-B after
              // test-a was repointed to ENV-C. "test-b still responds" would
              // pass even if it had silently moved to ENV-C.
              PocReport(b_marker == "ENV-B", "T2-test-b-after",
                        "expected ENV-B (unchanged), got '" +
                            (b_marker.empty() ? "<failed>" : b_marker) + "'");
              self->StepHttp2Baseline();
            },
            this));
  }

  // ---------------------------------------------------------------------
  // Scenario C: HTTP/2 session reuse.
  //
  // WHY THIS NEEDS ITS OWN BACKEND: an h2 session carries many streams over ONE
  // TCP connection. A request that reuses it performs no DNS lookup, so a rule
  // change cannot affect it -- a stronger form of the reuse seen with HTTP/1.1
  // keep-alive. Measuring that needs a server that actually negotiates h2
  // (h2server.py); substituting the HTTP/1.1 backend would measure a different
  // protocol, which docs/parra/87 forbids.
  //
  // Broken into small named steps rather than nested lambdas: the earlier draft
  // nested four callbacks deep and captured the wrong arguments, which is easy
  // to do and hard to see.
  void StepHttp2Baseline() {
    h2_partition_ = PartitionFor(content::StoragePartitionConfig::Create(
        browser_context_, /*partition_domain=*/"parra",
        /*partition_name=*/"test-a", /*in_memory=*/false));
    h2_context_ = h2_partition_ ? h2_partition_->GetNetworkContext() : nullptr;
    if (!h2_partition_ || !h2_context_) {
      PocReport(false, "H2-setup", "no partition/context for the h2 scenario");
      Finish();
      return;
    }
    ApplyRules(h2_context_, "map sheet.test 127.0.0.1:9543",
               base::BindOnce(&HostMappingPocRunner::OnHttp2MappedToA,
                              base::Unretained(this)));
  }

  void OnHttp2MappedToA(bool applied, std::string error) {
    PocReport(applied, "H2-map-to-A",
              "applied=" + std::string(applied ? "true" : "false") +
                  " rules='map sheet.test 127.0.0.1:9543'");
    if (!applied) {
      Finish();
      return;
    }
    h2_context_->ClearHostCache(nullptr, base::RepeatingClosure());
    FetchThroughPartition(
        h2_partition_, GURL("https://sheet.test:9543/poc"),
        net::LOAD_DISABLE_CACHE,
        base::BindOnce(&HostMappingPocRunner::OnHttp2BaselineFetched,
                       base::Unretained(this)));
  }

  void OnHttp2BaselineFetched(bool ok, std::string body, std::string error) {
    const std::string marker = ok ? EnvironmentMarkerIn(body) : std::string();
    PocReport(marker == "ENV-A", "H2-baseline",
              "expected ENV-A over h2, got '" +
                  (marker.empty() ? "<failed>" : marker) + "' err=" + error);
    ApplyRules(h2_context_, "map sheet.test 127.0.0.3:9545",
               base::BindOnce(&HostMappingPocRunner::OnHttp2Repointed,
                              base::Unretained(this)));
  }

  void OnHttp2Repointed(bool applied, std::string error) {
    PocReport(applied, "H2-repoint",
              "applied=" + std::string(applied ? "true" : "false"));
    // Stage 1: clear only the DNS cache. If the h2 session is reused, the
    // request still reaches the OLD backend (ENV-A) even though the rules now
    // say ENV-C.
    h2_context_->ClearHostCache(nullptr, base::RepeatingClosure());
    FetchThroughPartition(
        h2_partition_, GURL("https://sheet.test:9543/poc"),
        net::LOAD_DISABLE_CACHE,
        base::BindOnce(&HostMappingPocRunner::OnHttp2CacheOnlyFetched,
                       base::Unretained(this)));
  }

  void OnHttp2CacheOnlyFetched(bool ok, std::string body, std::string error) {
    h2_cache_only_ = ok ? EnvironmentMarkerIn(body) : std::string();
    LOG(INFO) << "PARRA-POC2 H2 cache_only marker='"
              << (h2_cache_only_.empty() ? "<failed>" : h2_cache_only_) << "'";
    h2_context_->CloseIdleConnections(base::RepeatingClosure());
    FetchThroughPartition(
        h2_partition_, GURL("https://sheet.test:9543/poc"),
        net::LOAD_DISABLE_CACHE,
        base::BindOnce(&HostMappingPocRunner::OnHttp2CloseIdleFetched,
                       base::Unretained(this)));
  }

  void OnHttp2CloseIdleFetched(bool ok, std::string body, std::string error) {
    h2_close_idle_ = ok ? EnvironmentMarkerIn(body) : std::string();
    LOG(INFO) << "PARRA-POC2 H2 close_idle marker='"
              << (h2_close_idle_.empty() ? "<failed>" : h2_close_idle_) << "'";
    LOG(INFO) << "PARRA-POC2 H2 MARKERS cache_only="
              << (h2_cache_only_.empty() ? "<failed>" : h2_cache_only_)
              << " closeidle="
              << (h2_close_idle_.empty() ? "<failed>" : h2_close_idle_);
    Finish();
  }

  void Finish() {
    LOG(INFO) << "PARRA-POC2 SUMMARY checks=" << g_poc_checks
              << " failures=" << g_poc_failures << " test_a='" << test_a_before_
              << "'->cache_only='" << test_a_cache_only_ << "'->closeidle='"
              << (test_a_after_.empty() ? "?" : test_a_after_) << "'"
              << " test_b='" << test_b_before_ << "'->'"
              << (test_b_after_.empty() ? "?" : test_b_after_) << "'"
              << " h2_cache_only='" << h2_cache_only_ << "'"
              << " h2_closeidle='" << h2_close_idle_ << "'";
    // Machine-readable lines for the outer scripts.
    LOG(INFO) << "PARRA-POC2 MARKERS test_a_before=" << test_a_before_
              << " test_a_cache_only=" << test_a_cache_only_
              << " test_a_closeidle=" << test_a_after_
              << " test_b_before=" << test_b_before_
              << " test_b_after=" << test_b_after_;
  }

  raw_ptr<content::BrowserContext> browser_context_;
  raw_ptr<content::StoragePartition> empty_partition_ = nullptr;
  raw_ptr<content::StoragePartition> test_a_partition_ = nullptr;
  raw_ptr<content::StoragePartition> test_b_partition_ = nullptr;
  raw_ptr<content::StoragePartition> h2_partition_ = nullptr;
  raw_ptr<network::mojom::NetworkContext> empty_context_ = nullptr;
  raw_ptr<network::mojom::NetworkContext> test_a_context_ = nullptr;
  raw_ptr<network::mojom::NetworkContext> test_b_context_ = nullptr;
  raw_ptr<network::mojom::NetworkContext> h2_context_ = nullptr;
  std::string test_a_before_;
  std::string test_a_cache_only_;
  std::string test_a_after_;
  std::string test_b_before_;
  std::string test_b_after_;
  std::string h2_cache_only_;
  std::string h2_close_idle_;
};

}  // namespace

void MaybeRunHostMappingPoc() {
  const base::CommandLine* command_line =
      base::CommandLine::ForCurrentProcess();
  if (!command_line->HasSwitch(switches::kParraPocHostMappingUpdate)) {
    return;
  }
  BrowserWindowInterface* browser =
      GetLastActiveBrowserWindowInterfaceWithAnyProfile();
  if (!browser || !browser->GetProfile()) {
    LOG(ERROR) << "PARRA-POC1 no browser window/profile";
    return;
  }
  Profile* profile = browser->GetProfile();

  // Delayed so the StoragePartitions exist by the time they are queried.
  base::SingleThreadTaskRunner::GetCurrentDefault()->PostDelayedTask(
      FROM_HERE,
      base::BindOnce([](Profile* p) { HostMappingPocRunner::Start(p); },
                     profile),
      base::Seconds(6));
}

}  // namespace parra
