// Copyright 2026 The Parra Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chrome/browser/parra/parra_tab_ops_selftest.h"

#include <memory>
#include <string>
#include <vector>

#include "base/command_line.h"
#include "base/functional/bind.h"
#include "base/location.h"
#include "base/logging.h"
#include "base/run_loop.h"
#include "base/strings/string_number_conversions.h"
#include "base/task/sequenced_task_runner.h"
#include "base/task/single_thread_task_runner.h"
#include "chrome/browser/parra/environment_index_mapper.h"
#include "chrome/browser/parra/environment_tab_operations.h"
#include "chrome/browser/parra/environment_tab_router.h"
#include "chrome/browser/parra/environment_tag.h"
#include "chrome/browser/parra/parra_storage_partition_util.h"
#include "chrome/browser/parra/parra_switches.h"
#include "chrome/browser/profiles/profile.h"
#include "chrome/browser/ui/browser_window/public/browser_window_interface.h"
#include "chrome/browser/ui/browser_window/public/browser_window_interface_iterator.h"
#include "chrome/browser/ui/navigator/browser_navigator.h"
#include "chrome/browser/ui/navigator/browser_navigator_params.h"
#include "chrome/browser/ui/tabs/tab_group_model.h"
#include "chrome/browser/ui/tabs/tab_strip_model.h"
#include "chrome/browser/ui/tabs/tab_strip_model_delegate.h"
#include "components/tabs/public/tab_group.h"
#include "components/tabs/public/tab_interface.h"
#include "content/public/browser/navigation_controller.h"
#include "content/public/browser/security_principal.h"
#include "content/public/browser/site_instance.h"
#include "content/public/browser/web_contents.h"
#include "ui/base/base_window.h"

namespace parra {

namespace {

int g_checks = 0;
int g_failures = 0;

void Report(bool ok, const std::string& test, const std::string& detail) {
  ++g_checks;
  if (!ok)
    ++g_failures;
  LOG(INFO) << "PARRA-TABOPS " << (ok ? "PASS" : "FAIL") << " " << test << " | "
            << detail;
}

std::string Token(content::WebContents* contents) {
  return contents ? contents->GetUniqueToken().ToString() : std::string("<null>");
}

// The environment a tab is tagged with, or "" when it carries no tag.
std::string EnvironmentOfTab(content::WebContents* contents) {
  return contents ? EnvironmentTag::Get(contents) : std::string();
}

// The model index currently holding the tab whose unique token is `token`, or
// TabStripModel::kNoTab when that tab is no longer in the model. Used to prove that
// a specific tab SURVIVED an operation rather than being replaced by a new one.
int FindByToken(TabStripModel* model, const std::string& token) {
  if (!model) {
    return TabStripModel::kNoTab;
  }
  for (int i = 0; i < model->count(); ++i) {
    content::WebContents* contents = model->GetWebContentsAt(i);
    if (contents && contents->GetUniqueToken().ToString() == token) {
      return i;
    }
  }
  return TabStripModel::kNoTab;
}

std::string TextIds(const std::vector<std::string>& ids) {
  std::string out;
  for (size_t i = 0; i < ids.size(); ++i) {
    if (i)
      out += ",";
    out += ids[i].substr(0, 8);
  }
  return "[" + out + "]";
}

// The environment a live tab's committed document actually runs in, read from the
// SecurityPrincipal. Empty means the default partition.
std::string ActualPartitionEnvironment(content::WebContents* contents) {
  if (!contents)
    return std::string();
  content::SiteInstance* site_instance = contents->GetSiteInstance();
  if (!site_instance)
    return std::string();
  const std::string_view id = GetEnvironmentIdForStoragePartition(
      site_instance->GetSecurityPrincipal().GetStoragePartitionConfig());
  return id.empty() ? std::string(kDefaultEnvironmentId) : std::string(id);
}

// A full strip snapshot: token + tag + real partition, per model index.
struct Row {
  int index = 0;
  std::string token;
  std::string tag;
  std::string actual;
};

std::vector<Row> Snapshot(Profile* profile, TabStripModel* model) {
  std::vector<Row> rows;
  for (int i = 0; i < model->count(); ++i) {
    content::WebContents* contents = model->GetWebContentsAt(i);
    rows.push_back({i, Token(contents), EnvironmentTag::Get(contents),
                    ActualPartitionEnvironment(contents)});
  }
  return rows;
}

// Every token that existed before and must still exist after.
std::vector<std::string> TokensOfEnvironment(TabStripModel* model,
                                             const std::string& environment) {
  std::vector<std::string> tokens;
  for (int i = 0; i < model->count(); ++i) {
    content::WebContents* contents = model->GetWebContentsAt(i);
    if (EnvironmentTag::Get(contents) == environment)
      tokens.push_back(Token(contents));
  }
  return tokens;
}

// True when every token in `expected` still exists somewhere in the model.
bool AllStillPresent(TabStripModel* model,
                     const std::vector<std::string>& expected,
                     std::vector<std::string>* missing) {
  std::vector<std::string> live;
  for (int i = 0; i < model->count(); ++i)
    live.push_back(Token(model->GetWebContentsAt(i)));

  bool all = true;
  for (const std::string& token : expected) {
    bool found = false;
    for (const std::string& candidate : live) {
      if (candidate == token) {
        found = true;
        break;
      }
    }
    if (!found) {
      all = false;
      if (missing)
        missing->push_back(token);
    }
  }
  return all;
}

class TabOpsSelfTest {
 public:
  TabOpsSelfTest(BrowserWindowInterface* browser,
                 Profile* profile,
                 TabStripModel* model)
      : browser_(browser), profile_(profile), model_(model), mapper_(model) {}

  void Run();

 private:
  // P1: Close Other Tabs must not touch other environments.
  void TestCloseOtherTabs();
  // P1: Close Tabs To Right uses VISIBLE order, not the global model range.
  void TestCloseTabsToRight();
  // P1: a multi-selection that spans environments must not close foreign tabs.
  void TestCrossEnvironmentMultiSelect();
  // P2: an unhonorable requested environment must be refused, not downgraded.
  void TestRequestedEnvironmentRejected();
  // P2: every live tab's tag must equal its real partition.
  void TestTagMatchesPartition();
  // P4 item 11: Duplicate Tab keeps tag AND partition.
  void TestDuplicateTab();
  // P0: a tab group may only contain one environment.
  void TestGroupStaysInsideOneEnvironment();
  // P0: closing a group must not destroy another environment's tabs.
  void TestCloseGroupDoesNotCrossEnvironments();
  // P1: an empty victim set must REFUSE, never fall through to the global close.
  void TestEmptyVictimSetRefuses();
  // P2: about:blank / empty-document creation must not be misread as Personal.
  void TestAboutBlankPartitionSemantics();
  // P1: the close policy must be enforced on the MODEL, so the keyboard
  // accelerators (which call ExecuteContextMenuCommand() directly and never pass
  // through BrowserTabStripController) cannot bypass it.
  void TestAcceleratorPathIsCovered();

  // Builds a group containing `indices` via the production API and returns it.
  std::optional<tab_groups::TabGroupId> MakeGroup(
      const std::vector<int>& indices);
  // Removes every group from the model so each test starts clean.
  void DissolveAllGroups();

  // Restores a 3x3 layout so each test starts from a known state. Tabs are added
  // through the native path with the partition pinned up front, exactly like the
  // visibility PoC does.
  void EnsureThreeEnvironments();
  // Makes sure `environment` has at least `minimum` tabs, creating the missing
  // ones in that environment's own partition. Returns false (after reporting) when
  // it cannot.
  bool EnsureEnvironmentHasTabs(const std::string& environment, int minimum);
  void CloseAll();

  raw_ptr<BrowserWindowInterface> browser_;
  raw_ptr<Profile> profile_;
  raw_ptr<TabStripModel> model_;
  EnvironmentIndexMapper mapper_;
};

void TabOpsSelfTest::EnsureThreeEnvironments() {
  EnvironmentIndexMapper mapper(model_);
  const char* environments[] = {"personal", "test-a", "test-b"};
  for (const char* environment : environments) {
    if (mapper.GetVisibleCount(environment) > 0)
      continue;
    LOG(WARNING) << "PARRA-TABOPS setup: environment '" << environment
                 << "' is empty; the test needs the 3x3 layout from "
                    "--parra-poc-visibility";
  }
}

bool TabOpsSelfTest::EnsureEnvironmentHasTabs(const std::string& environment,
                                              int minimum) {
  int have = mapper_.GetVisibleCount(environment);
  if (have >= minimum)
    return true;

  const content::StoragePartitionConfig config =
      GetStoragePartitionConfigForEnvironment(profile_, environment);
  while (have < minimum) {
    std::unique_ptr<content::WebContents> contents;
    if (config.is_default()) {
      // CreateForFixedStoragePartition() CHECKs !is_default().
      content::WebContents::CreateParams create_params(profile_);
      contents = content::WebContents::Create(create_params);
    } else {
      // A REAL navigable URL, not about:blank.
      //
      // CreateForFixedStoragePartition() does not assign a site for an
      // empty-document scheme, so an about:blank SiteInstance ends up with the
      // DEFAULT StoragePartitionConfig in its SecurityPrincipal even though the
      // BrowsingInstance was pinned. The filler would then be tagged for an
      // environment it is not actually in -- exactly the mismatch TagNewContents()
      // exists to refuse. Using a real URL keeps the filler honest.
      auto site_instance = content::SiteInstance::CreateForFixedStoragePartition(
          profile_, GURL("https://sheet.test:8443/"), config);
      content::WebContents::CreateParams create_params(profile_,
                                                      std::move(site_instance));
      contents = content::WebContents::Create(create_params);
    }
    if (!contents) {
      Report(false, "setup-create-tab",
             "could not create a WebContents for '" + environment + "'");
      return false;
    }

    // Tag through the real router so the same tag/partition invariant applies to
    // test fixtures as to production tabs.
    ResolvedTabEnvironment resolved;
    resolved.active = true;
    resolved.environment_id = environment;
    resolved.partition = config;
    resolved.origin = EnvironmentOrigin::kExplicitRequest;
    if (!EnvironmentTabRouter::TagNewContents(contents.get(), resolved)) {
      Report(false, "setup-create-tab-invariant",
             "filler for '" + environment +
                 "' landed in the wrong partition; refusing to insert it");
      return false;
    }

    // ADD_NONE: insert in the background. The filler must not steal activation
    // from the tab under test, and the environment is activated explicitly below.
    model_->AddWebContents(std::move(contents), -1, ui::PAGE_TRANSITION_TYPED,
                           AddTabTypes::ADD_NONE);
    ++have;
    LOG(INFO) << "PARRA-TABOPS setup: created a filler tab for '" << environment
              << "' (" << have << "/" << minimum << ") partition=" << config;
  }

  mapper_.SetActiveEnvironment(environment);
  return true;
}

void TabOpsSelfTest::CloseAll() {
  // Close every non-pinned tab through the native API; used between tests.
  while (model_->count() > 0) {
    content::WebContents* contents = model_->GetWebContentsAt(0);
    model_->CloseWebContentsAt(0, TabCloseTypes::CLOSE_NONE);
    if (model_->GetWebContentsAt(0) == contents)
      break;  // unload handler refused; do not spin
  }
}

void TabOpsSelfTest::TestTagMatchesPartition() {
  const std::vector<Row> rows = Snapshot(profile_, model_);
  int mismatched = 0;
  std::string detail;
  for (const Row& row : rows) {
    if (row.tag != row.actual) {
      ++mismatched;
      detail += " g" + base::NumberToString(row.index) + ":" + row.tag + "!=" +
                row.actual;
    }
  }
  Report(mismatched == 0, "P2-tag-equals-actual-partition",
         base::NumberToString(rows.size()) + " tab(s), " +
             base::NumberToString(mismatched) + " mismatch" + detail);
}

void TabOpsSelfTest::TestCloseOtherTabs() {
  // Needs at least two tabs in test-a. Earlier tests are destructive, so this one
  // starts by restoring the layout it needs rather than assuming the 3x3 pristine
  // state -- a test that silently skips is how a real regression hides.
  if (!EnsureEnvironmentHasTabs("test-a", /*minimum=*/2))
    return;

  const std::string target_environment = "test-a";
  const std::vector<int> visible =
      mapper_.GetVisibleGlobalIndices(target_environment);
  if (visible.empty()) {
    Report(false, "P1-close-other-tabs-precondition",
           "environment '" + target_environment + "' has no tabs");
    return;
  }

  mapper_.SetActiveEnvironment(target_environment);

  const std::vector<std::string> foreign_before = [&] {
    std::vector<std::string> tokens;
    for (const char* other : {"personal", "test-b"}) {
      for (const std::string& t : TokensOfEnvironment(model_, other))
        tokens.push_back(t);
    }
    return tokens;
  }();
  const std::vector<std::string> own_before =
      TokensOfEnvironment(model_, target_environment);

  const int context_index = visible[0];
  const std::string kept_token = Token(model_->GetWebContentsAt(context_index));

  std::vector<tabs::TabHandle> victims;
  const bool resolved = EnvironmentTabOperations::ResolveCloseCommandVictims(
      model_, context_index, TabStripModel::CommandCloseOtherTabs, &victims);
  Report(resolved, "P1-close-other-tabs-resolved",
         base::NumberToString(victims.size()) + " victim(s) inside '" +
             target_environment + "'");

  // Every resolved victim must belong to the target environment. This is checked
  // on the victim set itself, before anything is closed.
  int foreign_victims = 0;
  for (tabs::TabHandle handle : victims) {
    tabs::TabInterface* tab = handle.Get();
    if (tab && EnvironmentTag::Get(tab->GetContents()) != target_environment)
      ++foreign_victims;
  }
  Report(foreign_victims == 0, "P1-close-other-tabs-no-foreign-victim",
         base::NumberToString(foreign_victims) + " victim(s) outside '" +
             target_environment + "'");

  if (browser_) {
    Report(model_->ExecuteCloseCommandWithVictims(
               context_index, TabStripModel::CommandCloseOtherTabs, victims),
           "P1-close-other-tabs-executed", "issued through the native close");
  }

  std::vector<std::string> missing;
  const bool foreign_intact = AllStillPresent(model_, foreign_before, &missing);
  Report(foreign_intact, "P1-close-other-tabs-foreign-intact",
         base::NumberToString(foreign_before.size()) + " foreign tab(s) before, " +
             (foreign_intact ? "all still alive"
                             : "missing " + TextIds(missing)));

  // The context tab must survive: "close OTHER tabs".
  bool kept_alive = false;
  for (int i = 0; i < model_->count(); ++i) {
    if (Token(model_->GetWebContentsAt(i)) == kept_token)
      kept_alive = true;
  }
  Report(kept_alive, "P1-close-other-tabs-context-survives",
         "token " + kept_token.substr(0, 8));

  // And the environment should now hold only the context tab.
  Report(mapper_.GetVisibleCount(target_environment) == 1,
         "P1-close-other-tabs-only-context-left",
         base::NumberToString(mapper_.GetVisibleCount(target_environment)) +
             " tab(s) left in '" + target_environment + "' (was " +
             base::NumberToString(own_before.size()) + ")");
}

void TabOpsSelfTest::TestCloseTabsToRight() {
  // Earlier tests are destructive (group close removes its members), so restore the
  // layout this test needs instead of depending on the pristine 3x3. A test that
  // silently degrades to a FAIL because of test ordering is a broken test, not a
  // product finding.
  if (!EnsureEnvironmentHasTabs("test-a", /*minimum=*/3))
    return;

  const std::vector<int> before_visible =
      mapper_.GetVisibleGlobalIndices("test-a");
  if (before_visible.size() < 2) {
    Report(false, "P1-close-right-precondition",
           "need at least two test-a tabs, have " +
               base::NumberToString(before_visible.size()));
    return;
  }

  mapper_.SetActiveEnvironment("test-a");

  // Context = the FIRST visible test-a tab. Everything the user sees to its right
  // inside test-a must close; every foreign tab must survive.
  const int context_index = before_visible[0];

  std::vector<std::string> foreign_before;
  for (int i = 0; i < model_->count(); ++i) {
    content::WebContents* contents = model_->GetWebContentsAt(i);
    if (EnvironmentTag::Get(contents) != "test-a")
      foreign_before.push_back(Token(contents));
  }

  const std::vector<std::string> own_after_expected = [&] {
    std::vector<std::string> tokens;
    // Visible test-a tabs strictly to the right of the context in VISIBLE order.
    for (size_t i = 1; i < before_visible.size(); ++i)
      tokens.push_back(Token(model_->GetWebContentsAt(before_visible[i])));
    return tokens;
  }();

  std::vector<tabs::TabHandle> victims;
  const bool resolved = EnvironmentTabOperations::ResolveCloseCommandVictims(
      model_, context_index, TabStripModel::CommandCloseTabsToRight, &victims);
  Report(resolved, "P1-close-right-resolved",
         base::NumberToString(victims.size()) + " victim(s)");
  Report(victims.size() == own_after_expected.size(),
         "P1-close-right-visible-count",
         "expected " + base::NumberToString(own_after_expected.size()) +
             " (visible tabs right of the context), got " +
             base::NumberToString(victims.size()));

  int foreign_victims = 0;
  for (tabs::TabHandle handle : victims) {
    tabs::TabInterface* tab = handle.Get();
    if (tab && EnvironmentTag::Get(tab->GetContents()) != "test-a")
      ++foreign_victims;
  }
  Report(foreign_victims == 0, "P1-close-right-no-foreign-victim",
         base::NumberToString(foreign_victims) + " victim(s) outside test-a");

  if (browser_) {
    model_->ExecuteCloseCommandWithVictims(
        context_index, TabStripModel::CommandCloseTabsToRight, victims);
  }

  std::vector<std::string> missing;
  const bool foreign_intact = AllStillPresent(model_, foreign_before, &missing);
  Report(foreign_intact, "P1-close-right-foreign-intact",
         base::NumberToString(foreign_before.size()) + " foreign tab(s) before, " +
             (foreign_intact ? "all still alive"
                             : "missing " + TextIds(missing)));
}

void TabOpsSelfTest::TestCrossEnvironmentMultiSelect() {
  // Select a tab in test-a and a tab in test-b at the same time, then run the
  // multi-select Close command with a test-a context tab.
  const std::vector<int> test_a = mapper_.GetVisibleGlobalIndices("test-a");
  const std::vector<int> test_b = mapper_.GetVisibleGlobalIndices("test-b");
  if (test_a.empty() || test_b.empty()) {
    Report(false, "P1-multiselect-precondition",
           "needs tabs in both test-a and test-b");
    return;
  }

  mapper_.SetActiveEnvironment("test-a");

  const std::string foreign_token = Token(model_->GetWebContentsAt(test_b[0]));

  // A cross-environment multi-selection cannot be built through the public model
  // API (selection_model() is const, and the UI's shift-click path is not callable
  // from here). Rather than fake the selection, verify the CONTAINMENT RULE on the
  // input the command would actually see: take Chromium's own candidate set for
  // CommandCloseTab and confirm the filter keeps only this environment's tabs.
  std::vector<tabs::TabHandle> candidates =
      model_->GetCloseCommandCandidates(test_a[0], TabStripModel::CommandCloseTab);
  Report(!candidates.empty(), "P1-multiselect-candidates",
         base::NumberToString(candidates.size()) +
             " native candidate(s) for the multi-select close");

  std::vector<tabs::TabHandle> victims;
  const bool resolved = EnvironmentTabOperations::ResolveCloseCommandVictims(
      model_, test_a[0], TabStripModel::CommandCloseTab, &victims);

  int foreign_victims = 0;
  for (tabs::TabHandle handle : victims) {
    tabs::TabInterface* tab = handle.Get();
    if (tab && EnvironmentTag::Get(tab->GetContents()) != "test-a")
      ++foreign_victims;
  }
  Report(foreign_victims == 0, "P1-multiselect-no-foreign-victim",
         base::NumberToString(foreign_victims) + " victim(s) outside test-a");
  Report(resolved && victims.size() == 1,
         "P1-multiselect-only-own-tab",
         base::NumberToString(victims.size()) +
             " victim(s); expected exactly the test-a context tab");

  if (browser_ && resolved) {
    model_->ExecuteCloseCommandWithVictims(test_a[0],
                                           TabStripModel::CommandCloseTab,
                                           victims);
  }

  // Whatever the close did, the test-b tab must still be alive.
  std::vector<std::string> missing;
  Report(AllStillPresent(model_, {foreign_token}, &missing),
         "P1-multiselect-foreign-intact",
         "foreign token " + foreign_token.substr(0, 8) +
             (missing.empty() ? " still alive" : " DESTROYED"));
}

void TabOpsSelfTest::TestRequestedEnvironmentRejected() {
  // P2 NEGATIVE CASE.
  //
  // Ask for an environment that is not configured. The router must refuse with a
  // diagnosable failure instead of silently creating the tab in Personal.
  {
    const ResolvedTabEnvironment resolved =
        EnvironmentTabRouter::ResolveForNewTab(profile_, /*source_contents=*/nullptr,
                                              "no-such-environment");
    Report(resolved.active && !resolved.ok(),
           "P2-unknown-requested-env-rejected",
           "active=" + std::string(resolved.active ? "yes" : "no") +
               " ok=" + std::string(resolved.ok() ? "yes" : "no") +
               " failure='" + resolved.failure + "'");
    Report(!resolved.failure.empty(), "P2-refusal-is-diagnosable",
           "failure text: '" + resolved.failure + "'");
  }

  // A configured environment must still resolve cleanly, so the refusal above is
  // specific to a bad request and not a blanket rejection.
  {
    const ResolvedTabEnvironment resolved =
        EnvironmentTabRouter::ResolveForNewTab(profile_, /*source_contents=*/nullptr,
                                              "test-a");
    Report(resolved.ok(), "P2-known-requested-env-accepted",
           "env='" + resolved.environment_id + "' origin=" +
               base::NumberToString(static_cast<int>(resolved.origin)));
    Report(resolved.environment_id == "test-a",
           "P2-known-requested-env-honored", resolved.environment_id);
  }

  // And an explicitly requested environment must not be silently replaced by the
  // default environment: this is the "no silent downgrade to Personal" rule.
  {
    const ResolvedTabEnvironment resolved =
        EnvironmentTabRouter::ResolveForNewTab(profile_, /*source_contents=*/nullptr,
                                              "test-b");
    Report(resolved.ok() && resolved.environment_id == "test-b",
           "P2-requested-env-not-downgraded",
           "requested='test-b' resolved='" + resolved.environment_id + "'");
  }
}

void TabOpsSelfTest::TestDuplicateTab() {
  // Duplicate Tab goes through WebContents::Clone(), which reuses the source
  // SiteInstance (so the partition is inherited by construction) but copies NO
  // UserData -- which is where EnvironmentTag lives. The bug this guards against
  // is therefore "right partition, wrong tag", not "wrong partition".
  const std::vector<int> visible = mapper_.GetVisibleGlobalIndices("test-b");
  if (visible.empty()) {
    Report(false, "P4-duplicate-precondition", "test-b has no tabs");
    return;
  }

  mapper_.SetActiveEnvironment("test-b");

  content::WebContents* source = model_->GetWebContentsAt(visible[0]);
  const std::string source_token = Token(source);
  const std::string source_tag = EnvironmentTag::Get(source);
  const std::string source_actual = ActualPartitionEnvironment(source);
  const int source_index = model_->GetIndexOfWebContents(source);

  const size_t before_count = model_->count();

  // The same call the tab context menu's Duplicate command makes.
  browser_->GetTabStripModel()->delegate()->DuplicateContentsAt(source_index);

  Report(model_->count() == static_cast<int>(before_count) + 1,
         "P4-duplicate-created-tab",
         base::NumberToString(before_count) + " -> " +
             base::NumberToString(model_->count()));

  if (model_->count() != static_cast<int>(before_count) + 1)
    return;

  // The clone is inserted immediately right of the source.
  content::WebContents* clone = model_->GetWebContentsAt(source_index + 1);
  if (!clone) {
    Report(false, "P4-duplicate-clone-found", "no tab at source_index + 1");
    return;
  }

  const std::string clone_tag = EnvironmentTag::Get(clone);
  const std::string clone_actual = ActualPartitionEnvironment(clone);

  Report(clone_tag == source_tag, "P4-duplicate-keeps-tag",
         "source='" + source_tag + "' clone='" + clone_tag + "'");
  Report(clone_actual == source_actual, "P4-duplicate-keeps-partition",
         "source='" + source_actual + "' clone='" + clone_actual + "'");
  Report(clone_tag == clone_actual, "P4-duplicate-tag-matches-partition",
         "clone tag='" + clone_tag + "' actual='" + clone_actual + "'");
  Report(Token(clone) != source_token, "P4-duplicate-is-new-webcontents",
         "clone token=" + Token(clone).substr(0, 8) +
             " source token=" + source_token.substr(0, 8));
}

void TabOpsSelfTest::DissolveAllGroups() {
  if (!model_->group_model())
    return;
  // Collect first: RemoveFromGroup mutates the group list.
  std::vector<tab_groups::TabGroupId> groups =
      model_->group_model()->ListTabGroups();
  for (const tab_groups::TabGroupId& group : groups) {
    if (!model_->group_model()->ContainsTabGroup(group))
      continue;
    std::vector<int> members;
    for (int index :
         model_->group_model()->GetTabGroup(group)->ListTabs().ToIntVector()) {
      if (model_->ContainsIndex(index))
        members.push_back(index);
    }
    if (!members.empty())
      model_->RemoveFromGroup(members);
  }
}

std::optional<tab_groups::TabGroupId> TabOpsSelfTest::MakeGroup(
    const std::vector<int>& indices) {
  if (indices.size() < 2)
    return std::nullopt;
  // AddToNewGroup() returns a fresh id even when an embedder policy vetoes the
  // call, so "created" must be judged by whether the members actually carry the
  // group, not by the return value.
  const tab_groups::TabGroupId candidate = model_->AddToNewGroup(indices);
  if (!model_->group_model() ||
      !model_->group_model()->ContainsTabGroup(candidate)) {
    return std::nullopt;
  }
  return candidate;
}

void TabOpsSelfTest::TestGroupStaysInsideOneEnvironment() {
  DissolveAllGroups();

  const std::vector<int> test_a = mapper_.GetVisibleGlobalIndices("test-a");
  const std::vector<int> test_b = mapper_.GetVisibleGlobalIndices("test-b");
  if (test_a.size() < 2 || test_b.empty()) {
    Report(false, "P0-group-precondition",
           "needs >=2 test-a tabs and >=1 test-b tab");
    return;
  }

  mapper_.SetActiveEnvironment("test-a");

  // (a) Same-environment grouping must still work: the policy must not be a blanket
  //     ban, otherwise the test below would pass for the wrong reason.
  const std::optional<tab_groups::TabGroupId> same_env_group =
      MakeGroup({test_a[0], test_a[1]});
  Report(same_env_group.has_value(), "P0-same-env-group-allowed",
         same_env_group ? "group created inside 'test-a'"
                        : "group creation was vetoed (policy too strict)");

  if (!same_env_group.has_value())
    return;

  // (b) The group's environment must be resolvable and equal to test-a.
  const std::string group_env =
      EnvironmentTabOperations::EnvironmentOfGroup(model_, *same_env_group);
  Report(group_env == "test-a", "P0-group-environment-is-test-a",
         "EnvironmentOfGroup()='" + group_env + "'");

  // (c) Adding a test-b tab to a test-a group must be VETOED.
  const std::vector<int> before_test_b = mapper_.GetVisibleGlobalIndices("test-b");
  const std::string foreign_token =
      Token(model_->GetWebContentsAt(before_test_b[0]));

  model_->AddToExistingGroup({before_test_b[0]}, *same_env_group, false);

  // Find where the foreign tab ended up and check its group.
  std::optional<tab_groups::TabGroupId> foreign_group;
  for (int i = 0; i < model_->count(); ++i) {
    if (Token(model_->GetWebContentsAt(i)) == foreign_token) {
      foreign_group = model_->GetTabGroupForTab(i);
      break;
    }
  }
  Report(!foreign_group.has_value(),
         "P0-cross-env-add-to-group-vetoed",
         foreign_group ? "the test-b tab JOINED the test-a group (policy failed)"
                       : "the test-b tab stayed ungrouped; token still alive");

  // (d) The group must still span exactly one environment.
  Report(EnvironmentTabOperations::GroupSpansEnvironments(model_,
                                                          *same_env_group) ==
             false,
         "P0-group-does-not-span-environments",
         "GroupSpansEnvironments() == false");

  // (e) Creating a NEW group out of tabs from two environments must also be vetoed.
  const std::vector<int> test_a_now = mapper_.GetVisibleGlobalIndices("test-a");
  const int a_index = test_a_now.empty() ? -1 : test_a_now.front();
  const int b_index = before_test_b[0];
  if (a_index >= 0) {
    std::vector<int> mixed = {a_index, b_index};
    std::sort(mixed.begin(), mixed.end());
    model_->AddToNewGroup(mixed);
    const std::optional<tab_groups::TabGroupId> a_group =
        model_->GetTabGroupForTab(a_index);
    const std::optional<tab_groups::TabGroupId> b_group =
        model_->GetTabGroupForTab(b_index);
    Report(!(a_group.has_value() && b_group.has_value() && *a_group == *b_group),
           "P0-cross-env-new-group-vetoed",
           a_group && b_group && *a_group == *b_group
               ? "a mixed group was created (policy failed)"
               : "no group was created across environments");
  }
}

void TabOpsSelfTest::TestCloseGroupDoesNotCrossEnvironments() {
  DissolveAllGroups();

  const std::vector<int> test_a = mapper_.GetVisibleGlobalIndices("test-a");
  if (test_a.size() < 2) {
    Report(false, "P0-close-group-precondition", "needs >=2 test-a tabs");
    return;
  }
  mapper_.SetActiveEnvironment("test-a");

  const std::optional<tab_groups::TabGroupId> group =
      MakeGroup({test_a[0], test_a[1]});
  if (!group.has_value()) {
    Report(false, "P0-close-group-setup", "could not create the test group");
    return;
  }

  // Record every OTHER environment's tokens: none may be destroyed.
  std::vector<std::string> foreign_before;
  for (const char* other : {"personal", "test-b"}) {
    for (const std::string& t : TokensOfEnvironment(model_, other))
      foreign_before.push_back(t);
  }

  const bool closed =
      EnvironmentTabOperations::CloseGroupInActiveEnvironment(model_, *group);
  Report(closed, "P0-close-group-succeeded",
         "CloseGroupInActiveEnvironment() returned true");

  std::vector<std::string> missing;
  Report(AllStillPresent(model_, foreign_before, &missing),
         "P0-close-group-foreign-intact",
         base::NumberToString(foreign_before.size()) +
             " foreign tab(s) before; " +
             (missing.empty() ? "all still alive"
                              : "MISSING " + TextIds(missing)));

  // The group is gone: its members were closed.
  Report(!model_->group_model() || !model_->group_model()->ContainsTabGroup(*group),
         "P0-close-group-removed", "the group no longer exists");
}

void TabOpsSelfTest::TestEmptyVictimSetRefuses() {
  // P1: the crucial containment property. A close command whose environment has
  // nothing to close must REFUSE. Before this round the caller could not tell
  // "not a close command" from "refused", so it fell through to Chromium's global
  // close -- silently widening the operation to every environment.
  //
  // Construct the exact state: the context tab is in an environment, and that
  // environment has no other tab, so Close Other Tabs has an empty victim set.
  DissolveAllGroups();

  // Drain test-b down to a single tab.
  while (mapper_.GetVisibleCount("test-b") > 1) {
    const std::vector<int> visible = mapper_.GetVisibleGlobalIndices("test-b");
    if (visible.empty())
      break;
    const int last = visible.back();
    content::WebContents* contents = model_->GetWebContentsAt(last);
    model_->CloseWebContentsAt(last, TabCloseTypes::CLOSE_NONE);
    if (model_->ContainsIndex(last) &&
        model_->GetWebContentsAt(last) == contents) {
      break;  // unload handler refused; stop rather than spin
    }
  }

  const std::vector<int> remaining = mapper_.GetVisibleGlobalIndices("test-b");
  if (remaining.size() != 1) {
    Report(false, "P1-empty-victims-precondition",
           "could not reduce test-b to a single tab (have " +
               base::NumberToString(remaining.size()) + ")");
    return;
  }

  mapper_.SetActiveEnvironment("test-b");
  const int context_index = remaining[0];
  const std::string context_token = Token(model_->GetWebContentsAt(context_index));

  const auto plan = EnvironmentTabOperations::PlanCloseCommand(
      model_, context_index, TabStripModel::CommandCloseOtherTabs);
  Report(plan.disposition ==
             EnvironmentTabOperations::CloseCommandDisposition::kRefuse,
         "P1-empty-victims-refused",
         "disposition=" + base::NumberToString(
                              static_cast<int>(plan.disposition)) +
             " reason='" + plan.reason + "'");

  // A refusal must NOT be reported as "not a close command" -- that is what would
  // let the caller run the native path.
  Report(plan.is_close_command(), "P1-refusal-is-not-a-fallthrough",
         "is_close_command() == true, so the caller cannot delegate");

  // Nothing may have been closed by planning alone.
  Report(Token(model_->GetWebContentsAt(context_index)) == context_token,
         "P1-planning-closes-nothing",
         "the context tab is untouched after planning");

  // And a non-close command must still be reported as "not mine", so unrelated
  // menu items keep working.
  const auto other = EnvironmentTabOperations::PlanCloseCommand(
      model_, context_index, TabStripModel::CommandReload);
  Report(other.disposition ==
             EnvironmentTabOperations::CloseCommandDisposition::kNotACloseCommand,
         "P1-non-close-command-passes-through",
         "CommandReload is not treated as a close command");
}

void TabOpsSelfTest::TestAboutBlankPartitionSemantics() {
  // P2. THE REAL NEGATIVE CASE.
  //
  // A page created from about:blank for a non-default environment CANNOT be
  // isolated: Chromium never assigns a site to an empty-document SiteInstance, so
  // it never registers with its BrowsingInstance, so no StoragePartitionConfig is
  // ever recorded for it and the first real navigation reads the DEFAULT partition.
  //
  // Measured, before the fix: a blank page created for test-a committed its first
  // HTTPS navigation in 'personal'. The fix is to REFUSE that creation path rather
  // than let it silently land in the wrong environment.
  //
  // This test therefore asserts the REFUSAL, not a successful binding.
  const content::StoragePartitionConfig config =
      GetStoragePartitionConfigForEnvironment(profile_, "test-a");

  if (config.is_default()) {
    Report(false, "P2-about-blank-precondition",
           "'test-a' maps to the default partition, so this case cannot be tested");
    return;
  }

  // (a) The empty-document path must be recognized as un-isolatable.
  const GURL about_blank(url::kAboutBlankURL);
  Report(!content::SiteInstance::ShouldAssignSiteForURL(about_blank),
         "P2-about-blank-has-no-site",
         "Chromium assigns no site to " + about_blank.spec() +
             ", which is why a partition cannot be pinned to it");

  // (b) A blank SiteInstance must NOT report a Parra partition: it reports the
  //     default one, which is exactly why relying on it would be a silent
  //     downgrade to Personal.
  auto blank_instance = content::SiteInstance::CreateForFixedStoragePartition(
      profile_, about_blank, config);
  const content::StoragePartitionConfig reported =
      blank_instance->GetSecurityPrincipal().GetStoragePartitionConfig();
  Report(reported.is_default(), "P2-about-blank-reports-default-partition",
         "an unassigned SiteInstance reports " +
             std::string(reported.is_default() ? "the DEFAULT partition"
                                               : "a named partition") +
             ", so its partition cannot be trusted for isolation");

  // (c) Tagging a blank page is still allowed (the tag records intent), but the
  //     commit-check must not pretend to have verified anything.
  content::WebContents::CreateParams create_params(profile_,
                                                  std::move(blank_instance));
  std::unique_ptr<content::WebContents> blank =
      content::WebContents::Create(create_params);
  if (!blank) {
    Report(false, "P2-about-blank-created", "could not create a blank contents");
    return;
  }

  ResolvedTabEnvironment resolved;
  resolved.active = true;
  resolved.environment_id = "test-a";
  resolved.partition = config;
  resolved.origin = EnvironmentOrigin::kExplicitRequest;
  Report(EnvironmentTabRouter::TagNewContents(blank.get(), resolved),
         "P2-about-blank-tagging-allowed",
         "TagNewContents() records the intended environment for a blank page");
  Report(EnvironmentTag::Get(blank.get()) == "test-a",
         "P2-about-blank-tagged-test-a", EnvironmentTag::Get(blank.get()));

  Report(EnvironmentTabRouter::TagMatchesCommittedPartition(blank.get(), nullptr,
                                                           nullptr),
         "P2-about-blank-no-false-mismatch",
         "no committed document, so the commit-check is vacuously true");

  // Do NOT navigate it: that is the path the browser now refuses, and navigating
  // here would just reproduce the leak inside the test.
  LOG(INFO) << "PARRA-TABOPS about-blank: not navigating, because navigation from a "
               "site-unassigned instance is the refused path";

  // (d) Drive the ACTUAL refusal in the real creation path, in this running browser.
  //
  // This is the "real negative case" the phase requires: a full Navigate() with an
  // empty-document URL and an explicitly requested non-default environment must
  // produce NO tab. Anything else would mean the page ended up somewhere.
  const int count_before = model_->count();
  NavigateParams params(browser_, GURL(url::kAboutBlankURL),
                        ui::PAGE_TRANSITION_TYPED);
  params.disposition = WindowOpenDisposition::kNewForegroundTab;
  params.parra_environment_id = "test-a";
  Navigate(&params);

  const int count_after = model_->count();
  Report(count_after == count_before,
         "P2-about-blank-creation-refused",
         "model count " + base::NumberToString(count_before) + " -> " +
             base::NumberToString(count_after) +
             "; no tab was created for an empty-document URL in 'test-a'");

  // And the same creation for PERSONAL must still work, so the refusal is specific
  // to the un-isolatable combination and not a blanket ban on about:blank.
  NavigateParams personal_params(browser_, GURL(url::kAboutBlankURL),
                                 ui::PAGE_TRANSITION_TYPED);
  personal_params.disposition = WindowOpenDisposition::kNewForegroundTab;
  personal_params.parra_environment_id = "personal";
  Navigate(&personal_params);
  const int count_personal = model_->count();
  Report(count_personal == count_after + 1,
         "P2-about-blank-personal-still-allowed",
         "model count " + base::NumberToString(count_after) + " -> " +
             base::NumberToString(count_personal) +
             "; about:blank in the default environment is still permitted");

  if (count_personal == count_after + 1) {
    // Clean the probe tab up so the following tests see the expected layout.
    const int probe_index = model_->active_index();
    if (probe_index != TabStripModel::kNoTab)
      model_->CloseWebContentsAt(probe_index, TabCloseTypes::CLOSE_NONE);
  }
}

void TabOpsSelfTest::TestAcceleratorPathIsCovered() {
  LOG(INFO) << "PARRA-TABOPS P1-accelerator: driving ExecuteContextMenuCommand() "
               "directly, exactly as browser_commands.cc does for "
               "IDC_CLOSE_OTHER_TABS / IDC_CLOSE_TABS_TO_RIGHT";
  DissolveAllGroups();

  // This test exists because the accelerators do NOT go through
  // BrowserTabStripController::ExecuteContextMenuCommand(). They call
  // TabStripModel::ExecuteContextMenuCommand() directly. If the containment lived
  // only in the controller, these commands would silently close every other
  // environment's hidden tabs. Driving the model method directly from here is
  // therefore the same call shape the accelerator uses -- not a simulation of it.

  // Give 'test-a' enough tabs that Close Tabs To Right has real victims inside the
  // environment, and record the foreign set beforehand.
  EnsureEnvironmentHasTabs("test-a", 3);
  mapper_.SetActiveEnvironment("test-a");

  const std::vector<int> visible = mapper_.GetVisibleGlobalIndices("test-a");
  if (visible.size() < 2) {
    Report(false, "P1-accelerator-precondition",
           "need at least two test-a tabs, have " +
               base::NumberToString(visible.size()));
    return;
  }

  // Snapshot every token that must survive: all tabs outside 'test-a'.
  std::vector<std::pair<std::string, std::string>> foreign;
  for (int i = 0; i < model_->count(); ++i) {
    const std::string tag = EnvironmentOfTab(model_->GetWebContentsAt(i));
    if (tag != "test-a") {
      foreign.emplace_back(Token(model_->GetWebContentsAt(i)), tag);
    }
  }

  // Activate the FIRST visible tab so everything to its right (in visible order)
  // becomes the victim set.
  const int context_index = visible.front();
  model_->ActivateTabAt(context_index);
  mapper_.SetActiveEnvironment("test-a");

  const int before_count = model_->count();

  // THE CALL THE ACCELERATOR MAKES. No controller involved.
  model_->ExecuteContextMenuCommand(context_index,
                                    TabStripModel::CommandCloseTabsToRight);

  // Every foreign tab must still be alive and still present.
  int still_present = 0;
  for (const auto& [token, tag] : foreign) {
    if (FindByToken(model_, token) != TabStripModel::kNoTab) {
      ++still_present;
    }
  }
  Report(still_present == static_cast<int>(foreign.size()),
         "P1-accelerator-close-right-foreign-intact",
         base::NumberToString(still_present) + "/" +
             base::NumberToString(foreign.size()) +
             " foreign tab(s) still alive after a direct "
             "ExecuteContextMenuCommand(CommandCloseTabsToRight)");

  // And the command must not have been a no-op either: the in-environment victims
  // were closed, which proves the policy let an approved set through instead of
  // blanket-refusing every accelerator close.
  Report(model_->count() < before_count,
         "P1-accelerator-close-right-still-works",
         "model count " + base::NumberToString(before_count) + " -> " +
             base::NumberToString(model_->count()) +
             "; the approved in-environment victims were closed");

  // Now the refusal half: drain 'test-b' to one tab, then drive the same
  // accelerator path there. With an empty victim set the model call must close
  // NOTHING -- before the policy was moved onto the model, this path would have
  // fallen through to the native global close.
  DissolveAllGroups();
  mapper_.SetActiveEnvironment("test-b");
  while (mapper_.GetVisibleCount("test-b") > 1) {
    const std::vector<int> v = mapper_.GetVisibleGlobalIndices("test-b");
    if (v.empty()) {
      break;
    }
    const int last = v.back();
    model_->CloseWebContentsAt(last, TabCloseTypes::CLOSE_NONE);
    if (model_->ContainsIndex(last) &&
        EnvironmentOfTab(model_->GetWebContentsAt(last)) == "test-b") {
      break;
    }
  }

  const std::vector<int> lonely = mapper_.GetVisibleGlobalIndices("test-b");
  if (lonely.size() != 1) {
    Report(false, "P1-accelerator-refuse-precondition",
           "could not reduce test-b to a single tab (have " +
               base::NumberToString(lonely.size()) + ")");
    return;
  }

  const int lonely_index = lonely.front();
  const std::string lonely_token = Token(model_->GetWebContentsAt(lonely_index));
  mapper_.SetActiveEnvironment("test-b");
  const int count_before_refusal = model_->count();

  model_->ExecuteContextMenuCommand(lonely_index,
                                    TabStripModel::CommandCloseOtherTabs);

  Report(model_->count() == count_before_refusal,
         "P1-accelerator-refusal-closes-nothing",
         "model count " + base::NumberToString(count_before_refusal) + " -> " +
             base::NumberToString(model_->count()) +
             "; an empty victim set through the accelerator path must abort");
  Report(FindByToken(model_, lonely_token) != TabStripModel::kNoTab,
         "P1-accelerator-refusal-keeps-context-tab",
         "context tab token " + lonely_token + " is still alive");
}

void TabOpsSelfTest::Run() {
  LOG(INFO) << "PARRA-TABOPS BEGIN model_count=" << model_->count()
            << " active_env='" << mapper_.active_environment() << "'";

  if (model_->count() < 3) {
    Report(false, "T0-precondition",
           "needs the 3x3 layout from --parra-poc-visibility; model has " +
               base::NumberToString(model_->count()) + " tab(s)");
    LOG(INFO) << "PARRA-TABOPS SUMMARY checks=" << g_checks
              << " failures=" << g_failures;
    return;
  }

  EnsureThreeEnvironments();

  // Order matters: the invariant check runs first, on the pristine layout, so a
  // pre-existing inconsistency is attributed correctly rather than to a close.
  TestTagMatchesPartition();
  TestRequestedEnvironmentRejected();
  TestDuplicateTab();

  // P0: tab groups must stay inside one environment.
  TestGroupStaysInsideOneEnvironment();
  TestCloseGroupDoesNotCrossEnvironments();

  // P2: about:blank must not be misread as Personal.
  TestAboutBlankPartitionSemantics();

  // P1: an empty victim set must refuse instead of falling back. Destructive
  // (drains an environment), so it runs before the close-command tests that need
  // a full layout.
  TestEmptyVictimSetRefuses();

  // P1: the close policy must be enforced on the MODEL so the keyboard
  // accelerators cannot bypass it. Runs before the destructive tests it needs a
  // full layout for, and after the empty-victim test so it can build its own.
  TestAcceleratorPathIsCovered();

  // Destructive tests, isolated by re-activating the environment each time.
  TestCloseTabsToRight();
  TestCrossEnvironmentMultiSelect();
  TestCloseOtherTabs();

  TestTagMatchesPartition();

  // The about:blank sub-test schedules its commit check ~2s out, so the summary and
  // browser shutdown must wait for it. Printing SUMMARY here would report a check
  // count taken before those assertions ran -- which is how a failure would get
  // silently dropped from the summary line.
  base::SingleThreadTaskRunner::GetCurrentDefault()->PostDelayedTask(
      FROM_HERE,
      base::BindOnce(
          [](TabStripModel* model, Profile* profile) {
            LOG(INFO) << "PARRA-TABOPS SUMMARY checks=" << g_checks
                      << " failures=" << g_failures << " result="
                      << (g_failures == 0 ? "ALL-PASS" : "FAILURES");

            for (const Row& row : Snapshot(profile, model)) {
              LOG(INFO) << "PARRA-TABOPS STRIP index=" << row.index
                        << " tag=" << row.tag << " actual=" << row.actual
                        << " token=" << row.token.substr(0, 8);
            }

            LOG(INFO) << "PARRA-TABOPS EXITING";
            BrowserWindowInterface* browser =
                GetLastActiveBrowserWindowInterfaceWithAnyProfile();
            if (browser)
              browser->GetWindow()->Close();
          },
          model_, profile_),
      base::Seconds(4));
}
}  // namespace

void MaybeRunTabOpsSelfTest() {
  if (!base::CommandLine::ForCurrentProcess()->HasSwitch(
          switches::kParraPocTabOpsSelfTest)) {
    return;
  }

  BrowserWindowInterface* browser =
      GetLastActiveBrowserWindowInterfaceWithAnyProfile();
  if (!browser) {
    LOG(ERROR) << "PARRA-TABOPS no browser window";
    return;
  }
  Profile* profile = browser->GetProfile();
  TabStripModel* model = browser->GetTabStripModel();
  if (!profile || !model) {
    LOG(ERROR) << "PARRA-TABOPS no profile/tab strip";
    return;
  }

  // Delayed so the visibility PoC has finished creating and committing the 3x3
  // layout; the test asserts on real partitions, which only exist after commit.
  TabOpsSelfTest* test = new TabOpsSelfTest(browser, profile, model);
  base::SingleThreadTaskRunner::GetCurrentDefault()->PostDelayedTask(
      FROM_HERE,
      base::BindOnce(
          [](TabOpsSelfTest* owned) {
            owned->Run();
            delete owned;
          },
          test),
      base::Seconds(6));
}

}  // namespace parra
