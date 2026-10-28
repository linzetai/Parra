// Copyright 2026 The Parra Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chrome/browser/parra/environment_tab_operations.h"

#include <algorithm>
#include <optional>
#include <string>
#include <vector>

#include "base/logging.h"
#include "base/strings/string_number_conversions.h"
#include "chrome/browser/parra/environment_index_mapper.h"
#include "chrome/browser/parra/environment_tag.h"
#include "chrome/browser/parra/parra_storage_partition_util.h"
#include "chrome/browser/parra/window_environment_state.h"
#include "chrome/browser/ui/browser_window/public/browser_window_interface.h"
#include "chrome/browser/ui/tabs/tab_group_model.h"
#include "chrome/browser/ui/tabs/tab_strip_model.h"
#include "components/tabs/public/tab_group.h"
#include "components/tabs/public/tab_interface.h"
#include "content/public/browser/web_contents.h"
#include "ui/gfx/range/range.h"

namespace parra {

// static
std::string EnvironmentTabOperations::ActiveEnvironment(TabStripModel* model) {
  if (!model || !IsParraPocEnabled())
    return std::string();
  return EnvironmentIndexMapper(model).active_environment();
}

// static
EnvironmentTabContext EnvironmentTabOperations::MakeContext(
    BrowserWindowInterface* window) {
  EnvironmentTabContext context;
  context.window = window;
  if (!window)
    return context;

  context.environment_id = ActiveEnvironment(window->GetTabStripModel());
  return context;
}

// static
std::string EnvironmentTabOperations::EnvironmentForUserInitiatedNewTab(
    BrowserWindowInterface* window) {
  const EnvironmentTabContext context = MakeContext(window);
  if (!context.is_valid() || context.environment_id.empty())
    return std::string();

  TabStripModel* model = context.window->GetTabStripModel();
  if (!model)
    return std::string();

  // When the environment still has a tab, that tab IS the source the navigator
  // will use, so inheriting from it already produces the right answer and nothing
  // needs to be forced.
  //
  // Only the empty case needs an explicit id: there is no tab of this environment
  // to inherit from, and the active tab belongs to a different environment, so
  // inheritance would silently create the new tab there. The user's intent is
  // unambiguous -- they are looking at an empty strip for THIS environment -- so
  // this is exactly the state in which naming the environment is both necessary
  // and correct.
  const EnvironmentIndexMapper mapper(model);
  if (mapper.GetVisibleCount(context.environment_id) > 0) {
    // The normal case. Logged because "which environment did the new tab join"
    // is the single fact T1/T2 assert on, and without this line the only evidence
    // is the tag written later, which makes a wrong inheritance look identical to
    // a correct one until the page commits.
    LOG(INFO) << "parra-tabs: new tab inherits from the active tab (environment '"
              << context.environment_id << "' has "
              << mapper.GetVisibleCount(context.environment_id)
              << " visible tab(s))";
    return std::string();
  }

  LOG(INFO) << "parra-tabs: new tab requested while environment '"
            << context.environment_id
            << "' is empty; creating it in that environment instead of "
               "inheriting from another environment's active tab";
  return context.environment_id;
}

// static
bool EnvironmentTabOperations::SelectRelativeTab(
    BrowserWindowInterface* browser,
    bool next,
    TabStripUserGestureDetails gesture_details) {
  if (!browser || !IsParraPocEnabled())
    return false;

  TabStripModel* model = browser->GetTabStripModel();
  if (!model)
    return false;

  // Reported through the mapper, which reads the window-scoped active
  // environment. Using a process-global here would make two windows share one
  // cycle, so Ctrl+Tab in window A would jump inside window B's environment.
  const std::string environment_id = ActiveEnvironment(model);
  if (environment_id.empty())
    return false;

  const EnvironmentIndexMapper mapper(model);

  // The visible set of an environment shorter than the model means a native
  // Ctrl+Tab would walk into another environment's tab. That is the whole reason
  // this function exists.
  const std::vector<int> visible = mapper.GetVisibleGlobalIndices(environment_id);
  if (visible.size() < 2) {
    // One tab (or none): there is nowhere to go inside this environment.
    // Reported as handled so the upstream path does not cycle into a foreign
    // environment. Both "next" and "previous" are no-ops here, exactly like
    // stock Chromium with a single tab.
    LOG(INFO) << "parra-tabs: Ctrl+Tab inside environment '" << environment_id
              << "' is a no-op (visible tabs=" << visible.size() << ")";
    return true;
  }

  const std::optional<int> active_global = mapper.GetActiveGlobalIndex();
  if (!active_global.has_value()) {
    // No active tab at all: activate the environment's first tab so the user is
    // not left looking at nothing.
    model->ActivateTabAt(visible.front(), gesture_details);
    return true;
  }

  // Refuse to cycle from a tab that is not in the active environment. This can
  // only happen if the two disagreed, which is an invariant violation reported
  // elsewhere; guessing a starting point would silently move the user into
  // another environment.
  if (!mapper.IsTabVisible(environment_id, *active_global)) {
    LOG(ERROR) << "parra-tabs: Ctrl+Tab skipped: active tab g" << *active_global
               << " is not in environment '" << environment_id
               << "'. Activating that environment's first tab instead.";
    model->ActivateTabAt(visible.front(), gesture_details);
    return true;
  }

  const std::optional<int> target =
      next ? mapper.GetNextVisibleGlobalIndex(environment_id, *active_global,
                                             /*wrap=*/true)
           : mapper.GetPreviousVisibleGlobalIndex(environment_id, *active_global,
                                                  /*wrap=*/true);
  if (!target.has_value()) {
    // GetVisibleGlobalIndices() is non-empty and the active tab is in it, so this
    // cannot happen; report rather than cycle silently.
    LOG(ERROR) << "parra-tabs: no " << (next ? "next" : "previous")
               << " tab in environment '" << environment_id
               << "' despite " << visible.size() << " visible tabs";
    return true;
  }

  LOG(INFO) << "parra-tabs: Ctrl+" << (next ? "" : "Shift+") << "Tab g"
            << *active_global << " -> g" << *target << " inside environment '"
            << environment_id << "' (visible=" << visible.size()
            << ", model=" << model->count() << ")";
  model->ActivateTabAt(*target, gesture_details);
  return true;
}

// static
bool EnvironmentTabOperations::SelectTabAtVisibleIndex(
    BrowserWindowInterface* browser,
    int visible_index,
    TabStripUserGestureDetails gesture_details) {
  if (!browser || !IsParraPocEnabled())
    return false;

  TabStripModel* model = browser->GetTabStripModel();
  if (!model)
    return false;

  const std::string environment_id = ActiveEnvironment(model);
  if (environment_id.empty())
    return false;

  const EnvironmentIndexMapper mapper(model);

  // THE conversion that matters: the shortcut counts tabs the user can see, and
  // the model counts tabs across every environment. Interpreting Ctrl+3 as a
  // global model index would activate whatever tab happens to sit third in the
  // model, which in a three-environment window is usually a hidden one.
  const std::optional<int> global =
      mapper.VisibleToGlobal(environment_id, visible_index);
  if (!global.has_value()) {
    // The environment has fewer visible tabs than the key implies. Stock
    // Chromium also does nothing in that case, so returning true keeps the
    // behaviour identical while still preventing a global-index interpretation.
    LOG(INFO) << "parra-tabs: Ctrl+" << (visible_index + 1)
              << " ignored; environment '" << environment_id << "' has only "
              << mapper.GetVisibleCount(environment_id) << " visible tab(s)";
    return true;
  }

  LOG(INFO) << "parra-tabs: Ctrl+" << (visible_index + 1) << " -> visible["
            << visible_index << "] = g" << *global << " inside environment '"
            << environment_id << "'";
  model->ActivateTabAt(*global, gesture_details);
  return true;
}

// static
bool EnvironmentTabOperations::SelectLastTab(
    BrowserWindowInterface* browser,
    TabStripUserGestureDetails gesture_details) {
  if (!browser || !IsParraPocEnabled())
    return false;

  TabStripModel* model = browser->GetTabStripModel();
  if (!model)
    return false;

  const std::string environment_id = ActiveEnvironment(model);
  if (environment_id.empty())
    return false;

  const EnvironmentIndexMapper mapper(model);
  const std::vector<int> visible = mapper.GetVisibleGlobalIndices(environment_id);
  if (visible.empty()) {
    LOG(INFO) << "parra-tabs: Ctrl+9 ignored; environment '" << environment_id
              << "' has no tabs";
    return true;
  }

  LOG(INFO) << "parra-tabs: Ctrl+9 -> last visible g" << visible.back()
            << " inside environment '" << environment_id << "'";
  model->ActivateTabAt(visible.back(), gesture_details);
  return true;
}

// static
bool EnvironmentTabOperations::PrepareCloseActiveTab(
    BrowserWindowInterface* browser) {
  if (!browser || !IsParraPocEnabled())
    return false;

  TabStripModel* model = browser->GetTabStripModel();
  if (!model)
    return false;

  const std::string environment_id = ActiveEnvironment(model);
  if (environment_id.empty())
    return false;

  const int active = model->active_index();
  if (active == TabStripModel::kNoTab || !model->ContainsIndex(active)) {
    // Nothing is active, so there is nothing to constrain. Upstream will handle
    // it (and in practice does nothing).
    return false;
  }

  // The selection can contain tabs from several environments -- a multi-select
  // in the strip is a model-level selection, and Ctrl+W closes the whole
  // selection. Closing it verbatim would destroy another environment's pages
  // while the user is looking at this one.
  const tabs::TabInterface* active_tab = model->GetActiveTab();
  const auto& selected = model->selection_model().selected_tabs();
  bool selection_leaks = false;
  for (const tabs::TabInterface* tab : selected) {
    if (tab != active_tab &&
        EnvironmentTag::Get(tab->GetContents()) != environment_id) {
      selection_leaks = true;
      break;
    }
  }

  if (selection_leaks) {
    LOG(WARNING) << "parra-tabs: Ctrl+W selection spans environments; closing "
                    "only the active tab of '"
                 << environment_id
                 << "' instead of the whole multi-selection";
    // Re-activating the active tab collapses the selection to it. This is a
    // selection change only: no tab is created, closed or moved.
    model->ActivateTabAt(active);
    return true;
  }

  // Also refuse when the ACTIVE tab itself is not in the active environment.
  // That combination should be impossible (it is the invariant the filter
  // maintains), but silently closing a foreign tab would be far worse than
  // reporting it.
  if (EnvironmentTag::Get(active_tab->GetContents()) != environment_id) {
    LOG(ERROR) << "parra-tabs: Ctrl+W skipped: active tab g" << active
               << " belongs to '" << EnvironmentTag::Get(active_tab->GetContents())
               << "' but the window is on '" << environment_id << "'";
    return true;
  }

  return false;
}

// static
void EnvironmentTabOperations::FinishCloseActiveTab(
    BrowserWindowInterface* browser,
    const std::string& environment_before_close) {
  if (!browser || environment_before_close.empty() || !IsParraPocEnabled())
    return;

  TabStripModel* model = browser->GetTabStripModel();
  if (!model)
    return;

  EnvironmentIndexMapper mapper(model);

  const int remaining = mapper.GetVisibleCount(environment_before_close);
  if (remaining == 0) {
    // The environment the user was in is now empty.
    //
    // Do NOT fall back to another environment: showing another environment's
    // page would mean the content area silently switched identity, which is
    // precisely the isolation failure this architecture exists to prevent. Do
    // NOT close the window either -- the model still owns other environments'
    // tabs, and Browser::TabStripEmpty() only fires when the MODEL empties.
    //
    // The window is left as the native model left it, with the active environment
    // unchanged, so the very next Ctrl+T puts a new tab back into this same
    // environment (see EnvironmentTabRouter). That is the "empty environment"
    // state, and it is the only state in which the strip shows nothing while the
    // model is non-empty.
    LOG(INFO) << "parra-tabs: environment '" << environment_before_close
              << "' is now empty (model still holds " << model->count()
              << " tab(s) from other environments); window stays open";
    return;
  }

  const std::optional<int> active_global = mapper.GetActiveGlobalIndex();
  if (active_global.has_value() &&
      mapper.IsTabVisible(environment_before_close, *active_global)) {
    // The native close already selected a tab of this environment. Nothing to do.
    return;
  }

  // The native strategy picked a tab in another environment (it favours the
  // opener, the next index, or the previously active tab, none of which know
  // about environments). Move back inside the environment, preferring the tab
  // right at the closed position so Ctrl+W behaves like a normal strip.
  const std::optional<int> target =
      mapper.GetActiveGlobalIndexFor(environment_before_close);
  if (!target.has_value()) {
    LOG(ERROR) << "parra-tabs: environment '" << environment_before_close
               << "' reports " << remaining
               << " visible tab(s) but no restorable index";
    return;
  }

  LOG(INFO) << "parra-tabs: native close selected g"
            << (active_global ? base::NumberToString(*active_global) : "<none>")
            << " outside environment '" << environment_before_close
            << "'; re-activating g" << *target << " inside it";
  model->ActivateTabAt(*target);
}

EnvironmentTabOperations::ScopedCloseEnvironmentRestorer::
    ScopedCloseEnvironmentRestorer(BrowserWindowInterface* browser)
    : browser_(browser),
      environment_before_close_(browser ? ActiveEnvironment(
                                              browser->GetTabStripModel())
                                        : std::string()) {}

EnvironmentTabOperations::ScopedCloseEnvironmentRestorer::
    ~ScopedCloseEnvironmentRestorer() {
  FinishCloseActiveTab(browser_, environment_before_close_);
}

// static
bool EnvironmentTabOperations::ValidateVictimsBelongToEnvironment(
    TabStripModel* model,
    const std::vector<tabs::TabHandle>& victims,
    const std::string& environment_id,
    const char* what) {
  if (!model || environment_id.empty())
    return false;

  const EnvironmentIndexMapper mapper(model);

  for (tabs::TabHandle handle : victims) {
    tabs::TabInterface* tab = handle.Get();
    if (!tab) {
      LOG(ERROR) << "parra: REFUSING to close via " << what
                 << ": a victim handle no longer resolves to a live tab";
      return false;
    }

    const int index = model->GetIndexOfTab(tab);
    if (index == TabStripModel::kNoTab) {
      LOG(ERROR) << "parra: REFUSING to close via " << what
                 << ": a victim is not in this TabStripModel";
      return false;
    }

    const std::string tag = EnvironmentTag::Get(tab->GetContents());
    if (tag != environment_id) {
      LOG(ERROR) << "parra: REFUSING to close via " << what << ": tab g" << index
                 << " token=" << tab->GetContents()->GetUniqueToken().ToString()
                 << " belongs to environment '" << tag
                 << "', but this operation may only close tabs of '"
                 << environment_id << "'";
      return false;
    }

    if (!mapper.IsTabVisible(environment_id, index)) {
      LOG(ERROR) << "parra: REFUSING to close via " << what << ": tab g" << index
                 << " is tagged '" << tag
                 << "' but the visible set of '" << environment_id
                 << "' excludes it; the model changed while planning";
      return false;
    }
  }

  return true;
}

// static
EnvironmentTabOperations::CloseCommandPlan
EnvironmentTabOperations::PlanCloseCommand(
    TabStripModel* model,
    int context_index,
    TabStripModel::ContextMenuCommand command_id) {
  CloseCommandPlan plan;

  // Is this even a close command? Decided from the enum alone, so an unrecognized
  // command is a clean "not mine" instead of a silent refusal that would break
  // unrelated menu items.
  const bool is_close_command =
      command_id == TabStripModel::CommandCloseTab ||
      command_id == TabStripModel::CommandCloseOtherTabs ||
      command_id == TabStripModel::CommandCloseTabsToRight ||
      command_id == TabStripModel::CommandCloseAllTabs;

  if (!is_close_command) {
    plan.disposition = CloseCommandDisposition::kNotACloseCommand;
    return plan;
  }

  if (!model || !IsParraPocEnabled()) {
    // Parra off: behave exactly like upstream. This is not a refusal.
    plan.disposition = CloseCommandDisposition::kNotACloseCommand;
    return plan;
  }

  // From here on this IS a close command, so every remaining early-out must be
  // kRefuse -- never a fallthrough to the native path. That distinction is the
  // whole point of the tri-state.
  plan.disposition = CloseCommandDisposition::kRefuse;

  if (!model->ContainsIndex(context_index)) {
    plan.reason = "context index is out of range";
    return plan;
  }

  const std::string environment_id = ActiveEnvironment(model);
  if (environment_id.empty()) {
    plan.reason = "the window has no readable active environment";
    return plan;
  }

  const EnvironmentIndexMapper mapper(model);
  if (!mapper.IsTabVisible(environment_id, context_index)) {
    plan.reason = "the context tab is not visible in environment '" +
                  environment_id + "'";
    return plan;
  }

  std::vector<tabs::TabHandle> victims;
  if (!ResolveCloseCommandVictims(model, context_index, command_id, &victims)) {
    // The resolver already logged why. Nothing safe to close inside this
    // environment -- refuse rather than widen to the native global set.
    plan.reason = "no tab of environment '" + environment_id +
                  "' is a safe victim for this command";
    return plan;
  }

  // Final gate: re-validate the exact set that is about to be destroyed.
  if (!ValidateVictimsBelongToEnvironment(model, victims, environment_id,
                                          "tab context menu close")) {
    plan.reason = "the final victim set failed environment validation";
    return plan;
  }

  plan.disposition = CloseCommandDisposition::kCloseVictims;
  plan.victims = std::move(victims);
  return plan;
}

// static
bool EnvironmentTabOperations::ResolveCloseCommandVictims(
    TabStripModel* model,
    int context_index,
    TabStripModel::ContextMenuCommand command_id,
    std::vector<tabs::TabHandle>* victims) {
  if (!victims)
    return false;
  victims->clear();

  if (!model || !IsParraPocEnabled())
    return false;
  if (!model->ContainsIndex(context_index))
    return false;

  const std::string environment_id = ActiveEnvironment(model);
  if (environment_id.empty())
    return false;

  const EnvironmentIndexMapper mapper(model);

  // Step 1: let Chromium build its own candidate set.
  //
  // Reusing GetCloseCommandCandidates() rather than replicating the rules is the
  // whole point: pinned tabs, focused group, split membership, collapsed groups
  // and the pinned home tab stay exactly as upstream defines them. Parra only
  // filters the result, so it can never introduce a victim Chromium would not
  // have chosen.
  const std::vector<tabs::TabHandle> candidates =
      model->GetCloseCommandCandidates(context_index, command_id);
  if (candidates.empty())
    return false;

  // Step 2: keep only candidates that belong to the active environment.
  //
  // The environment check is done against EnvironmentTag, which is the single
  // definition of ownership, and then cross-checked with IsTabVisible() so a
  // disagreement between the two is treated as "not ours" rather than silently
  // closed. Dropping only ever shrinks the set, so a single inconsistent tab
  // cannot make the whole command reach further.
  int rejected_foreign = 0;
  int rejected_inconsistent = 0;
  for (tabs::TabHandle handle : candidates) {
    tabs::TabInterface* tab = handle.Get();
    if (!tab)
      continue;

    const int index = model->GetIndexOfTab(tab);
    if (index == TabStripModel::kNoTab)
      continue;

    const std::string tag = EnvironmentTag::Get(tab->GetContents());
    if (tag != environment_id) {
      ++rejected_foreign;
      continue;
    }

    // Tag says "ours" but the mapper disagrees: the tag and the visible set are
    // computed from the same source, so this means the model changed under us.
    // Refuse rather than guess.
    if (!mapper.IsTabVisible(environment_id, index)) {
      ++rejected_inconsistent;
      continue;
    }

    victims->push_back(handle);
  }

  LOG(INFO) << "parra-tabs: close command " << static_cast<int>(command_id)
            << " on environment '" << environment_id << "': "
            << candidates.size() << " native candidate(s), " << victims->size()
            << " kept, " << rejected_foreign
            << " dropped as other environments, " << rejected_inconsistent
            << " dropped as inconsistent";

  // Nothing left to close inside this environment: refusing to run the command is
  // the only outcome that cannot touch a hidden environment's page.
  return !victims->empty();
}

// static
std::string EnvironmentTabOperations::EnvironmentOfGroup(
    TabStripModel* model,
    tab_groups::TabGroupId group) {
  if (!model || !IsParraPocEnabled())
    return std::string();
  if (!model->group_model() || !model->group_model()->ContainsTabGroup(group))
    return std::string();

  const gfx::Range range = model->group_model()->GetTabGroup(group)->ListTabs();

  std::string environment;
  for (int index : range.ToIntVector()) {
    if (!model->ContainsIndex(index))
      continue;

    content::WebContents* contents = model->GetWebContentsAt(index);
    const std::string tag = EnvironmentTag::Get(contents);
    if (environment.empty()) {
      environment = tag;
    } else if (environment != tag) {
      // Inconsistent group: report "no single environment" rather than picking one.
      return std::string();
    }
  }
  return environment;
}

// static
bool EnvironmentTabOperations::GroupSpansEnvironments(
    TabStripModel* model,
    tab_groups::TabGroupId group) {
  if (!model || !model->group_model() ||
      !model->group_model()->ContainsTabGroup(group)) {
    return false;
  }

  const gfx::Range range = model->group_model()->GetTabGroup(group)->ListTabs();

  std::string first;
  for (int index : range.ToIntVector()) {
    if (!model->ContainsIndex(index))
      continue;
    const std::string tag = EnvironmentTag::Get(model->GetWebContentsAt(index));
    if (first.empty()) {
      first = tag;
    } else if (first != tag) {
      return true;
    }
  }
  return false;
}

// static
bool EnvironmentTabOperations::CanJoinGroup(TabStripModel* model,
                                            int context_index,
                                            tab_groups::TabGroupId group) {
  if (!model || !IsParraPocEnabled())
    return true;  // Parra off: upstream behaviour.
  if (!model->ContainsIndex(context_index))
    return false;
  if (!model->group_model() || !model->group_model()->ContainsTabGroup(group))
    return false;

  // The comparison uses the CONTEXT TAB's environment, not the window's active one.
  // Adding a background tab to a background group is legitimate as long as both are
  // in the same environment; forcing the active environment here would wrongly
  // block that.
  const std::string tab_environment =
      EnvironmentTag::Get(model->GetWebContentsAt(context_index));
  const std::string group_environment = EnvironmentOfGroup(model, group);

  if (group_environment.empty()) {
    // The group already spans environments (pre-existing inconsistency). Refusing to
    // add more members keeps it from growing, and is the only answer that cannot
    // make things worse.
    LOG(ERROR) << "parra: refusing to add tab g" << context_index
               << " to a group that already spans environments";
    return false;
  }

  if (tab_environment != group_environment) {
    LOG(WARNING) << "parra: refusing to put tab g" << context_index
                 << " (environment '" << tab_environment
                 << "') into a group of environment '" << group_environment
                 << "': a tab group may only contain one environment";
    return false;
  }

  return true;
}

// static
bool EnvironmentTabOperations::CloseGroupInActiveEnvironment(
    TabStripModel* model,
    tab_groups::TabGroupId group) {
  if (!model || !IsParraPocEnabled())
    return false;
  if (!model->group_model() || !model->group_model()->ContainsTabGroup(group))
    return false;

  const std::string environment_id = ActiveEnvironment(model);
  if (environment_id.empty()) {
    LOG(ERROR) << "parra: refusing to close a tab group: the window has no "
                  "readable active environment";
    return false;
  }

  const gfx::Range range = model->group_model()->GetTabGroup(group)->ListTabs();

  // Split the members into "ours" and "foreign".
  std::vector<int> ours;
  std::vector<int> foreign;
  std::vector<std::string> foreign_environments;
  for (int index : range.ToIntVector()) {
    if (!model->ContainsIndex(index))
      continue;
    const std::string tag = EnvironmentTag::Get(model->GetWebContentsAt(index));
    if (tag == environment_id) {
      ours.push_back(index);
    } else {
      foreign.push_back(index);
      foreign_environments.push_back(tag);
    }
  }

  if (ours.empty()) {
    LOG(ERROR) << "parra: refusing to close a tab group that holds no tab of "
                  "environment '"
               << environment_id << "'";
    return false;
  }

  // Re-validate the final set against the invariant before touching anything.
  // The group's own members are the plan here; the check is the same one the
  // context-menu path uses, so both routes share one definition of "safe".
  {
    std::vector<tabs::TabHandle> planned;
    planned.reserve(ours.size());
    for (int index : ours)
      planned.push_back(model->GetTabAtIndex(index)->GetHandle());
    if (!ValidateVictimsBelongToEnvironment(model, planned, environment_id,
                                            "tab group close")) {
      LOG(ERROR) << "parra: refusing to close tab group: its members failed "
                    "environment validation";
      return false;
    }
  }

  if (!foreign.empty()) {
    // A cross-environment group. Detach the foreign members FIRST, so the native
    // group close that follows cannot see them at all. This is the "defuse, then
    // use the native path" strategy: it needs no reimplementation of group closing
    // and leaves the foreign tabs alive, merely un-grouped.
    LOG(WARNING) << "parra: tab group spans environments; detaching "
                 << foreign.size() << " foreign member(s) before closing the "
                 << ours.size() << " member(s) of '" << environment_id << "'";
    model->RemoveFromGroup(foreign);
  }

  // Now every remaining member of the group is in this environment, so Chromium's
  // own group close is safe to use unchanged.
  model->CloseAllTabsInGroup(group);

  LOG(INFO) << "parra: closed tab group of environment '" << environment_id
            << "' (" << ours.size() << " tab(s)); " << foreign.size()
            << " foreign member(s) kept alive and un-grouped";
  return true;
}

// static
bool EnvironmentTabOperations::MayGroupTabs(
    TabStripModel* model,
    const std::vector<int>& indices,
    std::optional<tab_groups::TabGroupId> group) {
  if (!model || !IsParraPocEnabled())
    return true;  // Parra off: upstream behaviour.
  if (indices.empty())
    return true;

  // Environment of the tabs being grouped. They must all agree with each other,
  // otherwise the group itself would span environments.
  std::string tabs_environment;
  for (int index : indices) {
    if (!model->ContainsIndex(index))
      return false;
    const std::string tag = EnvironmentTag::Get(model->GetWebContentsAt(index));
    if (tabs_environment.empty()) {
      tabs_environment = tag;
    } else if (tabs_environment != tag) {
      LOG(WARNING) << "parra: refusing to group tabs from several environments "
                      "into one group ('"
                   << tabs_environment << "' vs '" << tag << "')";
      return false;
    }
  }

  if (!group.has_value()) {
    // New group: the tabs agreeing with each other is sufficient. The group's
    // environment is then defined by its members.
    return true;
  }

  const std::string group_environment = EnvironmentOfGroup(model, *group);
  if (group_environment.empty()) {
    LOG(ERROR) << "parra: refusing to add tabs to a group that already spans "
                  "environments";
    return false;
  }

  if (group_environment != tabs_environment) {
    LOG(WARNING) << "parra: refusing to add tabs of environment '"
                 << tabs_environment << "' to a group of environment '"
                 << group_environment
                 << "': a tab group may only contain one environment";
    return false;
  }

  return true;
}

// static
void EnvironmentTabOperations::InstallGroupMembershipPolicy(TabStripModel* model) {
  if (!model)
    return;

  if (!IsParraPocEnabled()) {
    // Explicitly clear: a model reused after Parra was disabled must not keep a
    // stale policy.
    model->SetGroupMembershipPolicy(
        TabStripModel::GroupMembershipPolicy());
    return;
  }

  // The policy captures only the model; it reads the environment per call, so a
  // window switching environments is picked up automatically.
  model->SetGroupMembershipPolicy(base::BindRepeating(
      [](TabStripModel* target, const std::vector<int>& indices,
         std::optional<tab_groups::TabGroupId> group) {
        return EnvironmentTabOperations::MayGroupTabs(target, indices, group);
      },
      model));
}

// static
void EnvironmentTabOperations::InstallCloseCommandPolicy(TabStripModel* model) {
  if (!model)
    return;

  if (!IsParraPocEnabled()) {
    // Explicitly clear: a model reused after Parra was disabled must not keep a
    // stale policy that refuses closes.
    model->SetCloseCommandPolicy(TabStripModel::CloseCommandPolicy());
    return;
  }

  // Why this is installed on the MODEL and not only in the views controller:
  // ExecuteContextMenuCommand() is called directly by the keyboard accelerators in
  // browser_commands.cc (CloseOtherTabs, CloseTabsToRight). Those call sites never
  // pass through BrowserTabStripController::ExecuteContextMenuCommand(), so a
  // policy enforced there alone would simply not exist for Ctrl-based closes -- the
  // exact containment hole this round exists to close. Installing on the model
  // covers every caller, present and future, by construction.
  model->SetCloseCommandPolicy(base::BindRepeating(
      [](TabStripModel* target, int context_index,
         TabStripModel::ContextMenuCommand command_id)
          -> TabStripModel::CloseCommandDecision {
        const CloseCommandPlan plan =
            EnvironmentTabOperations::PlanCloseCommand(target, context_index,
                                                       command_id);
        switch (plan.disposition) {
          case CloseCommandDisposition::kNotACloseCommand:
            // Not a close command: let the native path run unchanged. The empty
            // victim list is what tells the model "no policy applied here".
            return {.allowed = true, .victims = {}};

          case CloseCommandDisposition::kCloseVictims:
            return {.allowed = true, .victims = plan.victims};

          case CloseCommandDisposition::kRefuse:
            LOG(WARNING) << "parra: refusing close command "
                         << static_cast<int>(command_id)
                         << " at index " << context_index << ": " << plan.reason
                         << ". Nothing will be closed; this is deliberate, not a "
                            "fallback.";
            return {.allowed = false, .victims = {}};
        }
      },
      model));
}

// static
bool EnvironmentTabOperations::ContainCloseCommandToActiveEnvironment(
    TabStripModel* model,
    int context_index,
    TabStripModel::ContextMenuCommand command_id) {
  if (!model || !IsParraPocEnabled())
    return false;
  if (!model->ContainsIndex(context_index))
    return false;

  const std::string environment_id = ActiveEnvironment(model);
  if (environment_id.empty())
    return false;

  const EnvironmentIndexMapper mapper(model);

  // The context tab must itself be visible in the active environment: it is the
  // tab the user right-clicked. If it is not, the strip and the window state
  // already disagree (reported elsewhere) and acting from it would be worse than
  // doing nothing.
  if (!mapper.IsTabVisible(environment_id, context_index))
    return false;

  // CommandCloseTab closes the SELECTION when the context tab is selected, so a
  // selection reaching outside the environment is one thing that must be narrowed
  // before the command runs. Narrowing happens by activating the context tab,
  // which collapses the selection to that one tab -- a selection change only, no
  // tab created or destroyed.
  if (command_id == TabStripModel::CommandCloseTab) {
    const auto& selected_tabs = model->selection_model().selected_tabs();
    bool selection_leaks = false;
    for (const tabs::TabInterface* tab : selected_tabs) {
      if (EnvironmentTag::Get(tab->GetContents()) != environment_id) {
        selection_leaks = true;
        break;
      }
    }
    if (selection_leaks) {
      LOG(WARNING) << "parra-tabs: tab context menu close spans environments; "
                      "narrowing the selection to the context tab of '"
                   << environment_id << "'";
      model->ActivateTabAt(context_index);
      return true;
    }
  }

  return false;
}

}  // namespace parra
