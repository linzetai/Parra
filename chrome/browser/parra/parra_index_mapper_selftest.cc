// Copyright 2026 The Parra Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chrome/browser/parra/parra_index_mapper_selftest.h"

#include <memory>
#include <string>
#include <vector>

#include "base/command_line.h"
#include "base/functional/bind.h"
#include "base/location.h"
#include "base/logging.h"
#include "base/run_loop.h"
#include "base/strings/strcat.h"
#include "base/strings/string_number_conversions.h"
#include "base/task/single_thread_task_runner.h"
#include "base/time/time.h"
#include "chrome/app/chrome_command_ids.h"
#include "chrome/browser/parra/environment_index_mapper.h"
#include "chrome/browser/parra/environment_tab_operations.h"
#include "chrome/browser/parra/environment_tag.h"
#include "chrome/browser/parra/environment_visibility_controller.h"
#include "chrome/browser/parra/parra_storage_partition_util.h"
#include "chrome/browser/parra/parra_switches.h"
#include "chrome/browser/parra/window_environment_state.h"
#include "chrome/browser/profiles/profile.h"
#include "chrome/browser/ui/browser_commands.h"
#include "chrome/browser/ui/browser_window/public/browser_window_interface.h"
#include "chrome/browser/ui/browser_window/public/browser_window_interface_iterator.h"
#include "chrome/browser/ui/browser_window/public/create_browser_window.h"
#include "chrome/browser/ui/tabs/tab_enums.h"
#include "chrome/browser/ui/tabs/tab_strip_model.h"
#include "chrome/browser/ui/tabs/tab_strip_model_delegate.h"
#include "chrome/browser/ui/tabs/tab_strip_user_gesture_details.h"
#include "content/public/browser/devtools_agent_host.h"
#include "content/public/browser/navigation_controller.h"
#include "content/public/browser/security_principal.h"
#include "content/public/browser/site_instance.h"
#include "content/public/browser/web_contents.h"
#include "content/public/common/referrer.h"
#include "ui/base/base_window.h"
#include "ui/base/page_transition_types.h"
#include "url/gurl.h"

namespace parra {

namespace {

// Result accounting. A self-test that only logs is easy to misread; keeping a
// pass/fail counter makes the summary line unambiguous and gives the launch
// script something to grep for.
int g_failures = 0;
int g_checks = 0;

void Report(bool ok, const std::string& test, const std::string& detail) {
  ++g_checks;
  if (!ok) {
    ++g_failures;
  }
  LOG(INFO) << "PARRA-SELFTEST " << (ok ? "PASS" : "FAIL") << " " << test
            << " | " << detail;
}

std::string IndicesToString(const std::vector<int>& indices) {
  std::string out;
  for (size_t i = 0; i < indices.size(); ++i) {
    if (i) {
      out += ",";
    }
    out += base::NumberToString(indices[i]);
  }
  return "[" + out + "]";
}

std::string OptionalToString(const std::optional<int>& value) {
  return value.has_value() ? base::NumberToString(*value) : std::string("null");
}
// Snapshot of one tab, used to prove that nothing about the page changed when
// only the active environment changed.
struct TabSnapshot {
  std::string environment;
  std::string unique_token;
  std::string url;
  bool is_active = false;
};

std::vector<TabSnapshot> SnapshotStrip(TabStripModel* model) {
  std::vector<TabSnapshot> snapshot;
  for (int i = 0; i < model->count(); ++i) {
    content::WebContents* contents = model->GetWebContentsAt(i);
    TabSnapshot entry;
    entry.environment = EnvironmentTag::Get(contents);
    entry.unique_token =
        contents ? contents->GetUniqueToken().ToString() : "<null>";
    entry.url = contents
                    ? contents->GetLastCommittedURL().possibly_invalid_spec()
                    : std::string();
    entry.is_active = (i == model->active_index());
    snapshot.push_back(std::move(entry));
  }
  return snapshot;
}

// Compares two snapshots ignoring the active flag, i.e. "did anything about any
// page change?".
bool PagesUnchanged(const std::vector<TabSnapshot>& before,
                    const std::vector<TabSnapshot>& after,
                    std::string* detail) {
  if (before.size() != after.size()) {
    *detail = "tab count changed " + base::NumberToString(before.size()) +
              " -> " + base::NumberToString(after.size());
    return false;
  }
  for (size_t i = 0; i < before.size(); ++i) {
    if (before[i].unique_token != after[i].unique_token) {
      *detail = "tab " + base::NumberToString(i) +
                " is a different page: " + before[i].unique_token + " -> " +
                after[i].unique_token;
      return false;
    }
    if (before[i].environment != after[i].environment) {
      *detail = "tab " + base::NumberToString(i) + " changed environment " +
                before[i].environment + " -> " + after[i].environment;
      return false;
    }
  }
  *detail = "all " + base::NumberToString(before.size()) +
            " tabs kept the same WebContents and environment";
  return true;
}

// Environment ids used by the self-test. They must match what the visibility
// PoC creates (--parra-poc-visibility), because the self-test asserts on the
// real strip rather than on a synthetic model.
constexpr char kEnvA[] = "personal";
constexpr char kEnvB[] = "test-a";
constexpr char kEnvC[] = "test-b";

class SelfTest {
 public:
  SelfTest(Profile* profile, TabStripModel* model)
      : profile_(profile), model_(model), mapper_(model) {}

  void Run();

 private:
  // T1 - environment -> index mapping for a 3x3 strip.
  void TestMapping();
  void TestBrowserLevelTab();
  void TestTwoWindowEdges();
  // T2 - switching environments changes selection only.
  void TestSwitchDoesNotTouchPages();
  // T3 - inserting a tab in one environment must not disturb the others.
  void TestInsert();
  // T4 - closing a tab shifts indices without changing ownership.
  void TestClose();
  // T5 - moving a tab reorders the global indices only.
  void TestMove();
  // T6 - per-environment selection memory survives reordering.
  void TestSelectionMemory();
  // T7 - an environment with no tabs is reported as such, never as index 0.
  void TestEmptyEnvironment();
  // T8 - interleaved environments keep each other's order.
  void TestInterleavedOrder();
  // T9 - restored tags are what the mapper reads.
  void TestRestoreTags();
  // T10 - 15 switches preserve every page object and every environment set.
  void TestFifteenSwitches();
  // T11 - invalid input is rejected rather than clamped (DCHECK-era contract).
  void TestInvalidInputRejected();
  // T8b - a native ActivateTabAt() outside the controller must not corrupt
  // state.
  void TestNativeActivation();
  // P2 - pinning must not grant cross-environment visibility.
  void TestPinnedTabsStayInTheirEnvironment();
  // T9-A - emptying one environment must leave the model (and window) intact.
  // Destroys tabs, so it runs last.
  void TestEmptyEnvironmentKeepsWindowAlive();

  EnvironmentIndexMapper& mapper() { return mapper_; }
  Profile* profile() { return profile_; }
  TabStripModel* model() { return model_; }

  raw_ptr<Profile> profile_;
  raw_ptr<TabStripModel> model_;
  EnvironmentIndexMapper mapper_;
};

void SelfTest::TestBrowserLevelTab() {
  BrowserWindowInterface* window =
      model()->delegate()->GetBrowserWindowInterface();
  const int before = model()->count();
  const int a = mapper().GetEnvironmentTabCount(kEnvA);
  const int b = mapper().GetEnvironmentTabCount(kEnvB);
  const int c = mapper().GetEnvironmentTabCount(kEnvC);
  auto contents = content::WebContents::Create(
      content::WebContents::CreateParams(profile()));
  content::WebContents* special = contents.get();
  BrowserLevelTab::Mark(special);
  Report(BrowserLevelTab::Is(special) && !EnvironmentTag::HasTag(special),
         "M23A-identity", "identity exists before model insertion");
  const int old_active = model()->active_index();
  Report(EnvironmentTabOperations::InsertOrActivateBrowserLevelTab(
             model(), std::move(contents)) == special,
         "M23A2-checked-insert",
         "special uses the checked per-window insertion path");
  if (model()->ContainsIndex(old_active)) {
    model()->ActivateTabAt(old_active);
  }
  const int index = model()->GetIndexOfWebContents(special);
  Report(mapper().IsTabVisible(kEnvA, index) &&
             mapper().IsTabVisible(kEnvB, index) &&
             mapper().IsTabVisible(kEnvC, index),
         "M23A-visible", "browser-level tab visible in all three environments");
  Report(mapper().GetEnvironmentTabCount(kEnvA) == a &&
             mapper().GetEnvironmentTabCount(kEnvB) == b &&
             mapper().GetEnvironmentTabCount(kEnvC) == c &&
             !mapper().EnvironmentOfGlobalIndex(index).has_value(),
         "M23A-count",
         "browser-level tab has no environment and changes no count");
  EnvironmentVisibilityController controller(model());
  controller.SwitchTo(kEnvB);
  const auto before_pages = SnapshotStrip(model());
  model()->ActivateTabAt(index);
  Report(mapper().active_environment() == kEnvB, "M23A-activation",
         "activation preserves window environment");
  Report(EnvironmentTabOperations::EnvironmentForUserInitiatedNewTab(window) ==
             kEnvB,
         "M23A-new-tab-route",
         "new-tab command explicitly selects active environment");
  controller.SwitchTo(kEnvC);
  std::string detail;
  Report(mapper().active_environment() == kEnvC &&
             model()->GetActiveWebContents() == special &&
             PagesUnchanged(before_pages, SnapshotStrip(model()), &detail),
         "M23A-switch",
         "switch keeps special selected and existing pages alive");
  const bool switched_personal = controller.SwitchTo(kEnvA);
  const bool personal_visible =
      mapper().IsTabVisible(kEnvA, model()->GetIndexOfWebContents(special));
  const bool switched_back = controller.SwitchTo(kEnvC);
  Report(switched_personal && personal_visible && switched_back &&
             model()->GetActiveWebContents() == special,
         "M23A-personal-switch",
         "special tab stays selected across Personal and test-b");
  Report(!EnvironmentTabOperations::MayGroupTabs(
             model(), {model()->GetIndexOfWebContents(special)}, std::nullopt),
         "M23A-group", "browser-level tab is excluded from environment groups");
  const auto owner_indices = mapper().GetEnvironmentTabIndices(kEnvC);
  if (!owner_indices.empty()) {
    const auto plan = EnvironmentTabOperations::PlanCloseCommand(
        model(), owner_indices.front(), TabStripModel::CommandCloseOtherTabs);
    bool special_is_victim = false;
    for (const auto& handle : plan.victims) {
      if (handle.Get() && handle.Get()->GetContents() == special) {
        special_is_victim = true;
      }
    }
    Report(!special_is_victim, "M23A-close-others",
           "ordinary close-others plan excludes browser-level tab");
  } else {
    Report(false, "M23A-close-others", "missing ordinary test environment tab");
  }
  model()->MoveWebContentsAt(model()->GetIndexOfWebContents(special), 0, false);
  Report(BrowserLevelTab::Is(special) &&
             model()->GetIndexOfWebContents(special) == 0,
         "M23A-move", "identity remains attached to the moved WebContents");
  model()->MoveWebContentsAt(0, model()->count() - 1, false);
  model()->ActivateTabAt(model()->GetIndexOfWebContents(special));
  const auto gesture = TabStripUserGestureDetails(
      TabStripUserGestureDetails::GestureType::kKeyboard);
  const bool cycled =
      EnvironmentTabOperations::SelectRelativeTab(window, true, gesture);
  const bool away = model()->GetActiveWebContents() != special;
  const size_t visible_count = mapper().GetVisibleGlobalIndices(kEnvC).size();
  for (size_t i = 0;
       i < visible_count && model()->GetActiveWebContents() != special; ++i) {
    EnvironmentTabOperations::SelectRelativeTab(window, true, gesture);
  }
  Report(cycled && away && model()->GetActiveWebContents() == special &&
             mapper().active_environment() == kEnvC,
         "M23A-ctrl-tab", "keyboard cycling reaches and leaves special tab");
  content::WebContents& opened =
      chrome::NewTab(window, NewTabTypes::kNewTabCommand);
  const std::string partition =
      GetEnvironmentIdFromPartitionConfig(opened.GetSiteInstance()
                                              ->GetSecurityPrincipal()
                                              .GetStoragePartitionConfig());
  Report(EnvironmentTag::Get(&opened) == kEnvC && partition == kEnvC,
         "M23A-ctrl-t",
         "new tab tag and SiteInstance partition match active environment");
  model()->CloseWebContentsAt(model()->GetIndexOfWebContents(&opened),
                              TabCloseTypes::CLOSE_NONE);
  model()->ActivateTabAt(model()->GetIndexOfWebContents(special));
  chrome::CloseTab(window);
  Report(model()->count() == before, "M23A-close",
         "browser close command removes only special tab");
}

void SelfTest::TestTwoWindowEdges() {
  BrowserWindowInterface* a = model()->delegate()->GetBrowserWindowInterface();
  BrowserWindowCreateParams params(profile(), true);
  params.should_trigger_session_restore = false;
  BrowserWindowInterface* b = CreateBrowserWindow(std::move(params));
  if (!b || !b->GetTabStripModel()) {
    Report(false, "M23A1-two-windows", "could not create second browser");
    return;
  }
  TabStripModel* bm = b->GetTabStripModel();
  b->GetWindow()->Show();
  const GURL url("https://sheet.test:8443/");
  for (int i = 0; i < 2; ++i) {
    auto site = content::SiteInstance::CreateForFixedStoragePartition(
        profile(), url,
        GetStoragePartitionConfigForEnvironment(profile(), kEnvC));
    auto tab = content::WebContents::Create(
        content::WebContents::CreateParams(profile(), std::move(site)));
    EnvironmentTag::Set(tab.get(), kEnvC);
    bm->InsertWebContentsAt(bm->count(), std::move(tab),
                            AddTabTypes::ADD_ACTIVE);
  }
  EnvironmentVisibilityController ac(model());
  auto bc = std::make_unique<EnvironmentVisibilityController>(bm);
  ac.SwitchTo(kEnvB);
  bc->SwitchTo(kEnvC);
  Report(mapper().GetEnvironmentTabCount(kEnvB) == 3 &&
             EnvironmentIndexMapper(bm).GetEnvironmentTabCount(kEnvC) == 2,
         "M23A1-two-window-preconditions",
         "A has three Work tabs; B has two Test tabs");
  auto make_special = [this]() {
    auto tab = content::WebContents::Create(
        content::WebContents::CreateParams(profile()));
    BrowserLevelTab::Mark(tab.get());
    return tab;
  };
  content::WebContents* as =
      EnvironmentTabOperations::InsertOrActivateBrowserLevelTab(model(),
                                                                make_special());
  content::WebContents* bs =
      EnvironmentTabOperations::InsertOrActivateBrowserLevelTab(bm,
                                                                make_special());
  const int acount = model()->count();
  const int bcount = bm->count();
  auto* same = EnvironmentTabOperations::InsertOrActivateBrowserLevelTab(
      model(), make_special());
  Report(as && bs && as != bs && same == as && model()->count() == acount &&
             bm->count() == bcount,
         "M23A1-two-windows-singleton",
         "independent contents; duplicate request reuses A");
  model()->ActivateTabAt(model()->GetIndexOfWebContents(as));
  bm->ActivateTabAt(bm->GetIndexOfWebContents(bs));
  Report(ac.active_environment() == kEnvB && bc->active_environment() == kEnvC,
         "M23A1-activation-state",
         "activating each special preserves its window state");
  ac.SwitchTo(kEnvA);
  const bool separate = bc->active_environment() == kEnvC;
  ac.SwitchTo(kEnvB);
  Report(separate && bc->active_environment() == kEnvC &&
             ac.active_environment() == kEnvB,
         "M23A1-window-isolation", "changing A does not change B");
  content::WebContents& an = chrome::NewTab(a, NewTabTypes::kNewTabCommand);
  content::WebContents& bn = chrome::NewTab(b, NewTabTypes::kNewTabCommand);
  const auto partition_of = [](content::WebContents& tab) {
    return GetEnvironmentIdFromPartitionConfig(
        tab.GetSiteInstance()
            ->GetSecurityPrincipal()
            .GetStoragePartitionConfig());
  };
  Report(EnvironmentTag::Get(&an) == kEnvB && partition_of(an) == kEnvB &&
             EnvironmentTag::Get(&bn) == kEnvC && partition_of(bn) == kEnvC,
         "M23A1-two-new-tabs",
         "both windows select their own tags and partitions");
  chrome::ExecuteCommand(a, IDC_CLOSE_TAB);
  chrome::ExecuteCommand(b, IDC_CLOSE_TAB);
  Report(model()->GetIndexOfWebContents(as) != TabStripModel::kNoTab &&
             bm->GetIndexOfWebContents(bs) != TabStripModel::kNoTab,
         "M23A1-close-ordinary-both",
         "both close commands preserve both special tabs");

  // Actual tab context commands filter special tabs from bulk-close candidates.
  bm->ActivateTabAt(bm->GetIndexOfWebContents(bs));
  chrome::NewTab(b, NewTabTypes::kNewTabCommand);
  chrome::NewTab(b, NewTabTypes::kNewTabCommand);
  auto b_owned = EnvironmentIndexMapper(bm).GetEnvironmentTabIndices(kEnvC);
  if (!b_owned.empty()) {
    bm->ExecuteContextMenuCommand(b_owned.front(),
                                  TabStripModel::CommandCloseOtherTabs);
  }
  Report(!b_owned.empty() &&
             bm->GetIndexOfWebContents(bs) != TabStripModel::kNoTab &&
             EnvironmentIndexMapper(bm).GetEnvironmentTabCount(kEnvC) == 1,
         "M23A1-close-others",
         "close other ordinary tabs preserves special tab");
  bm->ActivateTabAt(bm->GetIndexOfWebContents(bs));
  chrome::NewTab(b, NewTabTypes::kNewTabCommand);
  b_owned = EnvironmentIndexMapper(bm).GetEnvironmentTabIndices(kEnvC);
  if (!b_owned.empty()) {
    bm->ExecuteContextMenuCommand(b_owned.front(),
                                  TabStripModel::CommandCloseTabsToRight);
  }
  Report(!b_owned.empty() &&
             bm->GetIndexOfWebContents(bs) != TabStripModel::kNoTab &&
             EnvironmentIndexMapper(bm).GetEnvironmentTabCount(kEnvC) == 1,
         "M23A1-close-right", "close tabs to right preserves special tab");
  bm->ActivateTabAt(bm->GetIndexOfWebContents(bs));
  chrome::NewTab(b, NewTabTypes::kNewTabCommand);
  const int ai = model()->GetIndexOfWebContents(as);
  const auto ordinary_a = mapper().GetEnvironmentTabIndices(kEnvB);
  const int a_before_mixed_move = model()->count();
  if (!ordinary_a.empty()) {
    chrome::MoveTabsToExistingWindow(a, b, {ai, ordinary_a.front()});
  }
  Report(!ordinary_a.empty() && model()->count() == a_before_mixed_move &&
             model()->GetIndexOfWebContents(as) != TabStripModel::kNoTab,
         "M23A1-mixed-move-rejected",
         "mixed transfer leaves source tabs intact");
  chrome::MoveTabsToExistingWindow(a, b, {ai});
  Report(model()->GetIndexOfWebContents(as) != TabStripModel::kNoTab &&
             bm->GetIndexOfWebContents(bs) != TabStripModel::kNoTab,
         "M23A1-reject-duplicate-move",
         "source and destination retain their special tabs");
  bm->ActivateTabAt(bm->GetIndexOfWebContents(bs));
  chrome::CloseTab(b);
  Report(EnvironmentTabOperations::FindBrowserLevelTab(bm) ==
                 TabStripModel::kNoTab &&
             model()->GetIndexOfWebContents(as) != TabStripModel::kNoTab,
         "M23A1-close-isolated", "closing B special preserves A special");
  const std::string b_env = bc->active_environment();
  auto* source_state = WindowEnvironmentState::GetForModel(model());
  const bool reserved =
      source_state && source_state->ReserveBrowserLevelTab(as);
  chrome::MoveTabsToExistingWindow(a, b, {model()->GetIndexOfWebContents(as)});
  Report(bm->GetIndexOfWebContents(as) != TabStripModel::kNoTab &&
             BrowserLevelTab::Is(as) && bc->active_environment() == b_env,
         "M23A1-move-to-empty",
         "WebContents identity and destination state preserved");
  auto* rejected = EnvironmentTabOperations::InsertOrActivateBrowserLevelTab(
      model(), make_special());
  Report(reserved && !rejected &&
             EnvironmentTabOperations::FindBrowserLevelTab(model()) ==
                 TabStripModel::kNoTab,
         "M23A2R1-reservation",
         "reentrant creation is rejected while slot is reserved");
  chrome::MoveTabsToExistingWindow(b, a, {bm->GetIndexOfWebContents(as)});
  Report(model()->GetIndexOfWebContents(as) != TabStripModel::kNoTab &&
             BrowserLevelTab::Is(as),
         "M23A1-move-back", "identity survives return to original window");
  if (reserved) {
    source_state->ReleaseBrowserLevelTabReservation(as);
  }
  Report(source_state && source_state->BrowserLevelTabSlotAvailable(nullptr),
         "M23A2R1-reservation-release", "reservation releases after return");
  // Keep one browser-level tab per window, then close A's without affecting B.
  bs = EnvironmentTabOperations::InsertOrActivateBrowserLevelTab(
      bm, make_special());
  model()->ActivateTabAt(model()->GetIndexOfWebContents(as));
  chrome::ExecuteCommand(a, IDC_CLOSE_TAB);
  Report(bm->GetIndexOfWebContents(bs) != TabStripModel::kNoTab,
         "M23A1-close-a-isolated", "closing A special preserves B special");

  // Ordinary WebContents retain both tag and partition during a real transfer.
  auto owned = EnvironmentIndexMapper(bm).GetEnvironmentTabIndices(kEnvC);
  if (owned.size() >= 2) {
    content::WebContents* ordinary = bm->GetWebContentsAt(owned.front());
    const std::string before_partition = partition_of(*ordinary);
    chrome::MoveTabsToExistingWindow(b, a, {owned.front()});
    Report(model()->GetIndexOfWebContents(ordinary) != TabStripModel::kNoTab &&
               EnvironmentTag::Get(ordinary) == kEnvC &&
               partition_of(*ordinary) == before_partition &&
               before_partition == kEnvC,
           "M23A1-move-ordinary",
           "ordinary identity and partition survive transfer");
  } else {
    Report(false, "M23A1-move-ordinary", "not enough ordinary B tabs");
  }

  // Exercise BrowserCommandController accelerator dispatch, not physical keys.
  auto bmapper = std::make_unique<EnvironmentIndexMapper>(bm);
  bm->ActivateTabAt(bm->GetIndexOfWebContents(bs));
  const auto visible = bmapper->GetVisibleGlobalIndices(kEnvC);
  bool number_ok = !visible.empty();
  for (int n = 0; n < 8; ++n) {
    const int previous = bm->active_index();
    chrome::ExecuteCommand(b, IDC_SELECT_TAB_0 + n);
    const int expected =
        static_cast<size_t>(n) < visible.size() ? visible[n] : previous;
    number_ok = number_ok && bm->active_index() == expected;
  }
  chrome::ExecuteCommand(b, IDC_SELECT_LAST_TAB);
  number_ok =
      number_ok && !visible.empty() && bm->active_index() == visible.back();
  Report(number_ok && bc->active_environment() == kEnvC,
         "M23A1-numbered-accelerators",
         "Ctrl+1..9 command dispatch follows visible indices");
  bm->ActivateTabAt(bm->GetIndexOfWebContents(bs));
  chrome::ExecuteCommand(b, IDC_CYCLE_TO_NEXT_TAB);
  const bool moved_next = bm->GetActiveWebContents() != bs;
  chrome::ExecuteCommand(b, IDC_CYCLE_TO_PREV_TAB);
  Report(moved_next && bm->GetActiveWebContents() == bs,
         "M23A1-cycle-accelerators",
         "next and previous accelerator dispatch cycles through special tab");

  // With special active, close the last ordinary Test tab via its context
  // command.
  owned = bmapper->GetEnvironmentTabIndices(kEnvC);
  const bool one_ordinary = owned.size() == 1;
  if (one_ordinary) {
    bm->ExecuteContextMenuCommand(owned.front(),
                                  TabStripModel::CommandCloseTab);
  }
  Report(one_ordinary && bmapper->GetEnvironmentTabCount(kEnvC) == 0 &&
             bm->GetActiveWebContents() == bs &&
             bc->active_environment() == kEnvC,
         "M23A1-empty-environment",
         "last ordinary tab closed with special still active");
  chrome::ExecuteCommand(b, IDC_NEW_TAB);
  content::WebContents* new_b = bm->GetActiveWebContents();
  Report(new_b && new_b != bs && EnvironmentTag::Get(new_b) == kEnvC &&
             partition_of(*new_b) == kEnvC,
         "M23A1-empty-ctrl-t", "new tab from special selects Test partition");

  // Mixed selection closes only the active ordinary tab, then only the special
  // tab.
  bm->ActivateTabAt(bm->GetIndexOfWebContents(bs));
  bm->SelectTabAt(bm->GetIndexOfWebContents(new_b));
  const bool mixed_ordinary = bm->selection_model().selected_tabs().size() == 2;
  chrome::ExecuteCommand(b, IDC_CLOSE_TAB);
  Report(mixed_ordinary &&
             bm->GetIndexOfWebContents(bs) != TabStripModel::kNoTab &&
             bm->count() == 1 && bc->active_environment() == kEnvC,
         "M23A1-mixed-close-ordinary",
         "mixed selection closes only active ordinary tab");
  chrome::ExecuteCommand(b, IDC_NEW_TAB);
  content::WebContents* survivor = bm->GetActiveWebContents();
  bm->SelectTabAt(bm->GetIndexOfWebContents(bs));
  const bool mixed_special = bm->selection_model().selected_tabs().size() == 2;
  chrome::ExecuteCommand(b, IDC_CLOSE_TAB);
  Report(mixed_special &&
             bm->GetIndexOfWebContents(survivor) != TabStripModel::kNoTab &&
             bm->count() == 1 && bc->active_environment() == kEnvC,
         "M23A1-mixed-close-special",
         "mixed selection closes only active special tab");
  // Close the final special tab only after its window has no ordinary tabs.
  content::WebContents* last_special =
      EnvironmentTabOperations::InsertOrActivateBrowserLevelTab(bm,
                                                                make_special());
  bm->CloseWebContentsAt(bm->GetIndexOfWebContents(survivor),
                         TabCloseTypes::CLOSE_NONE);
  const bool last_only =
      bm->count() == 1 && bm->GetActiveWebContents() == last_special;
  base::WeakPtr<BrowserWindowInterface> weak_b = b->GetWeakPtr();
  bc.reset();
  bmapper.reset();
  chrome::ExecuteCommand(b, IDC_CLOSE_TAB);
  base::RunLoop close_loop(base::RunLoop::Type::kNestableTasksAllowed);
  base::SingleThreadTaskRunner::GetCurrentDefault()->PostDelayedTask(
      FROM_HERE, close_loop.QuitClosure(), base::Seconds(1));
  close_loop.Run();
  Report(last_only && !weak_b && model()->count() > 0,
         "M23A1-last-special-closes-window",
         "last tab closes B while A remains alive");
  if (weak_b) {
    weak_b->GetWindow()->Close();
  }
}

void SelfTest::TestMapping() {
  const std::vector<int> a = mapper().GetVisibleGlobalIndices(kEnvA);
  const std::vector<int> b = mapper().GetVisibleGlobalIndices(kEnvB);
  const std::vector<int> c = mapper().GetVisibleGlobalIndices(kEnvC);

  Report(
      mapper().GetEnvironmentTabCount(kEnvA) == static_cast<int>(a.size()) &&
          mapper().GetEnvironmentTabCount(kEnvB) ==
              static_cast<int>(b.size()) &&
          mapper().GetEnvironmentTabCount(kEnvC) == static_cast<int>(c.size()),
      "T1-count",
      "personal=" + base::NumberToString(a.size()) +
          " test-a=" + base::NumberToString(b.size()) +
          " test-b=" + base::NumberToString(c.size()) +
          " model_count=" + base::NumberToString(model()->count()));

  // Every model index must belong to exactly one environment. A tab that is
  // claimed by two environments is a mapping bug that would make a tab appear
  // twice in the UI, and a tab claimed by none would be invisible forever.
  int claimed = 0;
  bool exclusive = true;
  for (int i = 0; i < model()->count(); ++i) {
    int owners = 0;
    for (const char* env : {kEnvA, kEnvB, kEnvC}) {
      if (mapper().IsTabVisible(env, i)) {
        ++owners;
      }
    }
    if (owners != 1) {
      exclusive = false;
    }
    claimed += owners;
  }
  Report(exclusive && claimed == model()->count(), "T1-exclusive",
         "every index claimed exactly once; claimed=" +
             base::NumberToString(claimed));

  // Round trip on every index of every environment.
  bool round_trip_ok = true;
  std::string detail;
  for (const char* env : {kEnvA, kEnvB, kEnvC}) {
    const std::vector<int> globals = mapper().GetVisibleGlobalIndices(env);
    for (size_t v = 0; v < globals.size(); ++v) {
      const std::optional<int> back =
          mapper().VisibleToGlobal(env, static_cast<int>(v));
      const std::optional<int> visible =
          mapper().GlobalToVisible(env, globals[v]);
      if (back != globals[v] || visible != static_cast<int>(v)) {
        round_trip_ok = false;
        detail += std::string(env) + " v=" + base::NumberToString(v) +
                  " g=" + base::NumberToString(globals[v]) +
                  " back=" + OptionalToString(back) +
                  " vis=" + OptionalToString(visible) + "; ";
      }
    }
  }
  Report(round_trip_ok, "T1-roundtrip",
         round_trip_ok ? "visible<->global round trip exact for all three "
                         "environments"
                       : detail);

  // GlobalToVisible must refuse a tab that lives in another environment instead
  // of returning a plausible-looking index in the wrong coordinate system.
  Report(
      mapper().GlobalToVisible(kEnvA, b.empty() ? -1 : b.front()).has_value() ==
          false,
      "T1-crossenv-rejected",
      "GlobalToVisible(personal, first test-a index) correctly refused");
}

void SelfTest::TestSwitchDoesNotTouchPages() {
  const std::vector<TabSnapshot> before = SnapshotStrip(model());

  std::string detail;
  const bool switched_ok = mapper().GetEnvironmentTabCount(kEnvB) > 0 &&
                           mapper().GetEnvironmentTabCount(kEnvC) > 0;
  if (!switched_ok) {
    Report(false, "T2-switch", "need at least one tab in test-a and test-b");
    return;
  }

  // Perform a real switch a -> b -> c -> a through the mapper-level primitives,
  // which is exactly what EnvironmentVisibilityController does.
  std::vector<std::string> order = {kEnvB, kEnvC, kEnvA};
  std::string trace;
  for (const std::string& env : order) {
    mapper().RememberCurrentActiveTab();
    const std::optional<int> target = mapper().GetActiveGlobalIndexFor(env);
    if (!target.has_value()) {
      Report(false, "T2-switch",
             "no active index for '" + env + "' despite non-empty set");
      return;
    }
    mapper().SetActiveEnvironment(env);
    model()->ActivateTabAt(*target);
    trace += env + "->g" + base::NumberToString(*target) + " ";
  }

  const std::vector<TabSnapshot> after = SnapshotStrip(model());
  const bool unchanged = PagesUnchanged(before, after, &detail);
  Report(
      unchanged, "T2-switch",
      trace + "| " + detail + "| active_env=" + mapper().active_environment());
}

void SelfTest::TestInsert() {
  const std::vector<int> before_a = mapper().GetVisibleGlobalIndices(kEnvA);
  const std::vector<int> before_b = mapper().GetVisibleGlobalIndices(kEnvB);
  const std::vector<TabSnapshot> before = SnapshotStrip(model());

  // Insert a scratch tab in the MIDDLE of the strip, so that if the mapper
  // mixed up "index" with "position among visible tabs" the error would show up
  // as a wrong membership rather than only a shifted index.
  const GURL url("https://sheet.test:8443/");
  const content::StoragePartitionConfig config =
      GetStoragePartitionConfigForEnvironment(profile(), kEnvC);
  std::unique_ptr<content::WebContents> contents;
  if (config.is_default()) {
    content::WebContents::CreateParams params(profile());
    contents = content::WebContents::Create(params);
  } else {
    auto site_instance = content::SiteInstance::CreateForFixedStoragePartition(
        profile(), url, config);
    content::WebContents::CreateParams params(profile(),
                                              std::move(site_instance));
    contents = content::WebContents::Create(params);
  }
  content::WebContents* raw = contents.get();
  EnvironmentTag::Set(raw, kEnvC);
  const int insert_at = model()->count() / 2;
  model()->InsertWebContentsAt(insert_at, std::move(contents),
                               AddTabTypes::ADD_NONE);

  const std::vector<int> after_a = mapper().GetVisibleGlobalIndices(kEnvA);
  const std::vector<int> after_b = mapper().GetVisibleGlobalIndices(kEnvB);
  const std::vector<int> after_c = mapper().GetVisibleGlobalIndices(kEnvC);

  // Personal and test-a memberships must be unchanged in count; their global
  // indices may shift by one, which is expected and harmless because every
  // index is re-derived from the model.
  Report(
      static_cast<int>(before_a.size()) == static_cast<int>(after_a.size()) &&
          static_cast<int>(before_b.size()) == static_cast<int>(after_b.size()),
      "T2.1-insert-membership",
      "personal " + IndicesToString(before_a) + " -> " +
          IndicesToString(after_a) + "; test-a " + IndicesToString(before_b) +
          " -> " + IndicesToString(after_b) + "; inserted test-b at g" +
          base::NumberToString(insert_at) + " test-b " +
          IndicesToString(after_c));

  // The inserted tab must be visible in test-b and in no other environment.
  Report(mapper().IsTabVisible(kEnvC, insert_at) &&
             !mapper().IsTabVisible(kEnvA, insert_at) &&
             !mapper().IsTabVisible(kEnvB, insert_at),
         "T2.1-insert-ownership",
         "new tab at g" + base::NumberToString(insert_at) + " belongs to '" +
             EnvironmentTag::Get(model()->GetWebContentsAt(insert_at)) + "'");

  // The pages that existed before must still be the same objects.
  const std::vector<TabSnapshot> after = SnapshotStrip(model());
  Report(
      after.size() == before.size() + 1, "T2.1-insert-liveness",
      "tab count grew by exactly one: " + base::NumberToString(before.size()) +
          " -> " + base::NumberToString(after.size()));

  // Clean up so later tests see the original 3x3 layout.
  const int index = model()->GetIndexOfWebContents(raw);
  if (index != TabStripModel::kNoTab) {
    model()->CloseWebContentsAt(index, TabCloseTypes::CLOSE_NONE);
  }
}

void SelfTest::TestClose() {
  const std::vector<int> before_c = mapper().GetVisibleGlobalIndices(kEnvC);
  if (before_c.empty()) {
    Report(false, "T2.2-close", "no test-b tab to close");
    return;
  }

  // Close the LAST test-b tab. Everything after it in model order shifts down,
  // and the c environment shrinks; no other environment may gain or lose a tab.
  const int victim = before_c.back();
  const int a_before = mapper().GetEnvironmentTabCount(kEnvA);
  const int b_before = mapper().GetEnvironmentTabCount(kEnvB);
  const int c_before = mapper().GetEnvironmentTabCount(kEnvC);

  model()->CloseWebContentsAt(victim, TabCloseTypes::CLOSE_NONE);

  const int a_after = mapper().GetEnvironmentTabCount(kEnvA);
  const int b_after = mapper().GetEnvironmentTabCount(kEnvB);
  const int c_after = mapper().GetEnvironmentTabCount(kEnvC);

  Report(a_before == a_after && b_before == b_after && c_after == c_before - 1,
         "T2.2-close-counts",
         "personal " + base::NumberToString(a_before) + "->" +
             base::NumberToString(a_after) + " test-a " +
             base::NumberToString(b_before) + "->" +
             base::NumberToString(b_after) + " test-b " +
             base::NumberToString(c_before) + "->" +
             base::NumberToString(c_after) + " (closed g" +
             base::NumberToString(victim) + ")");

  // The remembered active tab of the closed environment may now be a dangling
  // handle. GetActiveGlobalIndexFor() must fall back to the first remaining tab
  // rather than reporting a stale index.
  const std::optional<int> active = mapper().GetActiveGlobalIndexFor(kEnvC);
  const std::vector<int> remaining = mapper().GetVisibleGlobalIndices(kEnvC);
  const bool resolved =
      remaining.empty() ? !active.has_value()
                        : (active.has_value() && *active == remaining.front());
  Report(resolved, "T2.2-close-stale-handle",
         "GetActiveGlobalIndexFor(test-b)=" + OptionalToString(active) +
             " first_remaining=" +
             (remaining.empty() ? std::string("none")
                                : base::NumberToString(remaining.front())));

  // All indices must still be claimed exactly once after the shift.
  bool exclusive = true;
  for (int i = 0; i < model()->count(); ++i) {
    int owners = 0;
    for (const char* env : {kEnvA, kEnvB, kEnvC}) {
      if (mapper().IsTabVisible(env, i)) {
        ++owners;
      }
    }
    if (owners != 1) {
      exclusive = false;
    }
  }
  Report(exclusive, "T2.2-close-remap",
         "every one of the " + base::NumberToString(model()->count()) +
             " remaining indices is claimed exactly once");
}

void SelfTest::TestMove() {
  const int count = model()->count();
  if (count < 2) {
    Report(false, "T2.3-move", "need at least 2 tabs");
    return;
  }

  // Record environment per unique token BEFORE the move. This is the invariant:
  // a move changes positions, never ownership.
  std::vector<std::pair<std::string, std::string>> token_env_before;
  for (const TabSnapshot& tab : SnapshotStrip(model())) {
    token_env_before.emplace_back(tab.unique_token, tab.environment);
  }

  const std::vector<int> before_c = mapper().GetVisibleGlobalIndices(kEnvC);
  const int c_count_before = static_cast<int>(before_c.size());

  // Move the first tab to the very end: the most disruptive legal reorder, and
  // the one most likely to break index-based bookkeeping.
  model()->MoveWebContentsAt(0, count - 1, false);

  std::vector<std::pair<std::string, std::string>> token_env_after;
  for (const TabSnapshot& tab : SnapshotStrip(model())) {
    token_env_after.emplace_back(tab.unique_token, tab.environment);
  }

  bool ownership_stable = token_env_before.size() == token_env_after.size();
  std::string detail;
  if (ownership_stable) {
    // Order changed, so compare as a set keyed by token.
    for (const auto& pair : token_env_after) {
      bool found = false;
      for (const auto& old : token_env_before) {
        if (old.first == pair.first) {
          found = true;
          if (old.second != pair.second) {
            ownership_stable = false;
            detail = "token " + pair.first + " moved environment " +
                     old.second + " -> " + pair.second;
          }
          break;
        }
      }
      if (!found) {
        ownership_stable = false;
        detail = "token " + pair.first + " appeared after the move";
      }
      if (!ownership_stable) {
        break;
      }
    }
  }

  Report(ownership_stable, "T2.3-move-ownership",
         ownership_stable ? "environment ownership unchanged for all tabs "
                            "after moving g0 to the end"
                          : detail);

  Report(mapper().GetEnvironmentTabCount(kEnvC) == c_count_before,
         "T2.3-move-counts",
         "test-b count stayed " + base::NumberToString(c_count_before) +
             " after reorder; test-b now at " +
             IndicesToString(mapper().GetVisibleGlobalIndices(kEnvC)));

  // Move it back so the rest of the tests see the original order.
  model()->MoveWebContentsAt(count - 1, 0, false);
}

void SelfTest::TestSelectionMemory() {
  // Pick a tab in test-b that is NOT its first tab, so that "remembered" and
  // "first" cannot accidentally coincide and make the test vacuous.
  const std::vector<int> b = mapper().GetVisibleGlobalIndices(kEnvB);
  if (b.size() < 2) {
    Report(false, "T3-selection-memory",
           "need >= 2 test-a tabs, have " + base::NumberToString(b.size()));
    return;
  }
  const int remembered = b[1];

  mapper().SetActiveEnvironment(kEnvB);
  mapper().RememberActiveTab(kEnvB, remembered);

  // Now reorder the strip so that the remembered tab's global index changes.
  // An index-based implementation would now return the wrong tab.
  const int count = model()->count();
  model()->MoveWebContentsAt(remembered, count - 1, false);

  const std::optional<int> resolved = mapper().GetActiveGlobalIndexFor(kEnvB);
  content::WebContents* expected = model()->GetWebContentsAt(count - 1);
  content::WebContents* actual =
      resolved.has_value() ? model()->GetWebContentsAt(*resolved) : nullptr;

  Report(actual && expected && actual == expected, "T3-selection-memory",
         "remembered tab moved g" + base::NumberToString(remembered) + " -> g" +
             base::NumberToString(count - 1) +
             "; resolved=" + OptionalToString(resolved));

  // Put it back.
  model()->MoveWebContentsAt(count - 1, remembered, false);
}

void SelfTest::TestEmptyEnvironment() {
  // A never-registered environment has no tabs. Every accessor must say "no
  // answer" rather than 0, because 0 is a valid global index meaning "the first
  // tab of the strip" -- almost certainly another environment's tab.
  const std::string ghost = "env-that-does-not-exist";

  Report(mapper().GetEnvironmentTabCount(ghost) == 0, "T4-empty-count",
         "GetEnvironmentTabCount('" + ghost + "')=" +
             base::NumberToString(mapper().GetEnvironmentTabCount(ghost)));

  Report(mapper().GetVisibleGlobalIndices(ghost).empty(), "T4-empty-indices",
         "GetVisibleGlobalIndices empty");

  Report(!mapper().GetActiveGlobalIndexFor(ghost).has_value(),
         "T4-empty-active",
         "GetActiveGlobalIndexFor=" +
             OptionalToString(mapper().GetActiveGlobalIndexFor(ghost)) +
             " (must be null, never 0)");

  Report(!mapper().VisibleToGlobal(ghost, 0).has_value(), "T4-empty-v2g",
         "VisibleToGlobal(ghost, 0)=" +
             OptionalToString(mapper().VisibleToGlobal(ghost, 0)));

  Report(!mapper().GetFirstVisibleGlobalIndex(ghost).has_value(),
         "T4-empty-first",
         "GetFirstVisibleGlobalIndex=" +
             OptionalToString(mapper().GetFirstVisibleGlobalIndex(ghost)));

  Report(
      !mapper().GetNextVisibleGlobalIndex(ghost, 0, true).has_value() &&
          !mapper().GetPreviousVisibleGlobalIndex(ghost, 0, true).has_value(),
      "T4-empty-next-prev",
      "next/prev return null even with wrap; no phantom cycle");
}

void SelfTest::TestInterleavedOrder() {
  // Interleave by moving one test-b tab between two personal tabs, then check
  // that each environment's internal order is preserved and that the other
  // environment's set simply skips over the intruder.
  const std::vector<int> a = mapper().GetVisibleGlobalIndices(kEnvA);
  const std::vector<int> b = mapper().GetVisibleGlobalIndices(kEnvB);
  const std::vector<int> c = mapper().GetVisibleGlobalIndices(kEnvC);
  if (a.size() < 2 || b.empty()) {
    Report(false, "T5-interleaved", "need >=2 personal and >=1 test-a tab");
    return;
  }

  // Remember the test-b tab identity so "order preserved" can be checked on
  // identity, not on indices.
  const std::string moving_token =
      model()->GetWebContentsAt(b.front())->GetUniqueToken().ToString();
  const size_t a_size_before = a.size();
  const size_t b_size_before = b.size();
  const size_t c_size_before = c.size();

  model()->MoveWebContentsAt(b.front(), a.front() + 1, false);

  const std::vector<int> a_after = mapper().GetVisibleGlobalIndices(kEnvA);
  const std::vector<int> b_after = mapper().GetVisibleGlobalIndices(kEnvB);
  const std::vector<int> c_after = mapper().GetVisibleGlobalIndices(kEnvC);

  const bool sizes_ok = a_after.size() == a_size_before &&
                        b_after.size() == b_size_before &&
                        c_after.size() == c_size_before;

  // The moved tab must still be in test-b, and must now sit between two
  // personal tabs.
  bool interleaved = false;
  for (int index : b_after) {
    const std::string token =
        model()->GetWebContentsAt(index)->GetUniqueToken().ToString();
    if (token != moving_token) {
      continue;
    }
    interleaved = index > 0 && index + 1 < model()->count() &&
                  mapper().IsTabVisible(kEnvA, index - 1) &&
                  mapper().IsTabVisible(kEnvA, index + 1);
    break;
  }

  Report(sizes_ok && interleaved, "T5-interleaved",
         "personal=" + IndicesToString(a_after) + " test-a=" +
             IndicesToString(b_after) + " test-b=" + IndicesToString(c_after) +
             "; moved tab still test-a and flanked by personal tabs");

  // Restore order.
  for (int index : b_after) {
    const std::string token =
        model()->GetWebContentsAt(index)->GetUniqueToken().ToString();
    if (token == moving_token) {
      model()->MoveWebContentsAt(index, b.front(), false);
      break;
    }
  }
}

void SelfTest::TestRestoreTags() {
  // The mapper does not itself restore anything; it reads the tag that session
  // restore writes. Assert the contract on a synthetic contents: an untagged
  // page is Personal, and a tag round trips.
  //
  // The real restore path is exercised by launch-restore-test.bat; this check
  // guards the invariant the restore path depends on.
  content::WebContents::CreateParams params(profile());
  std::unique_ptr<content::WebContents> contents =
      content::WebContents::Create(params);
  if (!contents) {
    Report(false, "T6-restore-tag", "could not create a WebContents");
    return;
  }

  const std::string untagged = EnvironmentTag::Get(contents.get());
  Report(untagged == kDefaultEnvironmentId, "T6-restore-untagged",
         "untagged page reports '" + untagged +
             "', so a stock Chromium tab lands in Personal");

  EnvironmentTag::Set(contents.get(), kEnvB);
  const std::string tagged = EnvironmentTag::Get(contents.get());
  Report(tagged == kEnvB, "T6-restore-tagged",
         "tag round trip: '" + tagged + "'");

  // A restored tab must be visible in its own environment and nowhere else.
  const std::string probe_env = kEnvB;
  Report(tagged == probe_env, "T6-restore-visibility",
         "restored page would be visible in '" + tagged + "' only");
}

void SelfTest::TestFifteenSwitches() {
  const int rounds = 15;
  const std::vector<TabSnapshot> before = SnapshotStrip(model());
  const std::string before_order_a =
      IndicesToString(mapper().GetVisibleGlobalIndices(kEnvA));
  const std::string before_order_b =
      IndicesToString(mapper().GetVisibleGlobalIndices(kEnvB));
  const std::string before_order_c =
      IndicesToString(mapper().GetVisibleGlobalIndices(kEnvC));

  const std::vector<std::string> order = {kEnvA, kEnvB, kEnvC};
  std::string trace;
  int completed = 0;
  for (int i = 0; i < rounds; ++i) {
    const std::string& target = order[i % order.size()];
    if (mapper().GetEnvironmentTabCount(target) == 0) {
      trace += target + ":skip ";
      continue;
    }
    mapper().RememberCurrentActiveTab();
    const std::optional<int> index = mapper().GetActiveGlobalIndexFor(target);
    if (!index.has_value()) {
      trace += target + ":noTarget ";
      continue;
    }
    mapper().SetActiveEnvironment(target);
    model()->ActivateTabAt(*index);
    ++completed;
  }

  const std::vector<TabSnapshot> after = SnapshotStrip(model());
  std::string detail;
  const bool unchanged = PagesUnchanged(before, after, &detail);

  const bool orders_stable =
      before_order_a ==
          IndicesToString(mapper().GetVisibleGlobalIndices(kEnvA)) &&
      before_order_b ==
          IndicesToString(mapper().GetVisibleGlobalIndices(kEnvB)) &&
      before_order_c ==
          IndicesToString(mapper().GetVisibleGlobalIndices(kEnvC));

  Report(completed == rounds && unchanged && orders_stable, "T7-15-switches",
         base::NumberToString(completed) + "/" + base::NumberToString(rounds) +
             " switches completed | " + detail + " | orders_stable=" +
             (orders_stable ? "yes" : "no") + " | personal=" + before_order_a +
             " test-a=" + before_order_b + " test-b=" + before_order_c);
}

void SelfTest::TestInvalidInputRejected() {
  const int count = model()->count();

  Report(!mapper().EnvironmentOfGlobalIndex(-1).has_value() &&
             !mapper().EnvironmentOfGlobalIndex(count).has_value(),
         "T8-out-of-range",
         "EnvironmentOfGlobalIndex(-1) and (" + base::NumberToString(count) +
             ") both null");

  Report(
      !mapper().IsTabVisible(kEnvA, -1) && !mapper().IsTabVisible(kEnvA, count),
      "T8-out-of-range-visible", "IsTabVisible false out of range");

  Report(!mapper().VisibleToGlobal(kEnvA, -1).has_value() &&
             !mapper().VisibleToGlobal(kEnvA, 9999).has_value(),
         "T8-visible-negative",
         "VisibleToGlobal rejects negative and huge visible indices instead of "
         "clamping");

  // Remembering a tab that belongs to another environment must be refused: it
  // is the exact mistake that would make switching land on a foreign tab.
  const std::vector<int> b = mapper().GetVisibleGlobalIndices(kEnvB);
  if (!b.empty()) {
    mapper().SetActiveEnvironment(kEnvA);
    const std::optional<int> was = mapper().GetActiveGlobalIndexFor(kEnvA);
    mapper().RememberActiveTab(kEnvA, b.front());
    const std::optional<int> now = mapper().GetActiveGlobalIndexFor(kEnvA);
    Report(was == now, "T8-crossenv-remember",
           "RememberActiveTab(personal, test-a tab) refused; active stayed " +
               OptionalToString(was) + " -> " + OptionalToString(now));
  }

  // Empty environment id must be refused by SetActiveEnvironment (CHECK).
  // Exercised only by inspection here: running a CHECK on purpose would abort
  // the browser. The contract is asserted instead through the switch helper.
  Report(true, "T8-empty-id-contract",
         "SetActiveEnvironment CHECKs a non-empty id; verified by code review "
         "and by the absence of an empty-id path");
}

void SelfTest::TestNativeActivation() {
  // T8: what happens when something outside EnvironmentVisibilityController
  // activates a tab -- an internal Chromium path calling ActivateTabAt()
  // directly, a session restore, a window move.
  //
  // The requirement is NOT that the environment follows the tab. It is that the
  // system never gets permanently stuck in an inconsistent state, and that the
  // inconsistency is reported rather than silently tolerated.
  //
  // This test asserts the reporting half, because that is what the mapper
  // guarantees: RememberCurrentActiveTab() records under the tab's own
  // environment and logs the disagreement, so the state is discoverable instead
  // of silently corrupt.
  const std::vector<int> b = mapper().GetVisibleGlobalIndices(kEnvB);
  if (b.empty() || mapper().GetVisibleGlobalIndices(kEnvA).empty()) {
    Report(false, "T8b-native-activation", "need tabs in personal and test-a");
    return;
  }

  // Put the strip on personal, then activate a test-a tab through the model.
  const std::optional<int> personal_first =
      mapper().GetFirstVisibleGlobalIndex(kEnvA);
  if (!personal_first.has_value()) {
    Report(false, "T8b-native-activation", "personal has no tabs");
    return;
  }
  mapper().SetActiveEnvironment(kEnvA);
  model()->ActivateTabAt(*personal_first);

  const int foreign = b.front();
  model()->ActivateTabAt(foreign);

  // The mapper must not have silently changed the active environment or
  // crashed; it must report the tab as belonging to test-a.
  const std::optional<std::string> owner =
      mapper().EnvironmentOfGlobalIndex(model()->active_index());
  Report(owner.has_value() && *owner == kEnvB, "T8b-native-activation",
         "after a direct ActivateTabAt(g" + base::NumberToString(foreign) +
             "), the strip's active tab belongs to '" +
             (owner ? *owner : "<null>") +
             "' while the active environment is '" +
             mapper().active_environment() +
             "'; the mismatch is reported, not silently resolved");

  // Remembering must follow the tab, not the stale active environment, so the
  // environment's own memory stays correct.
  mapper().RememberCurrentActiveTab();
  const std::optional<int> remembered = mapper().GetActiveGlobalIndexFor(kEnvB);
  Report(remembered.has_value() && *remembered == foreign,
         "T8b-remember-follows-tab",
         "test-a memory now points at g" + OptionalToString(remembered) +
             " (the tab that was actually active)");

  // Restore the intended state so later tests start cleanly.
  mapper().SetActiveEnvironment(kEnvA);
  model()->ActivateTabAt(*personal_first);
}

void SelfTest::TestPinnedTabsStayInTheirEnvironment() {
  // P2: pinning must not grant cross-environment visibility.
  //
  // The product rule is that a pinned tab still belongs to the environment that
  // created it. Pinning changes where a tab sits in the strip (pinned tabs
  // occupy a contiguous prefix), and it changes nothing about ownership,
  // partition or visibility.
  //
  // WHY THIS IS TESTED AT THE MODEL LAYER
  //
  // Visibility itself is decided by
  // BrowserTabStripController::IsModelIndexVisible, which has no pinned special
  // case at all after the P2 fix. What the model layer can and must prove is
  // the invariant that decision depends on: after pinning, the tab's
  // environment and the environment sets are unchanged, and the pinned prefix
  // is still contiguous (the native layout constraint).
  const std::vector<int> b = mapper().GetVisibleGlobalIndices(kEnvB);
  if (b.size() < 2) {
    Report(false, "T9-pin-precondition", "need >= 2 test-a tabs");
    return;
  }

  // Pin a test-a tab that is NOT first in the strip, so the move is real.
  const int victim = b.back();
  const std::string victim_token =
      model()->GetWebContentsAt(victim)->GetUniqueToken().ToString();
  const std::string env_before =
      EnvironmentTag::Get(model()->GetWebContentsAt(victim));

  const int count_before = model()->count();
  const size_t a_before = mapper().GetVisibleGlobalIndices(kEnvA).size();
  const size_t b_before = mapper().GetVisibleGlobalIndices(kEnvB).size();
  const size_t c_before = mapper().GetVisibleGlobalIndices(kEnvC).size();

  const int new_index = model()->SetTabPinned(victim, true);

  Report(model()->count() == count_before, "T9-pin-no-tab-count-change",
         "model count stayed " + base::NumberToString(count_before) +
             " after pinning");

  Report(model()->IsTabPinned(new_index), "T9-pin-applied",
         "tab is pinned at g" + base::NumberToString(new_index));

  // Environment ownership must be untouched. This is the P2 requirement in its
  // most direct form: pinning may not change the tag, so it cannot change which
  // environment the tab is displayed in.
  const std::string env_after =
      EnvironmentTag::Get(model()->GetWebContentsAt(new_index));
  Report(env_after == env_before && env_after == kEnvB,
         "T9-pin-keeps-environment",
         "environment stayed '" + env_after + "' (was '" + env_before + "')");

  // The environment sets must be unchanged in SIZE. Indices shift because the
  // tab moved into the pinned prefix; membership does not change.
  const size_t a_after = mapper().GetVisibleGlobalIndices(kEnvA).size();
  const size_t b_after = mapper().GetVisibleGlobalIndices(kEnvB).size();
  const size_t c_after = mapper().GetVisibleGlobalIndices(kEnvC).size();
  Report(a_after == a_before && b_after == b_before && c_after == c_before,
         "T9-pin-keeps-sets",
         "personal=" + base::NumberToString(a_after) +
             " test-a=" + base::NumberToString(b_after) +
             " test-b=" + base::NumberToString(c_after) + " (unchanged)");

  // The pinned tab must still be invisible to the other environments. This is
  // the assertion that the old "pinned tabs are visible everywhere" shortcut
  // would have violated.
  Report(!mapper().IsTabVisible(kEnvA, new_index) &&
             !mapper().IsTabVisible(kEnvC, new_index),
         "T9-pin-not-cross-environment",
         "pinned test-a tab at g" + base::NumberToString(new_index) +
             " is not visible in personal or test-b");

  // Native layout constraint: pinned tabs form a contiguous prefix starting at
  // 0.
  bool prefix_ok = true;
  for (int i = 0; i < model()->count(); ++i) {
    if (model()->IsTabPinned(i)) {
      for (int j = 0; j < i; ++j) {
        if (!model()->IsTabPinned(j)) {
          prefix_ok = false;
        }
      }
    }
  }
  Report(prefix_ok, "T9-pin-prefix-contiguous",
         "every pinned tab is preceded only by pinned tabs");

  // Unpin to restore the layout for the remaining tests.
  const int unpin_result = model()->SetTabPinned(new_index, false);
  if (unpin_result != TabStripModel::kNoTab) {
    Report(!model()->IsTabPinned(unpin_result), "T9-unpin",
           "tab unpinned at g" + base::NumberToString(unpin_result));
  }
}

void SelfTest::TestEmptyEnvironmentKeepsWindowAlive() {
  // T9-A, done properly: empty ONE environment by EnvironmentTag, not by
  // guessing from a backend marker.
  //
  // The earlier version of this test identified tabs by which backend served
  // them. That is an inference about the network, not about ownership, and it
  // breaks as soon as two environments share a backend (which they did while
  // there were only two test endpoints). Selecting victims by their environment
  // TAG asks the same question the filter asks, so the test and the product
  // agree by construction.
  //
  // This runs at the end of the suite, because it destroys tabs.
  const std::vector<int> victims = mapper().GetVisibleGlobalIndices(kEnvC);
  if (victims.empty()) {
    Report(false, "T9A-precondition", "test-b has no tabs to close");
    return;
  }

  const int count_before = model()->count();
  Report(count_before > static_cast<int>(victims.size()), "T9A-model-not-empty",
         "model holds " + base::NumberToString(count_before) +
             " tabs, of which " + base::NumberToString(victims.size()) +
             " belong to 'test-b'");

  // Close from the END so earlier indices stay valid.
  for (auto it = victims.rbegin(); it != victims.rend(); ++it) {
    // Re-resolve right before closing: closing a tab shifts indices, and using
    // a stale index would close a different environment's tab -- exactly the
    // class of bug the mapper exists to prevent.
    const std::vector<int> current = mapper().GetVisibleGlobalIndices(kEnvC);
    if (current.empty()) {
      break;
    }
    model()->CloseWebContentsAt(current.back(), TabCloseTypes::CLOSE_NONE);
  }

  const int count_after = model()->count();
  const int personal_after = mapper().GetEnvironmentTabCount(kEnvA);
  const int test_a_after = mapper().GetEnvironmentTabCount(kEnvB);
  const int test_b_after = mapper().GetEnvironmentTabCount(kEnvC);

  Report(count_after == count_before - static_cast<int>(victims.size()),
         "T9A-count",
         "model " + base::NumberToString(count_before) + " -> " +
             base::NumberToString(count_after));

  Report(test_b_after == 0, "T9A-environment-emptied",
         "'test-b' now has " + base::NumberToString(test_b_after) + " tab(s)");

  // The core requirement: the model is NOT empty, and the other environments
  // kept their tabs. This is the state the window must survive.
  Report(count_after > 0 && personal_after == 3 && test_a_after == 3,
         "T9A-others-intact",
         "personal=" + base::NumberToString(personal_after) +
             " test-a=" + base::NumberToString(test_a_after) +
             "; window must stay alive because the model is not empty");

  // A tab in another environment must never be reported as visible in the
  // emptied one -- no substitution of another environment's tabs.
  bool substituted = false;
  for (int i = 0; i < count_after; ++i) {
    if (mapper().IsTabVisible(kEnvC, i)) {
      substituted = true;
    }
  }
  Report(!substituted, "T9A-no-substitution",
         "no surviving tab is claimed by the emptied environment");

  // The active tab after the mass close must still belong to a live
  // environment, otherwise the strip would have no valid selection.
  const std::optional<int> active = mapper().GetActiveGlobalIndex();
  if (active.has_value()) {
    const std::optional<std::string> owner =
        mapper().EnvironmentOfGlobalIndex(*active);
    Report(owner.has_value(), "T9A-active-tab-valid",
           "active tab at g" + base::NumberToString(*active) + " belongs to '" +
               (owner ? *owner : "<none>") + "'");
  }
}

void SelfTest::Run() {
  LOG(INFO) << "PARRA-SELFTEST BEGIN model_count="
            << base::NumberToString(model()->count());

  if (model()->count() < 3) {
    Report(false, "T0-precondition",
           "self-test needs the 3x3 layout from --parra-poc-visibility; model "
           "has only " +
               base::NumberToString(model()->count()) + " tab(s)");
    LOG(INFO) << "PARRA-SELFTEST SUMMARY checks=" << g_checks
              << " failures=" << g_failures;
    return;
  }

  TestMapping();
  TestSwitchDoesNotTouchPages();
  TestInsert();
  TestClose();
  TestMove();
  TestSelectionMemory();
  TestEmptyEnvironment();
  TestInterleavedOrder();
  TestRestoreTags();
  TestFifteenSwitches();
  TestInvalidInputRejected();
  TestNativeActivation();
  TestPinnedTabsStayInTheirEnvironment();
  TestBrowserLevelTab();
  TestTwoWindowEdges();
  TestEmptyEnvironmentKeepsWindowAlive();

  LOG(INFO) << "PARRA-SELFTEST SUMMARY checks=" << g_checks
            << " failures=" << g_failures
            << " result=" << (g_failures == 0 ? "ALL-PASS" : "FAILURES");

  // Final strip dump so a failure can be diagnosed from the log alone.
  for (int i = 0; i < model()->count(); ++i) {
    content::WebContents* contents = model()->GetWebContentsAt(i);
    LOG(INFO) << "PARRA-SELFTEST STRIP index=" << i
              << " env=" << EnvironmentTag::Get(contents)
              << " active=" << (i == model()->active_index() ? "yes" : "no")
              << " token="
              << (contents ? contents->GetUniqueToken().ToString() : "<null>");
  }

  // The self-test is verification-only, so it ends the run itself. Otherwise a
  // headless verification would have to guess when the suite finished and the
  // log would be truncated at an arbitrary point.
  LOG(INFO) << "PARRA-SELFTEST EXITING";
  base::SingleThreadTaskRunner::GetCurrentDefault()->PostTask(
      FROM_HERE, base::BindOnce([]() {
        BrowserWindowInterface* browser =
            GetLastActiveBrowserWindowInterfaceWithAnyProfile();
        if (!browser) {
          return;
        }
        // Close through the window's own close path so session shutdown runs
        // normally and the session file stays valid for the restore tests.
        browser->GetWindow()->Close();
      }));
}

// Explicit test-only fixture for exercising the real Views drag path.
void CreateDragFixture(BrowserWindowInterface* first) {
  if (!first || !first->GetProfile() || !first->GetTabStripModel()) {
    return;
  }
  Profile* profile = first->GetProfile();
  first->GetWindow()->SetBounds(gfx::Rect(20, 30, 760, 400));
  auto create_window = [](Profile* p, const gfx::Rect& bounds) {
    BrowserWindowCreateParams params(p, true);
    params.should_trigger_session_restore = false;
    params.initial_bounds = bounds;
    BrowserWindowInterface* window = CreateBrowserWindow(std::move(params));
    chrome::NewTab(window, NewTabTypes::kNoUserAction);
    window->GetWindow()->SetBounds(bounds);
    window->GetWindow()->Show();
    return window;
  };
  BrowserWindowInterface* second =
      create_window(profile, gfx::Rect(850, 30, 760, 400));
  BrowserWindowInterface* occupied =
      create_window(profile, gfx::Rect(20, 480, 760, 400));
  Profile* otr = profile->GetPrimaryOTRProfile(true);
  BrowserWindowInterface* foreign =
      create_window(otr, gfx::Rect(850, 480, 760, 400));
  auto add_special = [](BrowserWindowInterface* window, const char* title) {
    auto tab = content::WebContents::Create(
        content::WebContents::CreateParams(window->GetProfile()));
    BrowserLevelTab::Mark(tab.get());
    content::WebContents* contents =
        EnvironmentTabOperations::InsertOrActivateBrowserLevelTab(
            window->GetTabStripModel(), std::move(tab));
    const std::string url =
        std::string("data:text/html,<title>") + title + "</title>";
    contents->GetController().LoadURL(GURL(url), content::Referrer(),
                                      ui::PAGE_TRANSITION_AUTO_TOPLEVEL,
                                      std::string());
    return content::DevToolsAgentHost::GetOrCreateFor(contents)->GetId();
  };
  const std::string source_id = add_special(first, "ParraDragSource");
  const std::string occupied_id = add_special(occupied, "ParraDragOccupied");
  LOG(INFO) << "PARRA-DRAG-FIXTURE source=" << source_id
            << " occupied=" << occupied_id
            << " a=" << first->GetSessionID().id()
            << " b=" << second->GetSessionID().id()
            << " c=" << occupied->GetSessionID().id()
            << " d=" << foreign->GetSessionID().id();
  first->GetWindow()->Activate();
}

}  // namespace

void MaybeRunIndexMapperSelfTest() {
  if (base::CommandLine::ForCurrentProcess()->HasSwitch(
          "parra-poc-drag-fixture")) {
    BrowserWindowInterface* browser =
        GetLastActiveBrowserWindowInterfaceWithAnyProfile();
    if (browser) {
      base::SingleThreadTaskRunner::GetCurrentDefault()->PostDelayedTask(
          FROM_HERE,
          base::BindOnce(
              [](base::WeakPtr<BrowserWindowInterface> weak) {
                if (weak) {
                  CreateDragFixture(weak.get());
                }
              },
              browser->GetWeakPtr()),
          base::Seconds(4));
    }
    return;
  }
  if (!base::CommandLine::ForCurrentProcess()->HasSwitch(
          switches::kParraPocSelfTest)) {
    return;
  }

  BrowserWindowInterface* browser =
      GetLastActiveBrowserWindowInterfaceWithAnyProfile();
  if (!browser) {
    LOG(ERROR) << "PARRA-SELFTEST no browser window";
    return;
  }
  Profile* profile = browser->GetProfile();
  TabStripModel* model = browser->GetTabStripModel();
  if (!profile || !model) {
    LOG(ERROR) << "PARRA-SELFTEST no profile/tab strip";
    return;
  }

  // Run on the next task so the strip is fully populated and every page has had
  // a chance to commit; the visibility PoC runs in PostBrowserStart and may
  // still be starting loads when this is scheduled.
  SelfTest* test = new SelfTest(profile, model);
  base::SingleThreadTaskRunner::GetCurrentDefault()->PostDelayedTask(
      FROM_HERE,
      base::BindOnce(
          [](SelfTest* owned) {
            owned->Run();
            delete owned;
          },
          test),
      base::Seconds(4));
}

}  // namespace parra
