// Copyright 2012 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chrome/browser/ui/views/tabs/browser_tab_strip_controller.h"

#include <memory>
#include <optional>
#include <utility>

#include "base/feature_list.h"
#include "base/functional/bind.h"
#include "base/functional/callback.h"
#include "base/memory/raw_ptr.h"
#include "base/metrics/histogram_functions.h"
#include "base/metrics/histogram_macros.h"
#include "base/metrics/user_metrics.h"
#include "build/build_config.h"
#include "chrome/browser/favicon/favicon_utils.h"
#include "chrome/browser/parra/environment_tab_operations.h"
#include "chrome/browser/parra/parra_storage_partition_util.h"
#include "chrome/browser/resource_coordinator/tab_lifecycle_unit_external.h"
#include "chrome/browser/resource_coordinator/tab_lifecycle_unit_source.h"
#include "chrome/browser/search/search.h"
#include "chrome/browser/tab_group_sync/tab_group_sync_service_factory.h"
#include "chrome/browser/ui/browser_command_controller.h"
#include "chrome/browser/ui/browser_commands.h"
#include "chrome/browser/ui/browser_tabstrip.h"
#include "chrome/browser/ui/browser_window/public/browser_window_features.h"
#include "chrome/browser/ui/browser_window/public/browser_window_interface.h"
#include "chrome/browser/ui/navigator/browser_navigator_params.h"
#include "chrome/browser/ui/tab_ui_helper.h"
#include "chrome/browser/ui/tabs/features.h"
#include "chrome/browser/ui/tabs/public/tab_features.h"
#include "chrome/browser/ui/tabs/saved_tab_groups/saved_tab_group_utils.h"
#include "chrome/browser/ui/tabs/split_tab_util.h"
#include "chrome/browser/ui/tabs/tab_enums.h"
#include "chrome/browser/ui/tabs/tab_group_deletion_dialog_controller.h"
#include "chrome/browser/ui/tabs/tab_group_model.h"
#include "chrome/browser/ui/tabs/tab_group_theme.h"
#include "chrome/browser/ui/tabs/tab_menu_model.h"
#include "chrome/browser/ui/tabs/tab_menu_model_delegate.h"
#include "chrome/browser/ui/tabs/tab_muted_utils.h"
#include "chrome/browser/ui/tabs/tab_strip_model.h"
#include "chrome/browser/ui/tabs/tab_strip_model_delegate.h"
#include "chrome/browser/ui/tabs/tab_strip_model_observer.h"
#include "chrome/browser/ui/tabs/tab_strip_user_gesture_details.h"
#include "chrome/browser/ui/tabs/tab_utils.h"
#include "chrome/browser/ui/ui_features.h"
#include "chrome/browser/ui/unload_controller.h"
#include "chrome/browser/ui/views/contextual_tasks/contextual_tasks_close_button_controller.h"
#include "chrome/browser/ui/views/frame/browser_frame_view.h"
#include "chrome/browser/ui/views/frame/browser_view.h"
#include "chrome/browser/ui/views/frame/browser_widget.h"
#include "chrome/browser/ui/views/frame/glass_frame_service.h"
#include "chrome/browser/ui/views/frame/tab_strip_region_view.h"
#include "chrome/browser/ui/views/tabs/groups/tab_group_accessibility.h"
#include "chrome/browser/ui/views/tabs/shared/tab_strip_types.h"
#include "chrome/browser/ui/views/tabs/tab.h"
#include "chrome/browser/ui/views/tabs/tab/tab_accessibility.h"
#include "chrome/browser/ui/views/tabs/tab/tab_context_menu_controller.h"
#include "chrome/browser/ui/views/tabs/tab_strip.h"
#include "chrome/browser/ui/web_applications/app_browser_controller.h"
#include "chrome/browser/ui/web_applications/web_app_tabbed_utils.h"
#include "components/contextual_tasks/public/features.h"
#include "components/feature_engagement/public/feature_constants.h"
#include "components/omnibox/browser/autocomplete_classifier.h"
#include "components/saved_tab_groups/public/saved_tab_group.h"
#include "components/saved_tab_groups/public/tab_group_sync_service.h"
#include "components/split_tabs/split_tab_id.h"
#include "components/tab_groups/tab_group_color.h"
#include "components/tab_groups/tab_group_id.h"
#include "components/tab_groups/tab_group_visual_data.h"
#include "components/tabs/public/split_tab_data.h"
#include "components/tabs/public/tab_context_menu_command.h"
#include "components/tabs/public/tab_group.h"
#include "components/tabs/public/tab_interface.h"
#include "components/tabs/public/tab_network_state.h"
#include "content/public/browser/navigation_entry.h"
#include "content/public/browser/navigation_handle.h"
#include "content/public/browser/peak_gpu_memory_tracker_factory.h"
#include "content/public/browser/web_contents.h"
#include "ui/base/models/list_selection_model.h"
#include "ui/base/models/menu_model.h"
#include "ui/base/mojom/menu_source_type.mojom-forward.h"
#include "ui/color/color_provider_manager.h"
#include "ui/compositor/compositor.h"
#include "ui/gfx/range/range.h"
#include "ui/views/controls/menu/menu_runner.h"
#include "ui/views/widget/widget.h"
#include "url/origin.h"

#if BUILDFLAG(IS_CHROMEOS)
#include "ash/public/cpp/window_properties.h"
#include "chromeos/ash/experiences/system_web_apps/types/system_web_app_delegate.h"
#endif  // BUILDFLAG(IS_CHROMEOS)

#if defined(USE_AURA)
#include "ui/aura/window.h"
#endif

using base::UserMetricsAction;
using content::WebContents;

namespace {

TabStripUserGestureDetails GetGestureDetail(const ui::Event& event) {
  TabStripUserGestureDetails gesture_detail(
      TabStripUserGestureDetails::GestureType::kOther, event.time_stamp());
  TabStripUserGestureDetails::GestureType type =
      TabStripUserGestureDetails::GestureType::kOther;
  if (event.type() == ui::EventType::kMousePressed) {
    type = TabStripUserGestureDetails::GestureType::kMouse;
  } else if (event.type() == ui::EventType::kGestureTapDown) {
    type = TabStripUserGestureDetails::GestureType::kTouch;
  }
  gesture_detail.type = type;
  return gesture_detail;
}

}  // namespace

////////////////////////////////////////////////////////////////////////////////
// BrowserTabStripController, public:

BrowserTabStripController::BrowserTabStripController(TabStripModel* model,
                                                     BrowserView* browser_view)
    : model_(model),
      tabstrip_(nullptr),
      browser_view_(browser_view),
      hover_tab_selector_(model) {}

BrowserTabStripController::~BrowserTabStripController() {
  // When we get here the TabStrip is being deleted. We need to explicitly
  // cancel the menu, otherwise it may try to invoke something on the tabstrip
  // from its destructor.
  if (context_menu_controller_.get()) {
    context_menu_controller_.reset();
  }

  model_->RemoveObserver(this);
}

void BrowserTabStripController::InitFromModel(TabStrip* tabstrip) {
  tabstrip_ = tabstrip;
  model_->SetTabStripUI(this);

  // Add all pinned / unpinned tabs regardless of group / split affiliation.
  std::vector<TabStrip::AddTabData> tabs_to_add;
  int i = 0;
  for (const tabs::TabInterface* tab_interface : *model_) {
    tabs_to_add.push_back({.index = i++,
                           .handle = tab_interface->GetHandle(),
                           .is_pinned = tab_interface->IsPinned()});
  }
  AddTabs(tabs_to_add);

  // Add group data.
  if (model_->SupportsTabGroups()) {
    for (const tab_groups::TabGroupId& group_id :
         model_->group_model()->ListTabGroups()) {
      tabstrip_->OnGroupCreated(group_id);

      for (const int index : model_->group_model()
                                 ->GetTabGroup(group_id)
                                 ->ListTabs()
                                 .ToIntVector()) {
        tabstrip_->AddTabToGroup(group_id, index);
      }

      tabstrip_->OnGroupContentsChanged(group_id);
    }
  }

  // Add split data.
  for (const split_tabs::SplitTabId& split_id : model_->ListSplits()) {
    split_tabs::SplitTabData* data = model_->GetSplitData(split_id);
    tabstrip_->OnSplitCreated(data->GetIndexRange().ToIntVector(), split_id);
  }

  // Set the active selected tab.
  if (!model_->empty()) {
    tabstrip_->SetSelection(GetSelectionModel());
  }

  tabstrip_->StopAnimating();

  // Parra: create this window's environment state and KEEP OWNERSHIP of it.
  //
  // This controller is constructed once per browser window and lives until that
  // window's tab strip is destroyed, which is exactly the lifetime the state
  // needs. Not created lazily at read time: a query that creates state makes the
  // answer depend on which component happened to run first, and it would let the
  // state be resurrected during teardown.
  //
  // CreateFor() returns null if a state already exists for this window, so a
  // second controller cannot silently overwrite the first one's state.
  if (parra::IsParraPocEnabled() && !parra_window_state_) {
    parra_window_state_ =
        parra::WindowEnvironmentState::CreateFor(browser_view_->browser());
    if (!parra_window_state_) {
      LOG(WARNING) << "parra-ui: this window already has environment state; "
                      "reusing it rather than creating a second one";
    }
  }

  // Parra: adopt the current environment as already applied, so the very first
  // layout already filters, instead of briefly showing every environment's tabs.
  parra_last_applied_environment_ =
      parra::EnvironmentIndexMapper(model_).active_environment();

  // Parra: install the "one environment per tab group" policy. Done here because
  // this method runs exactly once per window, next to where the window's
  // environment state is created. The tabs layer cannot hold this policy itself
  // (it would need to depend on //chrome/browser/parra, a GN cycle), so it is
  // injected from this side.
  parra::EnvironmentTabOperations::InstallGroupMembershipPolicy(model_);
  parra::EnvironmentTabOperations::InstallCloseCommandPolicy(model_);

  LogVisibilitySnapshot("init-from-model");

  if (GlassFrameService* service = GlassFrameService::GetInstance()) {
    glass_frame_service_subscription_ =
        service->RegisterGlassFrameEligibilityChangedCallback(
            browser_view_->browser(),
            base::BindRepeating(
                &BrowserTabStripController::OnGlassFrameEligibilityChanged,
                base::Unretained(this)));
    OnGlassFrameEligibilityChanged(
        service->IsBrowserWindowEligible(browser_view_->browser()));
  }

  if (base::FeatureList::IsEnabled(features::kTabGroupsFocusing)) {
    UpdateAllTabsFocusFreezing();
  }
}

void BrowserTabStripController::Reset() {
  // Stop observing.
  model_->RemoveObserver(this);
  tabstrip_ = nullptr;
  glass_frame_service_subscription_ = {};
}

// TODO(crbug.com/435178910): Change this to return a
// TabStripModelSelectionState instead of a ListSelectionModel.
ui::ListSelectionModel BrowserTabStripController::GetSelectionModel() const {
  return model_->selection_model().GetListSelectionModel();
}

int BrowserTabStripController::GetCount() const {
  return model_->count();
}

bool BrowserTabStripController::IsValidIndex(int index) const {
  return model_->ContainsIndex(index);
}

// Parra: the environment filter.
//
// This is the single place where the visible tab strip learns that a tab
// belongs to another environment. Everything else in the tab strip keeps
// operating on model indices, which is what makes the change small: the model
// still owns all nine tabs, it is only the answer to "should this one be on
// screen" that changes.
//
// Returns true whenever Parra is inactive, so upstream behaviour is untouched.
bool BrowserTabStripController::IsModelIndexVisible(int model_index) const {
  if (!parra::IsParraPocEnabled())
    return true;

  // A tab that is not in the model cannot be shown, whatever the environment.
  if (!model_->ContainsIndex(model_index))
    return false;

  // Pinned tabs get NO cross-environment privilege.
  //
  // Pinning is a "keep this around" statement about a tab, not a statement about
  // which environment the user is looking at. A pinned tab keeps its environment
  // tag, keeps its StoragePartition, and is hidden exactly like any other tab when
  // its environment is not the active one. Making pinned tabs visible everywhere
  // would leak another environment's title and favicon into the strip, which is
  // the specific leak this phase exists to prevent.
  const parra::EnvironmentIndexMapper mapper(model_);
  return mapper.IsTabVisible(mapper.active_environment(), model_index);
}

void BrowserTabStripController::SyncEnvironmentFilterIfStale() {
  if (!parra::IsParraPocEnabled())
    return;

  const std::string& active =
      parra::EnvironmentIndexMapper(model_).active_environment();
  if (active == parra_last_applied_environment_)
    return;

  // Record the new environment FIRST. A re-layout re-enters this class through
  // ShouldTabBeVisible() -> IsModelIndexVisible(), and if the recorded value
  // still looked stale that recursion would schedule another layout, forever.
  const std::string previous = parra_last_applied_environment_;
  parra_last_applied_environment_ = active;

  LOG(INFO) << "parra-ui: refreshing tab strip visibility '" << previous
            << "' -> '" << active << "' (no tab was created, moved or closed)";

  if (tabstrip_) {
    tabstrip_->InvalidateLayout();
    tabstrip_->DeprecatedLayoutImmediately();
  }

  // Emitted right after the re-layout, so `view_visible_tabs` reflects the state
  // the user is about to see rather than the state before the refresh.
  LogVisibilitySnapshot("after-environment-change");
}

void BrowserTabStripController::LogVisibilitySnapshot(const char* reason) const {
  if (!parra::IsParraPocEnabled() || !tabstrip_)
    return;

  const parra::EnvironmentIndexMapper mapper(model_);
  int visible = 0;
  int hidden = 0;
  std::string visible_list;
  std::string hidden_list;
  for (int i = 0; i < model_->count(); ++i) {
    const bool is_visible = IsModelIndexVisible(i);
    const std::string env =
        parra::EnvironmentTag::Get(model_->GetWebContentsAt(i));
    if (is_visible) {
      ++visible;
      if (!visible_list.empty())
        visible_list += ",";
      visible_list += base::NumberToString(i) + ":" + env;
    } else {
      ++hidden;
      if (!hidden_list.empty())
        hidden_list += ",";
      hidden_list += base::NumberToString(i) + ":" + env;
    }
  }

  LOG(INFO) << "parra-ui: SNAPSHOT (" << reason << ") active_env='"
            << mapper.active_environment() << "' model_count="
            << model_->count() << " visible=" << visible << " hidden=" << hidden
            << " | visible=" << visible_list << " | hidden=" << hidden_list;

  // Cross-check against what the view layer actually thinks, so a filter that
  // reports the right answer but is not honoured by the layout cannot pass
  // silently.
  int view_visible_tabs = 0;
  for (int i = 0; i < tabstrip_->GetTabCount(); ++i) {
    Tab* tab = tabstrip_->tab_at(i);
    if (tab && tab->GetVisible())
      ++view_visible_tabs;
  }
  LOG(INFO) << "parra-ui: SNAPSHOT (" << reason
            << ") view_tab_count=" << tabstrip_->GetTabCount()
            << " view_visible_tabs=" << view_visible_tabs;
}

void BrowserTabStripController::ApplyEnvironmentFilter() {
  if (!parra_window_state_)
    return;
  if (!tabstrip_)
    return;

  // Forcing the container through a full re-layout is what makes the filter
  // take effect: SetTabSlotVisibility() is what calls SetVisible() on each tab,
  // and it runs from Layout() / CompleteAnimationAndLayout(). Doing it
  // explicitly here means the strip is correct immediately after a switch
  // instead of on the next unrelated layout pass.
  tabstrip_->InvalidateLayout();
  tabstrip_->DeprecatedLayoutImmediately();
}bool BrowserTabStripController::IsActiveTab(int model_index) const {
  return GetActiveIndex() == model_index;
}

std::optional<int> BrowserTabStripController::GetActiveIndex() const {
  const int active_index = model_->active_index();
  if (IsValidIndex(active_index)) {
    return active_index;
  }
  return std::nullopt;
}

bool BrowserTabStripController::IsTabSelected(int model_index) const {
  return model_->IsTabSelected(model_index);
}

bool BrowserTabStripController::IsTabPinned(int model_index) const {
  return model_->ContainsIndex(model_index) && model_->IsTabPinned(model_index);
}

bool BrowserTabStripController::IsBrowserClosing() const {
  return model_->closing_all();
}

void BrowserTabStripController::SelectTab(int model_index,
                                          const ui::Event& event) {
  // When selecting a split tab, activate the most recently focused tab in the
  // split.
  std::optional<split_tabs::SplitTabId> split_id =
      tabstrip_->tab_at(model_index)->split();
  if (split_id.has_value()) {
    model_index = split_tabs::GetIndexOfLastActiveTab(model_, split_id.value());
  }

  std::unique_ptr<viz::PeakGpuMemoryTracker> tracker =
      content::PeakGpuMemoryTrackerFactory::Create(
          viz::PeakGpuMemoryTracker::Usage::CHANGE_TAB);

  model_->ActivateTabAt(model_index, GetGestureDetail(event));

  tabstrip_->GetWidget()
      ->GetCompositor()
      ->RequestSuccessfulPresentationTimeForNextFrame(base::BindOnce(
          [](std::unique_ptr<viz::PeakGpuMemoryTracker> tracker,
             const viz::FrameTimingDetails& frame_timing_details) {
            // This callback will be ran once the ui::Compositor presents the
            // next frame for the `tabstrip_`. The destruction of `tracker` will
            // get the peak GPU memory and record a histogram.
          },
          std::move(tracker)));
}

void BrowserTabStripController::RecordMetricsOnTabSelectionChange(
    std::optional<tab_groups::TabGroupId> group) {
  base::UmaHistogramEnumeration("TabStrip.Tab.Views.ActivationAction",
                                TabActivationTypes::kTab);

  if (!group) {
    return;
  }

  base::RecordAction(base::UserMetricsAction("TabGroups_SwitchGroupedTab"));

  if (!tab_groups::SavedTabGroupUtils::SupportsSharedTabGroups()) {
    return;
  }

  tab_groups::TabGroupSyncService* tab_group_service =
      tab_groups::TabGroupSyncServiceFactory::GetForProfile(
          GetBrowserWindowInterface()->GetProfile());

  if (!tab_group_service) {
    return;
  }

  std::optional<tab_groups::SavedTabGroup> saved_group =
      tab_group_service->GetGroup(group.value());
  if (saved_group && saved_group->collaboration_id()) {
    base::RecordAction(
        base::UserMetricsAction("TabGroups.Shared.SwitchGroupedTab"));
  }
}

void BrowserTabStripController::ExtendSelectionTo(int model_index) {
  model_->ExtendSelectionTo(model_index);
}

void BrowserTabStripController::ToggleSelected(int model_index) {
  if (model_->IsTabSelected(model_index)) {
    model_->DeselectTabAt(model_index);
  } else {
    model_->SelectTabAt(model_index);
  }
}

void BrowserTabStripController::AddSelectionFromAnchorTo(int model_index) {
  model_->AddSelectionFromAnchorTo(model_index);
}

void BrowserTabStripController::OnCloseTab(
    int model_index,
    CloseTabSource source,
    base::OnceCallback<void(CloseTabSource)> callback) {
  tabs::TabInterface* tab = model_->GetTabAtIndex(model_index);
  if (!tab) {
    return;
  }
  model_->delegate()->CloseTab(tab, source, std::move(callback));
}

void BrowserTabStripController::ToggleTabAudioMute(int model_index) {
  content::WebContents* const contents = model_->GetWebContentsAt(model_index);
  bool mute_tab = !contents->IsAudioMuted();
  UMA_HISTOGRAM_BOOLEAN("Media.Audio.TabAudioMuted", mute_tab);
  SetTabAudioMuted(contents, mute_tab, TabMutedReason::kAudioIndicator,
                   std::string());
}

void BrowserTabStripController::AddTabToGroup(
    int model_index,
    const tab_groups::TabGroupId& group) {
  model_->AddToExistingGroup({model_index}, group);
}

void BrowserTabStripController::RemoveTabFromGroup(int model_index) {
  model_->RemoveFromGroup({model_index});
}

void BrowserTabStripController::MoveTab(int start_index, int final_index) {
  model_->MoveWebContentsAt(start_index, final_index, false);
}

void BrowserTabStripController::MoveGroup(const tab_groups::TabGroupId& group,
                                          int final_index) {
  model_->MoveGroupTo(group, final_index);
}

void BrowserTabStripController::ToggleTabGroupCollapsedState(
    const tab_groups::TabGroupId group,
    ToggleTabGroupCollapsedStateOrigin origin) {
  const bool is_currently_collapsed = IsGroupCollapsed(group);
  bool should_toggle_group = true;

  if (!is_currently_collapsed && GetActiveIndex().has_value()) {
    const int active_index = GetActiveIndex().value();
    if (model_->GetTabGroupForTab(active_index) == group) {
      // If the active tab is in the group that is toggling to collapse, the
      // active tab should switch to the next available tab. If there are no
      // available tabs for the active tab to switch to, a new tab will
      // be created.
      const std::optional<int> next_active =
          model_->GetNextExpandedActiveTab(group);
      if (next_active.has_value()) {
        model_->ActivateTabAt(
            next_active.value(),
            TabStripUserGestureDetails(
                TabStripUserGestureDetails::GestureType::kOther));
      } else {
        // Create a new tab that will automatically be activated
        should_toggle_group = false;
        // In the "collapse group" scenario, we want the new tab to be created
        // outside of any group to avoid it being collapsed immediately.
        model_->delegate()->AddTabAt(GURL(), -1, true);
      }
    } else {
      // If the active tab is not in the group that is toggling to collapse,
      // reactive the active tab to deselect any other potentially selected
      // tabs.
      model_->ActivateTabAt(
          active_index, TabStripUserGestureDetails(
                            TabStripUserGestureDetails::GestureType::kOther));
    }
  }

  if (origin != ToggleTabGroupCollapsedStateOrigin::kMenuAction ||
      should_toggle_group) {
    tabstrip_->ToggleTabGroup(group, !is_currently_collapsed, origin);
    model_->ChangeTabGroupVisuals(
        group,
        tab_groups::TabGroupVisualData(GetGroupTitle(group),
                                       GetGroupColorId(group),
                                       !is_currently_collapsed),
        GetTabGroup(group)->IsCustomized());
  }

  const bool is_implicit_action =
      origin == ToggleTabGroupCollapsedStateOrigin::kMenuAction ||
      origin == ToggleTabGroupCollapsedStateOrigin::kTabsSelected;
  if (!is_implicit_action) {
    if (is_currently_collapsed) {
      base::RecordAction(
          base::UserMetricsAction("TabGroups_TabGroupHeader_Expanded"));
    } else {
      base::RecordAction(
          base::UserMetricsAction("TabGroups_TabGroupHeader_Collapsed"));
    }
  }
}

void BrowserTabStripController::ShowContextMenuForTab(
    Tab* tab,
    const gfx::Point& p,
    ui::mojom::MenuSourceType source_type) {
  std::optional<int> tab_index = tabstrip_->GetModelIndexOf(tab);
  if (!tab_index.has_value()) {
    return;
  }

  context_menu_controller_ = std::make_unique<TabContextMenuController>(
      model_->GetTabAtIndex(tab_index.value())->GetHandle(), this);

  auto model = std::make_unique<TabMenuModel>(
      context_menu_controller_.get(),
      TabMenuModelDelegate::From(GetBrowserWindowInterface()), model_,
      tab_index.value());

  TabMenuModel* model_ptr = model.get();
  context_menu_controller_->LoadModel(std::move(model), model_ptr);

  context_menu_controller_->RunMenuAt(p, source_type, tabstrip_->GetWidget());
  base::UmaHistogramEnumeration("TabStrip.Tab.Views.ActivationAction",
                                TabActivationTypes::kContextMenu);
}

void BrowserTabStripController::CloseContextMenuForTesting() {
  if (context_menu_controller_) {
    context_menu_controller_->CloseMenu();
  }
}

int BrowserTabStripController::HasAvailableDragActions() const {
  return model_->delegate()->GetDragActions();
}

void BrowserTabStripController::OnDropIndexUpdate(
    const std::optional<int> index,
    const bool drop_before) {
  // Perform a delayed tab transition if hovering directly over a tab.
  // Otherwise, cancel the pending one.
  if (index.has_value() && !drop_before) {
    hover_tab_selector_.StartTabTransition(index.value());
  } else {
    hover_tab_selector_.CancelTabTransition();
  }
}

void BrowserTabStripController::CreateNewTab(NewTabTypes context) {
  chrome::NewTab(browser_view_->browser(), context);
}

void BrowserTabStripController::OnStartedDragging() {
  if (!immersive_reveal_lock_.get()) {
    // The top-of-window views should be revealed while the user is dragging
    // tabs in immersive fullscreen. The top-of-window views may not be already
    // revealed if the user is attempting to attach a tab to a tabstrip
    // belonging to an immersive fullscreen window.
    immersive_reveal_lock_ =
        ImmersiveModeController::From(GetBrowserWindowInterface())
            ->GetRevealedLock(ImmersiveModeController::ANIMATE_REVEAL_NO);
  }
}

void BrowserTabStripController::OnStoppedDragging() {
  immersive_reveal_lock_.reset();
}

void BrowserTabStripController::TabKeyboardFocusChangedTo(
    const tabs::TabInterface* tab) {
  std::optional<int> index;
  if (tab) {
    index = model_->GetIndexOfTab(tab);
  }
  chrome::BrowserCommandController::From(browser_view_->browser())
      ->TabKeyboardFocusChangedTo(index);
}

std::u16string BrowserTabStripController::GetGroupTitle(
    const tab_groups::TabGroupId& group) const {
  return model_->group_model()->GetTabGroup(group)->visual_data()->title();
}

std::u16string BrowserTabStripController::GetGroupContentString(
    const tab_groups::TabGroupId& group) const {
  return tab_groups::GetGroupContentString(
      model_->group_model()->GetTabGroup(group));
}

tab_groups::TabGroupColorId BrowserTabStripController::GetGroupColorId(
    const tab_groups::TabGroupId& group) const {
  return model_->group_model()->GetTabGroup(group)->visual_data()->color();
}

TabGroup* BrowserTabStripController::GetTabGroup(
    const tab_groups::TabGroupId& group_id) const {
  return model_->group_model()->GetTabGroup(group_id);
}

bool BrowserTabStripController::IsGroupCollapsed(
    const tab_groups::TabGroupId& group) const {
  return model_->group_model()->ContainsTabGroup(group) &&
         model_->group_model()
             ->GetTabGroup(group)
             ->visual_data()
             ->is_collapsed();
}

std::optional<tab_groups::TabGroupId>
BrowserTabStripController::GetFocusedGroup() const {
  return model_->GetFocusedGroup();
}

void BrowserTabStripController::EnterFocusMode(
    const tab_groups::TabGroupId& group) {
  model_->EnterFocusMode(group);
}

void BrowserTabStripController::ExitFocusMode(
    std::optional<TabGroupFocusExitReason> reason) {
  model_->ExitFocusMode(reason);
}

void BrowserTabStripController::SetVisualDataForGroup(
    const tab_groups::TabGroupId& group,
    const tab_groups::TabGroupVisualData& visual_data) {
  model_->ChangeTabGroupVisuals(group, visual_data);
}

std::optional<int> BrowserTabStripController::GetFirstTabInGroup(
    const tab_groups::TabGroupId& group) const {
  tabs::TabInterface* tab =
      model_->group_model()->GetTabGroup(group)->GetFirstTab();
  if (!tab) {
    return std::nullopt;
  }
  return model_->GetIndexOfTab(tab);
}

gfx::Range BrowserTabStripController::ListTabsInGroup(
    const tab_groups::TabGroupId& group) const {
  return model_->group_model()->GetTabGroup(group)->ListTabs();
}

std::u16string BrowserTabStripController::GetAccessibleTabName(
    const Tab* tab) const {
  tabs::TabInterface* interface = tab->tab_handle().Get();
  if (!interface) {
    return std::u16string();
  }

  return tabs::GetAccessibleTabLabel(interface,
                                     /*is_for_tab=*/true);
}

BrowserWindowInterface* BrowserTabStripController::GetBrowserWindowInterface() {
  return browser_view_->browser();
}

////////////////////////////////////////////////////////////////////////////////
// BrowserTabStripController, TabStripModelObserver implementation:

void BrowserTabStripController::OnTabStripModelChanged(
    TabStripModel* tab_strip_model,
    const TabStripModelChange& change,
    const TabStripSelectionChange& selection) {
  switch (change.type()) {
    case TabStripModelChange::kInserted: {
      std::vector<TabStrip::AddTabData> tabs_to_add;
      for (const auto& contents : change.GetInsert()->contents) {
        CHECK(model_->ContainsIndex(contents.index), base::NotFatalUntil::M162);
        tabs::TabInterface* const tab_interface = contents.tab;
        tabs_to_add.push_back({.index = contents.index,
                               .handle = tab_interface->GetHandle(),
                               .is_pinned = tab_interface->IsPinned()});
      }
      AddTabs(tabs_to_add);
      for (const auto& contents : change.GetInsert()->contents) {
        UpdateTabFocusFreezing(contents.index);
      }
      break;
    }
    case TabStripModelChange::kRemoved: {
      for (const auto& contents : change.GetRemove()->contents) {
        hover_tab_selector_.CancelTabTransition();
        tabstrip_->RemoveTabAt(contents.contents, contents.index,
                               contents.contents == selection.old_contents);
        if (contents.remove_reason ==
                TabRemovedReason::kInsertedIntoSidePanel ||
            contents.remove_reason ==
                TabRemovedReason::kDeletedAndExpandSidePanel) {
          tabstrip_->StopAnimating();
        }
      }
      break;
    }
    case TabStripModelChange::kMoved: {
      auto* move = change.GetMove();
      // Cancel any pending tab transition.
      hover_tab_selector_.CancelTabTransition();
      tabstrip_->MoveTab(move->from_index, move->to_index);
      break;
    }
    case TabStripModelChange::kReplaced:
    case TabStripModelChange::kSelectionOnly:
      break;
  }

  if (tab_strip_model->empty()) {
    return;
  }

  if (!base::FeatureList::IsEnabled(
          tabs::kSessionRestoreShowThrobberOnVisible) &&
      selection.active_tab_changed()) {
    // It's possible for `new_contents` to be null when the final tab in a tab
    // strip is closed.
    content::WebContents* const new_contents = selection.new_contents;
    tabs::TabInterface* const new_tab_interface = selection.new_tab;
    std::optional<size_t> index = selection.new_model.active();
    if (new_contents && new_tab_interface && index.has_value()) {
      TabUIHelper::From(new_tab_interface)->SetWasActiveAtLeastOnce();
    }
  }

  if (selection.selection_changed()) {
    tabstrip_->SetSelection(selection.new_model);
  }

  // Parra: an environment switch activates a tab in the new environment, so this
  // is the point where the visible set must change too. Selection is the only
  // model-visible consequence of a switch (the model itself is untouched), which
  // is why the sync hangs off this event rather than off a tab event.
  //
  // ADOPT NATIVE ACTIVATION FIRST, THEN SYNC.
  //
  // Chromium can activate a tab without going through the Parra environment
  // switcher: Ctrl+Tab, Ctrl+Shift+Tab, Ctrl+1..9, a browser command, a bookmark,
  // session restore, a window move. In every one of those cases the user is now
  // looking at a page in some environment, and the tab strip must agree with the
  // content area. Failing to adopt leaves the two permanently disagreeing: content
  // shows an environment whose tabs the strip has hidden, so the strip shows no
  // selection at all.
  //
  // Adopting is preferred over refusing because the activation has already
  // happened by the time this observer runs; refusing would mean undoing a user
  // action. The ordering matters: adopt first, then let the filter repaint for the
  // newly adopted environment, so there is never an intermediate frame where the
  // strip filters for the old environment while the content shows the new one.
  if (parra::IsParraPocEnabled()) {
    AdoptActiveTabEnvironment();
  }
  SyncEnvironmentFilterIfStale();
}

void BrowserTabStripController::AdoptActiveTabEnvironment() {
  if (!parra::IsParraPocEnabled() || !model_)
    return;

  // Re-entrancy guard. Changing the environment below can lead back here through
  // a re-layout (ShouldTabBeVisible -> IsModelIndexVisible) and, in principle,
  // through a nested model notification. Without the guard two observers could
  // ping-pong the environment between two tabs.
  if (parra_adopting_active_tab_)
    return;

  const int active_index = model_->active_index();
  if (active_index == TabStripModel::kNoTab ||
      !model_->ContainsIndex(active_index)) {
    return;
  }

  // Resolve the window's state once; if this window has no Parra state (unit
  // tests, teardown) there is nothing to adopt into.
  parra::WindowEnvironmentState* state =
      parra::WindowEnvironmentState::GetForModel(model_);
  if (!state)
    return;

  const std::string& owner =
      parra::EnvironmentTag::Get(model_->GetWebContentsAt(active_index));

  if (owner == state->active_environment())
    return;

  base::AutoReset<bool> guard(&parra_adopting_active_tab_, true);

  // Distinguish the two reasons the active tab can end up in another
  // environment, because they have different consequences:
  //
  //  a) the user closed the last tab of the current environment. Chromium MUST
  //     keep some tab active (BrowserView::OnActiveTabChanged CHECKs for a
  //     non-null new_contents), so "this environment is now empty" cannot
  //     literally mean "show nothing". The window therefore follows the tab
  //     Chromium had to activate, and the environment it follows to is a real,
  //     consistent environment -- not a foreign page smuggled into the old one.
  //     Note the model is still non-empty, so Browser::TabStripEmpty() does not
  //     fire and the window stays open.
  //
  //  b) something else activated a foreign tab (a native command, a bookmark,
  //     session restore). Same conclusion: adopt, so the strip and the content
  //     area can never disagree.
  //
  // Either way the old environment is remembered, and Ctrl+T while it is
  // re-selected puts its new tab back there (see
  // EnvironmentTabOperations::EnvironmentForUserInitiatedNewTab).
  const bool previous_environment_emptied =
      parra::EnvironmentIndexMapper(model_).GetVisibleCount(
          state->active_environment()) == 0;

  LOG(INFO) << "parra-ui: native activation moved the active tab to environment '"
            << owner << "' (was '" << state->active_environment() << "'"
            << (previous_environment_emptied
                    ? "; the previous environment now has no tabs"
                    : "")
            << "); adopting it for this window instead of leaving the strip and "
               "the content area disagreeing";

  state->SetActiveEnvironment(owner);
}

void BrowserTabStripController::OnTabWillBeAdded() {
  tabstrip_->EndDrag(EndDragReason::kModelAddedTab);
}

void BrowserTabStripController::OnTabWillBeRemoved(tabs::TabInterface* tab,
                                                   int index) {
  tabstrip_->OnTabWillBeRemoved(tab->GetContents(), index);
}

void BrowserTabStripController::OnTabGroupChanged(
    const TabGroupChange& change) {
  switch (change.type) {
    case TabGroupChange::kCreated: {
      tabstrip_->OnGroupCreated(change.group);
      // Add tabs to the correct group if the group if re-inserted from a
      // different tabstrip as it is not an empty tab group.
      if (change.GetCreateChange()->reason() ==
          TabGroupChange::TabGroupCreationReason::
              kInsertedFromAnotherTabstrip) {
        const gfx::Range tabs_in_group =
            change.model->group_model()->GetTabGroup(change.group)->ListTabs();

        for (int i = static_cast<int>(tabs_in_group.start());
             i < static_cast<int>(tabs_in_group.end()); i++) {
          tabstrip_->AddTabToGroup(change.group, i);
        }
        tabstrip_->OnGroupContentsChanged(change.group);
      }
      break;
    }
    case TabGroupChange::kEditorOpened: {
      tabstrip_->OnGroupEditorOpened(change.group);
      break;
    }
    case TabGroupChange::kVisualsChanged: {
      const TabGroupChange::VisualsChange* visuals_delta =
          change.GetVisualsChange();
      const tab_groups::TabGroupVisualData* old_visuals =
          visuals_delta->old_visuals;
      const tab_groups::TabGroupVisualData* new_visuals =
          visuals_delta->new_visuals;
      if (old_visuals &&
          old_visuals->is_collapsed() != new_visuals->is_collapsed()) {
        gfx::Range tabs_in_group = ListTabsInGroup(change.group);
        for (auto i = tabs_in_group.start(); i < tabs_in_group.end(); ++i) {
          tabstrip_->tab_at(i)->SetVisible(!new_visuals->is_collapsed());
          if (new_visuals->is_collapsed()) {
            tabstrip_->tab_at(i)->CreateFreezingVote(
                FreezingVoteReason::kCollapsedGroup,
                model_->GetWebContentsAt(i));
          } else {
            tabstrip_->tab_at(i)->ReleaseFreezingVote(
                FreezingVoteReason::kCollapsedGroup);
          }
        }
      }

      tabstrip_->OnGroupVisualsChanged(change.group, old_visuals, new_visuals);

      // If the group whose visual data (e.g., color) changed is currently
      // focused, update the focus mode theme color.
      if (model_->GetFocusedGroup() == change.group) {
        UpdateFocusModeTheme(change.group);
      }
      break;
    }
    case TabGroupChange::kMoved: {
      tabstrip_->OnGroupMoved(change.group);
      break;
    }
    case TabGroupChange::kClosed: {
      tabstrip_->OnGroupClosed(change.group);
      break;
    }
  }
}

void BrowserTabStripController::OnTabPinnedStateChanged(tabs::TabInterface* tab,
                                                        int model_index) {
  tabstrip_->OnTabPinnedStateChanged(model_index, tab->IsPinned());
  UpdateTabFocusFreezing(model_index);
}

void BrowserTabStripController::TabGroupedStateChanged(
    TabStripModel* tab_strip_model,
    std::optional<tab_groups::TabGroupId> old_group,
    std::optional<tab_groups::TabGroupId> new_group,
    tabs::TabInterface* tab,
    int index) {
  tabstrip_->AddTabToGroup(new_group, index);

  if (old_group.has_value()) {
    tabstrip_->OnGroupContentsChanged(old_group.value());
  }

  if (new_group.has_value()) {
    tabstrip_->OnGroupContentsChanged(new_group.value());
  }

  UpdateTabFocusFreezing(index);
}

void BrowserTabStripController::OnSplitTabChanged(
    const SplitTabChange& change) {
  if (change.type == SplitTabChange::Type::kAdded) {
    std::vector<int> split_indices;
    std::transform(
        change.GetAddedChange()->tabs().begin(),
        change.GetAddedChange()->tabs().end(),
        std::back_inserter(split_indices),
        [](const std::pair<tabs::TabInterface*, int>& p) { return p.second; });

    tabstrip_->OnSplitCreated(split_indices, change.split_id);

    // Stop animating if we are updating an active split.
    if (change.GetAddedChange()->reason() ==
        SplitTabChange::SplitTabAddReason::kSplitTabUpdated) {
      tabstrip_->StopAnimating();
    }
  } else if (change.type == SplitTabChange::Type::kRemoved) {
    std::vector<int> split_indices;
    std::transform(
        change.GetRemovedChange()->tabs().begin(),
        change.GetRemovedChange()->tabs().end(),
        std::back_inserter(split_indices),
        [](const std::pair<tabs::TabInterface*, int>& p) { return p.second; });

    tabstrip_->OnSplitRemoved(split_indices);

    // Stop animating if we are updating an active split.
    if (change.GetRemovedChange()->reason() ==
        SplitTabChange::SplitTabRemoveReason::kSplitTabUpdated) {
      tabstrip_->StopAnimating();
    }
  } else if (change.type == SplitTabChange::Type::kContentsChanged) {
    std::vector<int> split_indices;
    std::transform(
        change.GetContentsChange()->new_tabs().begin(),
        change.GetContentsChange()->new_tabs().end(),
        std::back_inserter(split_indices),
        [](const std::pair<tabs::TabInterface*, int>& p) { return p.second; });
    tabstrip_->OnSplitContentsChanged(split_indices);
    tabstrip_->StopAnimating();
  }
}

void BrowserTabStripController::OnTabGroupFocusChanged(
    std::optional<tab_groups::TabGroupId> new_group_id,
    std::optional<tab_groups::TabGroupId> old_group_id) {
  CHECK(browser_view_);

  if (auto* tab_strip_view = browser_view_->tab_strip_view()) {
    tab_strip_view->OnTabGroupFocusChanged(new_group_id, old_group_id);
  }
  if (auto* browser_widget = browser_view_->browser_widget()) {
    UpdateFocusModeTheme(new_group_id);
    browser_widget->ThemeChanged();
    if (auto* frame_view = browser_widget->GetFrameView()) {
      frame_view->SchedulePaint();
    }
  }

  UpdateAllTabsFocusFreezing();
}

void BrowserTabStripController::UpdateFocusModeTheme(
    std::optional<tab_groups::TabGroupId> group_id) {
  std::optional<SkColor> color;
  if (group_id.has_value() && model_ && model_->group_model() &&
      model_->group_model()->ContainsTabGroup(group_id.value())) {
    if (model_->IsEphemeralTabGroup(group_id.value())) {
      return;
    }
    const TabGroup* group =
        model_->group_model()->GetTabGroup(group_id.value());
    if (group && group->visual_data()) {
      const auto* color_provider =
          tabstrip_ ? tabstrip_->GetColorProvider() : nullptr;
      if (color_provider) {
        color = color_provider->GetColor(
            GetTabGroupDialogColorId(group->visual_data()->color()));
      }
    }
  }

  if (browser_view_ && browser_view_->browser_widget()) {
    browser_view_->browser_widget()->SetUserColorOverride(color);
  }
}

void BrowserTabStripController::UpdateTabFocusFreezing(int model_index) {
  if (!features::IsTabGroupsFocusFreezingEnabled()) {
    return;
  }
  if (!tabstrip_ || !model_->ContainsIndex(model_index)) {
    return;
  }
  Tab* tab = tabstrip_->tab_at(model_index);
  const std::optional<tab_groups::TabGroupId> focused_group =
      model_->GetFocusedGroup();
  if (focused_group.has_value() && !tab->data().pinned &&
      tab->group() != focused_group.value()) {
    tab->CreateFreezingVote(FreezingVoteReason::kFocusedGroup,
                            model_->GetWebContentsAt(model_index));
  } else {
    tab->ReleaseFreezingVote(FreezingVoteReason::kFocusedGroup);
  }
}

void BrowserTabStripController::UpdateAllTabsFocusFreezing() {
  if (!features::IsTabGroupsFocusFreezingEnabled() || !tabstrip_) {
    return;
  }
  for (int i = 0; i < tabstrip_->GetTabCount(); ++i) {
    UpdateTabFocusFreezing(i);
  }
}

BrowserFrameView* BrowserTabStripController::GetFrameView() {
  return browser_view_->browser_widget()->GetFrameView();
}

const BrowserFrameView* BrowserTabStripController::GetFrameView() const {
  return browser_view_->browser_widget()->GetFrameView();
}

void BrowserTabStripController::AddTabs(
    const std::vector<TabStrip::AddTabData>& tabs_data) {
  // Cancel any pending tab transition.
  hover_tab_selector_.CancelTabTransition();

  tabstrip_->AddTabsAt(tabs_data);
}

bool BrowserTabStripController::IsContextMenuCommandChecked(
    TabStripModel::ContextMenuCommand command_id) {
  return false;
}

bool BrowserTabStripController::IsContextMenuCommandEnabled(
    tabs::TabInterface* tab,
    TabStripModel::ContextMenuCommand command_id) {
  return model_->IsContextMenuCommandEnabled(model_->GetIndexOfTab(tab),
                                             command_id);
}

bool BrowserTabStripController::IsContextMenuCommandAlerted(
    TabStripModel::ContextMenuCommand command_id) {
  return false;
}

void BrowserTabStripController::ExecuteContextMenuCommand(
    tabs::TabInterface* tab,
    TabStripModel::ContextMenuCommand command_id,
    int event_flags) {
  const int context_index = model_->GetIndexOfTab(tab);

  // Parra: every close command the tab context menu can issue is decided here, and
  // the decision is three-valued on purpose.
  //
  // Why three values and not a bool: the previous round treated "could not produce
  // a safe victim set" the same as "this is not a close command", so the caller
  // fell through to Chromium's native path -- which is the GLOBAL, unfiltered set.
  // That is precisely the "silently widen the operation" failure the containment
  // exists to stop. An empty victim set must ABORT.
  //
  // What needs guarding, and what does not:
  //
  //   * The tab's own close BUTTON goes through
  //     TabStripModelDelegate::CloseTab(), which closes exactly ONE WebContents.
  //     No cross-environment reach, not routed through here.
  //   * CommandCloseTab closes the whole multi-selection; CommandCloseOtherTabs and
  //     CommandCloseTabsToRight walk the GLOBAL model order; CommandCloseAllTabs
  //     (web-app windows) also walks it. All four can reach tabs of other
  //     environments, which are hidden from the strip.
  //
  // The close itself still runs through ExecuteCloseCommandWithVictims(), which
  // keeps Chromium's own group bookkeeping, unload handling and historical-tab
  // behaviour.
  const parra::EnvironmentTabOperations::CloseCommandPlan plan =
      parra::EnvironmentTabOperations::PlanCloseCommand(model_, context_index,
                                                        command_id);

  switch (plan.disposition) {
    case parra::EnvironmentTabOperations::CloseCommandDisposition::kNotACloseCommand:
      break;  // fall through to the native path below

    case parra::EnvironmentTabOperations::CloseCommandDisposition::kCloseVictims:
      if (model_->ExecuteCloseCommandWithVictims(context_index, command_id,
                                                 plan.victims)) {
        return;
      }
      // The command was a close command but the model refused to execute it with an
      // explicit victim list. Do NOT fall back to the native path: that would use a
      // victim set Parra never approved.
      LOG(ERROR) << "parra: close command " << static_cast<int>(command_id)
                 << " could not be executed with an explicit victim list; "
                    "aborting instead of falling back to the native global close";
      return;

    case parra::EnvironmentTabOperations::CloseCommandDisposition::kRefuse:
      // Refused. Narrow a leaking multi-selection so the state is left consistent,
      // then do nothing. No close, no native fallback.
      LOG(WARNING) << "parra: refusing close command "
                   << static_cast<int>(command_id) << " on tab g" << context_index
                   << ": " << plan.reason
                   << ". Nothing will be closed; this is deliberate, not a "
                      "fallback.";
      parra::EnvironmentTabOperations::ContainCloseCommandToActiveEnvironment(
          model_, context_index, command_id);
      return;
  }

  model_->ExecuteContextMenuCommand(context_index, command_id);
}

bool BrowserTabStripController::GetContextMenuAccelerator(
    int command_id,
    ui::Accelerator* accelerator) {
#if BUILDFLAG(IS_CHROMEOS)
  auto* const app_controller =
      web_app::AppBrowserController::From(GetBrowserWindowInterface());
  auto* system_app = app_controller ? app_controller->system_app() : nullptr;
  if (system_app && !system_app->ShouldShowTabContextMenuShortcut(
                        GetBrowserWindowInterface()->GetProfile(),
                        static_cast<tabs::TabContextMenuCommand>(command_id))) {
    return false;
  }
#endif  // BUILDFLAG(IS_CHROMEOS)

  int browser_cmd;
  return TabStripModel::ContextMenuCommandToBrowserCommand(command_id,
                                                           &browser_cmd) &&
         tabstrip_->GetWidget()->GetAccelerator(browser_cmd, accelerator);
}

void BrowserTabStripController::OnGlassFrameEligibilityChanged(
    bool is_eligible) {
  browser_view_->tab_strip_view()->OnGlassFrameEligibilityChanged(is_eligible);
}
