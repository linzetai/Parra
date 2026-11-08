// Copyright 2012 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CHROME_BROWSER_UI_VIEWS_TABS_BROWSER_TAB_STRIP_CONTROLLER_H_
#define CHROME_BROWSER_UI_VIEWS_TABS_BROWSER_TAB_STRIP_CONTROLLER_H_

#include <memory>
#include <vector>

#include "base/callback_list.h"
#include "base/memory/raw_ptr.h"
#include "chrome/browser/parra/environment_index_mapper.h"
#include "chrome/browser/parra/window_environment_state.h"
#include "chrome/browser/ui/immersive/immersive_mode_controller.h"
#include "chrome/browser/ui/tabs/hover_tab_selector.h"
#include "chrome/browser/ui/tabs/tab_strip_model.h"
#include "chrome/browser/ui/views/tabs/shared/tab_strip_types.h"
#include "chrome/browser/ui/views/tabs/tab/tab_context_menu_controller.h"
#include "chrome/browser/ui/views/tabs/tab_strip.h"
#include "chrome/browser/ui/views/tabs/tab_strip_controller.h"
#include "components/prefs/pref_change_registrar.h"
#include "components/tab_groups/tab_group_color.h"
#include "ui/base/mojom/menu_source_type.mojom-forward.h"
#include "ui/menus/simple_menu_model.h"

class BrowserFrameView;
class BrowserView;
class BrowserWindowInterface;
class Tab;
class TabGroup;

namespace tabs {
class TabInterface;
}  // namespace tabs

namespace tab_groups {
class TabGroupId;
class TabGroupVisualData;
}  // namespace tab_groups

// An implementation of TabStripController that sources data from the
// WebContentses in a TabStripModel.
class BrowserTabStripController : public TabStripController,
                                  public TabStripModelObserver,
                                  public TabContextMenuController::Delegate {
 public:
  BrowserTabStripController(TabStripModel* model, BrowserView* browser_view);
  BrowserTabStripController(const BrowserTabStripController&) = delete;
  BrowserTabStripController& operator=(const BrowserTabStripController&) =
      delete;
  ~BrowserTabStripController() override;

  void InitFromModel(TabStrip* tabstrip);
  void Reset();

  // TabStripController implementation:
  ui::ListSelectionModel GetSelectionModel() const override;

  int GetCount() const override;
  bool IsValidIndex(int model_index) const override;
  bool IsModelIndexVisible(int model_index) const override;
  bool IsActiveTab(int model_index) const override;
  std::optional<int> GetActiveIndex() const override;
  bool IsTabSelected(int model_index) const override;
  bool IsTabPinned(int model_index) const override;
  bool IsBrowserClosing() const override;
  void SelectTab(int model_index, const ui::Event& event) override;
  void RecordMetricsOnTabSelectionChange(
      std::optional<tab_groups::TabGroupId> group) override;
  void ExtendSelectionTo(int model_index) override;
  void ToggleSelected(int model_index) override;
  void AddSelectionFromAnchorTo(int model_index) override;
  void OnCloseTab(int model_index,
                  CloseTabSource source,
                  base::OnceCallback<void(CloseTabSource)> callback) override;
  void ToggleTabAudioMute(int model_index) override;
  void AddTabToGroup(int model_index,
                     const tab_groups::TabGroupId& group) override;
  void RemoveTabFromGroup(int model_index) override;
  void MoveTab(int start_index, int final_index) override;
  void MoveGroup(const tab_groups::TabGroupId& group, int final_index) override;
  using TabStripController::ToggleTabGroupCollapsedState;
  void ToggleTabGroupCollapsedState(
      tab_groups::TabGroupId group,
      ToggleTabGroupCollapsedStateOrigin origin) override;
  void ShowContextMenuForTab(Tab* tab,
                             const gfx::Point& p,
                             ui::mojom::MenuSourceType source_type) override;
  int HasAvailableDragActions() const override;
  void OnDropIndexUpdate(std::optional<int> index, bool drop_before) override;
  void CreateNewTab(NewTabTypes context) override;
  void OnStartedDragging() override;
  void OnStoppedDragging() override;
  void TabKeyboardFocusChangedTo(const tabs::TabInterface* tab) override;
  std::u16string GetGroupTitle(
      const tab_groups::TabGroupId& group_id) const override;
  std::u16string GetGroupContentString(
      const tab_groups::TabGroupId& group_id) const override;
  tab_groups::TabGroupColorId GetGroupColorId(
      const tab_groups::TabGroupId& group_id) const override;
  TabGroup* GetTabGroup(const tab_groups::TabGroupId& group_id) const override;
  bool IsGroupCollapsed(const tab_groups::TabGroupId& group) const override;

  std::optional<tab_groups::TabGroupId> GetFocusedGroup() const override;
  void EnterFocusMode(const tab_groups::TabGroupId& group) override;
  using TabStripController::ExitFocusMode;
  void ExitFocusMode(std::optional<TabGroupFocusExitReason> reason) override;

  void SetVisualDataForGroup(
      const tab_groups::TabGroupId& group,
      const tab_groups::TabGroupVisualData& visual_data) override;
  std::optional<int> GetFirstTabInGroup(
      const tab_groups::TabGroupId& group) const override;
  gfx::Range ListTabsInGroup(
      const tab_groups::TabGroupId& group_id) const override;
  std::u16string GetAccessibleTabName(const Tab* tab) const override;
  BrowserWindowInterface* GetBrowserWindowInterface() override;

  // Test-specific methods.
  void CloseContextMenuForTesting();

 private:
  // TabStripModelObserver implementation:
  void OnTabStripModelChanged(
      TabStripModel* tab_strip_model,
      const TabStripModelChange& change,
      const TabStripSelectionChange& selection) override;
  void OnTabWillBeAdded() override;
  void OnTabWillBeRemoved(tabs::TabInterface* tab, int index) override;
  void OnTabPinnedStateChanged(tabs::TabInterface* tab, int index) override;
  void OnTabGroupChanged(const TabGroupChange& change) override;
  void TabGroupedStateChanged(TabStripModel* tab_strip_model,
                              std::optional<tab_groups::TabGroupId> old_group,
                              std::optional<tab_groups::TabGroupId> new_group,
                              tabs::TabInterface* tab,
                              int index) override;
  void OnSplitTabChanged(const SplitTabChange& change) override;
  void OnTabGroupFocusChanged(
      std::optional<tab_groups::TabGroupId> new_focused_group_id,
      std::optional<tab_groups::TabGroupId> old_focused_group_id) override;

  // Updates the browser theme when focus mode is active for a tab group.
  // Only updates themes for permanent tab groups; non-group focus mode does not
  // theme the browser. Triggered when tab group focus changes or when the color
  // of the currently focused tab group changes.
  void UpdateFocusModeTheme(std::optional<tab_groups::TabGroupId> group_id);

  // Updates freezing votes when focus mode or tab membership changes.
  void UpdateTabFocusFreezing(int model_index);
  void UpdateAllTabsFocusFreezing();

  BrowserFrameView* GetFrameView();
  const BrowserFrameView* GetFrameView() const;

  // Adds tabs to the view model.
  void AddTabs(const std::vector<TabStrip::AddTabData>& tabs_data);

  void OnDiscardRingTreatmentEnabledChanged();
  void OnGlassFrameEligibilityChanged(bool is_eligible);

  // TabContextMenuController::Delegate:
  bool IsContextMenuCommandChecked(
      TabStripModel::ContextMenuCommand command_id) override;
  bool IsContextMenuCommandEnabled(
      tabs::TabInterface* tab,
      TabStripModel::ContextMenuCommand command_id) override;
  bool IsContextMenuCommandAlerted(
      TabStripModel::ContextMenuCommand command_id) override;
  void ExecuteContextMenuCommand(tabs::TabInterface* tab,
                                 TabStripModel::ContextMenuCommand command_id,
                                 int event_flags) override;
  bool GetContextMenuAccelerator(int command_id,
                                 ui::Accelerator* accelerator) override;

  raw_ptr<TabStripModel> model_;

  raw_ptr<TabStrip> tabstrip_;

  raw_ptr<BrowserView> browser_view_;

  // Parra: per-window environment state. OWNED HERE.
  //
  // This controller is created once per browser window and destroyed with that
  // window's tab strip, which is exactly the lifetime the environment state
  // needs: long enough to outlive any tab-close callback during teardown, and
  // short enough that nothing survives its window. See
  // window_environment_state.h for why the object must NOT be self-deleting
  // from BrowserDidClose.
  std::unique_ptr<parra::WindowEnvironmentState> parra_window_state_;

  // Parra: subscription to "this window's active environment changed".
  //
  // Held as a member so it unregisters when the controller is destroyed: a
  // window closed while a switch is in flight cannot call back into a dead tab
  // strip. This is the mechanism that makes a programmatic switch re-lay out
  // the strip immediately, instead of waiting for some later selection change
  // (the bug in docs/parra/93 §2.2).
  base::CallbackListSubscription parra_environment_changed_subscription_;

  // Recomputes tab visibility after the active environment changed and
  // re-lays out the strip. Kept as one function so the "switch environment"
  // path cannot forget one of the two steps.
  void ApplyEnvironmentFilter();

  // If the model's active tab belongs to an environment other than this
  // window's active environment, adopt that environment for this window.
  //
  // This is the single coordination point for every native Chromium path that
  // can change the active tab without consulting Parra: Ctrl+Tab,
  // Ctrl+Shift+Tab, Ctrl+1..9, browser commands, bookmarks, session restore. It
  // runs from the model observer, so it covers all of them at once instead of
  // patching each command.
  //
  // Adopting (rather than refusing) is deliberate: by the time the observer
  // runs the activation has already happened, so refusing would mean undoing a
  // user action.
  void AdoptActiveTabEnvironment();

  // Called when this window's active environment changed, by ANY cause: the
  // environment switcher, session restore, or a native activation that adopted
  // a foreign tab.
  //
  // Re-applies the filter so the strip matches the content area immediately,
  // instead of waiting for an unrelated selection change.
  void OnActiveEnvironmentChanged();

  // Makes sure the window's active environment has at least one tab.
  //
  // Chromium cannot have a tab strip with zero tabs, so "switch to an
  // environment that has no tabs" would otherwise leave the previously active
  // tab -- which belongs to a DIFFERENT environment -- attached to the content
  // view, rendering that environment's page and holding its document focus.
  // Creating the tab is the only way to honour the switch without exposing
  // foreign content.
  //
  // Lives here rather than in EnvironmentVisibilityController because only this
  // layer may call chrome::NewTab(), which is the one path that carries the
  // intended environment id. See the definition for the full argument.
  void EnsureActiveEnvironmentHasTab();

  // True while EnsureActiveEnvironmentHasTab() is creating a tab, so the
  // activation notification that chrome::NewTab() causes does not recurse.
  bool parra_creating_empty_environment_tab_ = false;

  // True while AdoptActiveTabEnvironment() is changing the environment.
  // Prevents the resulting notifications from re-entering and ping-ponging
  // between tabs.
  bool parra_adopting_active_tab_ = false;

 public:
  // Re-applies the environment filter if the active environment changed since
  // the last time it was applied. Safe to call at any time and cheap when
  // nothing changed; this is what the environment switcher calls after a
  // switch.
  void SyncEnvironmentFilterIfStale();

  // Emits a PARRA-UI snapshot of which model indices the filter reports as
  // visible, plus a cross-check of what the view layer actually shows.
  //
  // The cross-check matters: a filter that answers correctly but whose answer
  // is not honoured by the layout would look perfect in logs that only print
  // the filter's own opinion.
  void LogVisibilitySnapshot(const char* reason) const;

 private:
  // The environment the tab strip was last laid out for. Used to detect that a
  // switch happened, because an environment switch produces no TabStripModel
  // event at all.
  std::string parra_last_applied_environment_ = parra::kDefaultEnvironmentId;

  // If non-NULL it means we're showing a menu for the tab.
  std::unique_ptr<TabContextMenuController> context_menu_controller_;

  // Helper for performing tab selection as a result of dragging over a tab.
  HoverTabSelector hover_tab_selector_;

  // Forces the tabs to use the regular (non-immersive) style and the
  // top-of-window views to be revealed when the user is dragging `tabstrip`'s
  // tabs.
  std::unique_ptr<ImmersiveRevealedLock> immersive_reveal_lock_;

  base::CallbackListSubscription glass_frame_service_subscription_;
};

#endif  // CHROME_BROWSER_UI_VIEWS_TABS_BROWSER_TAB_STRIP_CONTROLLER_H_
