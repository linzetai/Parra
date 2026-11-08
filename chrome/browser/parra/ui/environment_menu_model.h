// Copyright 2026 The Parra Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CHROME_BROWSER_PARRA_UI_ENVIRONMENT_MENU_MODEL_H_
#define CHROME_BROWSER_PARRA_UI_ENVIRONMENT_MENU_MODEL_H_

#include <memory>
#include <string>
#include <vector>

#include "base/functional/callback.h"
#include "base/memory/raw_ptr.h"
#include "chrome/browser/parra/environment.h"
#include "ui/base/models/menu_model.h"
#include "ui/menus/simple_menu_model.h"

class BrowserWindowInterface;
class TabStripModel;

namespace parra {

class EnvironmentVisibilityController;

// The environment switcher menu.
//
// WHAT THIS IS RESPONSIBLE FOR, AND WHAT IT IS NOT
//
// It answers three questions and nothing else:
//   * which environments exist for this profile (Personal first, then store
//   order),
//   * how many tabs of each live in THIS window,
//   * which one this window is currently showing.
//
// It never decides whether a switch is ALLOWED: that is
// EnvironmentVisibilityController
// ::SwitchTo(), which fails closed for a deleted or disabled environment. A
// menu that duplicated that check would drift from it, and the drift would show
// up as a menu entry that looks enabled and does nothing.
//
// It is BOTH the model and its own delegate. SimpleMenuModel asks its delegate
// for the check mark, the enabled state and the command execution, and those
// answers depend on the same `entries_` list the model just built, so splitting
// them into two objects would mean two places that can disagree about row i.
//
// The menu is rebuilt on every open (see EnvironmentSwitcherButton), so the tab
// counts and the check mark are always current. That is deliberate: caching
// them would need an observer for every tab change in every environment, for a
// menu that is open for a couple of seconds.
class EnvironmentMenuModel : public ui::SimpleMenuModel,
                             public ui::SimpleMenuModel::Delegate {
 public:
  // One row of the menu.
  struct Entry {
    // Stable environment id, or empty for a non-selectable row (separator,
    // "manage").
    std::string environment_id;
    Environment environment;
    // Tabs of this environment in the window that opened the menu.
    int tab_count = 0;
    bool is_current = false;
    // A disabled environment is listed (the user must be able to see that it
    // exists) but cannot be selected.
    bool is_enabled = true;
  };

  // `window` supplies the profile (definitions) and the tab strip (counts).
  // Must outlive the model, which is why the model is owned by the button that
  // lives exactly as long as the window's toolbar.
  explicit EnvironmentMenuModel(BrowserWindowInterface* window);
  EnvironmentMenuModel(const EnvironmentMenuModel&) = delete;
  EnvironmentMenuModel& operator=(const EnvironmentMenuModel&) = delete;
  ~EnvironmentMenuModel() override;

  // Command id offset. Environment rows start here so they can never collide
  // with SimpleMenuModel's internal ids.
  static constexpr int kFirstEnvironmentCommandId = 5000;
  static constexpr int kManageCommandId = 4000;
  // The title row is added with AddTitle(), which owns its own internal id, so
  // this is NOT used for the title itself. It is kept as the id that Rebuild()
  // would use if the title ever became an actionable row (for example a
  // "manage" link in the header), and to document that 3000 is reserved and
  // must not be reused.
  static constexpr int kTitleCommandId = 3000;

  // Rebuilds the item list from the store and the live tab strip.
  //
  // Called once per menu open. Separate from the constructor because the model
  // is reused across opens: a menu built in the constructor would show the tab
  // counts from window creation time forever.
  void Rebuild();

  // Invoked with the chosen environment id once the menu has closed.
  //
  // The model does NOT switch by itself: it has no way to report a refusal, and
  // putting the switching rules here would create a second copy of them. The
  // owner (the toolbar button) supplies the callback and performs the switch.
  using SelectionCallback = base::RepeatingCallback<void(const std::string&)>;
  void set_selection_callback(SelectionCallback callback) {
    selection_callback_ = std::move(callback);
  }

  // The environment the user selected in the last ExecuteCommand(), or empty
  // when the chosen row was not an environment (separator, "manage").
  //
  // Read from MenuClosed(), i.e. AFTER the menu is gone. Executing the switch
  // from ExecuteCommand() would run it while the menu is still tearing down,
  // and the switch re-lays out the tab strip and may destroy focus -- doing
  // that under a closing menu is how a UI ends up with a stale or double-closed
  // menu.
  const std::string& last_selected_environment() const {
    return last_selected_environment_;
  }

  // Invoked once the menu has closed, when the user chose "管理环境".
  //
  // Delivered from MenuClosed() for the same reason as a selection: the
  // callback opens or focuses a tab, which re-lays out views, and doing that
  // under a menu that is still tearing down is how a UI ends up with a stale or
  // double-closed menu.
  using ManageCallback = base::RepeatingClosure;
  void set_manage_callback(ManageCallback callback) {
    manage_callback_ = std::move(callback);
  }

  // The entries built by the last Rebuild(), in menu order. Exposed for the
  // button's tests and for logging; the menu view itself only needs them to
  // draw.
  const std::vector<Entry>& entries() const { return entries_; }

 private:
  // ui::SimpleMenuModel::Delegate. Qualified so the compiler resolves them
  // against the Delegate base class specifically: this type inherits both
  // SimpleMenuModel and SimpleMenuModel::Delegate, and an unqualified
  // `override` on these would be ambiguous about which base it overrides.
  bool IsCommandIdChecked(int command_id) const override;
  bool IsCommandIdEnabled(int command_id) const override;
  void ExecuteCommand(int command_id, int event_flags) override;
  void MenuClosed(ui::SimpleMenuModel* source) override;

  // Adds one environment row, including its "selected" check mark.
  void AddEnvironmentRow(const Environment& environment,
                         int tab_count,
                         bool is_current);

  raw_ptr<BrowserWindowInterface> window_;
  raw_ptr<TabStripModel> tab_strip_;

  std::vector<Entry> entries_;

  std::string last_selected_environment_;
  bool manage_requested_ = false;

  SelectionCallback selection_callback_;
  ManageCallback manage_callback_;
};

}  // namespace parra

#endif  // CHROME_BROWSER_PARRA_UI_ENVIRONMENT_MENU_MODEL_H_
