// Copyright 2026 The Parra Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// Parra P3.4 PoC-2 scenario E: does a rule update disturb an ALREADY OPEN page?
//
// THE TWO CLAIMS THAT MUST NOT BE MERGED
// -------------------------------------
// "the page was not reloaded" and "the page's later requests use the new
// backend" are independent facts, and conflating them is how a PoC reports
// success for something it never measured:
//
//   * A rule update lives in the NetworkContext. It cannot reload a document,
//   so
//     "no reload" is nearly free to achieve and says nothing about routing.
//   * A page's later requests may still use a pooled connection to the OLD
//     backend even after the rules changed (PoC-2 measured exactly that for
//     both HTTP/1.1 keep-alive and h2). So "no reload" can be true WHILE later
//     requests are stale.
//
// This runner therefore only interferes with a live page's network
// configuration, and deliberately does NOT assert anything about the page
// content itself -- the content assertions are made externally, by CDP, in
// parra-poc/devtools/verify-poc2-page-state.js. That split matters: the code
// that changes the rules must not also be the code that grades whether the page
// survived, or a bug in the update path could hide itself.
//
// DELIBERATELY NARROW: this only updates rules on the partition of a named
// environment and reports the fact. It never navigates, never reloads, never
// closes a tab.

#include <string>
#include <utility>

#include "base/command_line.h"
#include "base/functional/bind.h"
#include "base/location.h"
#include "base/logging.h"
#include "base/task/single_thread_task_runner.h"
#include "base/time/time.h"
#include "chrome/browser/parra/environment_index_mapper.h"
#include "chrome/browser/parra/parra_host_mapping_poc.h"
#include "chrome/browser/parra/parra_storage_partition_util.h"
#include "chrome/browser/parra/parra_switches.h"
#include "chrome/browser/profiles/profile.h"
#include "chrome/browser/ui/browser_window/public/browser_window_interface.h"
#include "chrome/browser/ui/browser_window/public/browser_window_interface_iterator.h"
#include "chrome/browser/ui/tabs/tab_strip_model.h"
#include "content/public/browser/browser_context.h"
#include "content/public/browser/storage_partition.h"
#include "content/public/browser/web_contents.h"
#include "services/network/public/mojom/network_context.mojom.h"

namespace parra {

namespace {

// Applies `rules` to the NetworkContext of `environment_id` and logs one line.
//
// The log line is the only output. Whether the page survived is decided by the
// external CDP script, not here.
void ApplyRulesToEnvironment(Profile* profile,
                             const std::string& environment_id,
                             const std::string& rules,
                             const char* tag) {
  const PartitionResolution resolution =
      ResolvePartitionForEnvironment(profile, environment_id);
  if (!resolution.ok()) {
    LOG(ERROR) << "PARRA-POC2 PAGE " << tag << " cannot resolve environment '"
               << environment_id << "': " << resolution.failure;
    return;
  }

  content::StoragePartition* partition =
      profile->GetStoragePartition(resolution.config, /*can_create=*/false);
  if (!partition) {
    LOG(ERROR) << "PARRA-POC2 PAGE " << tag << " no StoragePartition for '"
               << environment_id << "'";
    return;
  }

  network::mojom::NetworkContext* context = partition->GetNetworkContext();
  if (!context) {
    LOG(ERROR) << "PARRA-POC2 PAGE " << tag << " no NetworkContext for '"
               << environment_id << "'";
    return;
  }

  // Timestamped because the external script correlates this with page state it
  // sampled before and after. Without a shared timeline, "the page survived"
  // and "the rules changed" could be true of two different moments.
  LOG(INFO) << "PARRA-POC2 PAGE " << tag << " repointing environment='"
            << environment_id << "' rules='" << rules << "'";

  context->SetHostMappingRules(
      rules, /*revision=*/1,
      base::BindOnce(
                 [](std::string environment_id, std::string tag, bool applied,
                    const std::string& error, uint64_t applied_revision,
                    uint64_t generation,
                    network::mojom::HostOverridePolicy policy,
                    const std::string& policy_reason) {
                   LOG(INFO) << "PARRA-POC2 PAGE " << tag
                             << " applied=" << (applied ? "true" : "false")
                             << " environment='" << environment_id
                             << "' applied_revision=" << applied_revision
                             << " generation=" << generation << " error='"
                             << error << "'";
                 },
                 environment_id, std::string(tag)));

  // The rule change alone does not touch pooled connections. Closing idle ones
  // is what makes the change visible to the page's next request, and the PoC
  // does it explicitly so the observed result is not ambiguous about which call
  // did what.
  context->CloseIdleConnections(base::RepeatingClosure());
}

}  // namespace

// No-op unless --parra-poc-page-state was supplied.
//
// Timeline (the external script sleeps through it):
//   t=0   browser starts, environment tabs load normally
//   t=+14s phase 1: test-a -> ENV-C (127.0.0.3:8445)
//   t=+28s phase 2: test-a -> ENV-A (127.0.0.1:8443)
//
// Two phases rather than one: a single repoint cannot distinguish "the page
// survived a rule change" from "the page survived the FIRST rule change and the
// second one broke it". Going out and back also proves the mapping is not
// one-way.
void MaybeRunPageStatePoc() {
  const base::CommandLine* command_line =
      base::CommandLine::ForCurrentProcess();
  if (!command_line->HasSwitch(switches::kParraPocPageState)) {
    return;
  }

  BrowserWindowInterface* browser =
      GetLastActiveBrowserWindowInterfaceWithAnyProfile();
  if (!browser || !browser->GetProfile()) {
    LOG(ERROR) << "PARRA-POC2 PAGE no browser window/profile";
    return;
  }
  Profile* profile = browser->GetProfile();

  base::SingleThreadTaskRunner::GetCurrentDefault()->PostDelayedTask(
      FROM_HERE,
      base::BindOnce(&ApplyRulesToEnvironment, profile, std::string("test-a"),
                     std::string("map sheet.test 127.0.0.3:8445"), "phase1"),
      base::Seconds(14));

  base::SingleThreadTaskRunner::GetCurrentDefault()->PostDelayedTask(
      FROM_HERE,
      base::BindOnce(&ApplyRulesToEnvironment, profile, std::string("test-a"),
                     std::string("map sheet.test 127.0.0.1:8443"), "phase2"),
      base::Seconds(28));

  LOG(INFO) << "PARRA-POC2 PAGE scheduled phase1 at +14s, phase2 at +28s";
}

}  // namespace parra
