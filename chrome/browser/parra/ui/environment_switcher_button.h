// Copyright 2026 The Parra Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CHROME_BROWSER_PARRA_UI_ENVIRONMENT_SWITCHER_BUTTON_H_
#define CHROME_BROWSER_PARRA_UI_ENVIRONMENT_SWITCHER_BUTTON_H_

#include <memory>
#include <optional>
#include <string>

#include "base/callback_list.h"
#include "base/functional/callback.h"
#include "base/memory/raw_ptr.h"
#include "base/memory/weak_ptr.h"
#include "base/timer/timer.h"
#include "chrome/browser/parra/environment.h"
#include "chrome/browser/ui/views/toolbar/toolbar_button.h"
#include "third_party/skia/include/core/SkColor.h"
#include "ui/base/metadata/metadata_header_macros.h"

class BrowserWindowInterface;
namespace views {
class MenuModelAdapter;
class MenuRunner;
class Widget;
}  // namespace views

namespace parra {

class EnvironmentMenuModel;
class EnvironmentSwitcherButtonTest;

// The toolbar control that shows this window's environment and opens the
// switcher menu.
//
// THREE JOBS, IN THIS ORDER OF IMPORTANCE
//
//   1. Report which environment the user is looking at. This is the capability
//      that did not exist at all before M1: the environment was real (its tabs,
//      cookies and host mappings were isolated) but invisible, so a user could
//      not tell which environment a window was on.
//   2. Let the user switch. The switch itself is performed by
//      EnvironmentVisibilityController::SwitchTo(), which fails closed; this
//      class only routes the request and reports the outcome.
//   3. Stay out of the way: no animation, no colour on the whole toolbar, no
//      custom theme. The button uses ToolbarButton's own theme colours so it
//      looks native in light, dark and high-contrast modes.
//
// VISIBILITY RULE
// Regular profiles always expose the switcher so a new profile can open
// environment management before any custom environment exists.
class EnvironmentSwitcherButton : public ToolbarButton {
  METADATA_HEADER(EnvironmentSwitcherButton, ToolbarButton)

 public:
  explicit EnvironmentSwitcherButton(BrowserWindowInterface* window);
  EnvironmentSwitcherButton(const EnvironmentSwitcherButton&) = delete;
  EnvironmentSwitcherButton& operator=(const EnvironmentSwitcherButton&) =
      delete;
  ~EnvironmentSwitcherButton() override;

  // The environment this button is currently displaying. Empty when the button
  // has no window (teardown).
  const std::string& displayed_environment() const {
    return displayed_environment_;
  }

  // Re-reads the environment from the window and updates its label and accent.
  void UpdateFromWindow();

  // Called by the tab strip after its window state has been created.
  void BindEnvironmentState();
  void SetAvailableStripWidth(int width);
  void SetThemeChangedCallback(base::RepeatingCallback<void(SkColor)> callback);
  void SetFeedbackCallback(
      base::RepeatingCallback<void(const std::u16string&)> callback);

  // Switches this window to `environment_id` and reports the outcome.
  //
  // Returns false when the switch was refused (environment deleted or
  // disabled). A refusal leaves the window on its current environment: this
  // method never falls back to another environment, because silently landing
  // the user somewhere else is worse than doing nothing.
  bool SwitchTo(const std::string& environment_id);

  // Number of times a switch was refused. Used by the acceptance checks.
  int refused_switch_count() const { return refused_switch_count_; }
  std::optional<SkColor> environment_color() const { return accent_color_; }

  // The menu model, for tests and for the acceptance harness. Owned by this
  // button.
  EnvironmentMenuModel* menu_model_for_testing() { return menu_model_.get(); }

 private:
  friend class EnvironmentSwitcherButtonTest;

  // ToolbarButton (and Button):
  void UpdateColorsAndInsets() override;
  void StateChanged(ButtonState old_state) override;
  void UpdateEnvironmentBackground();
  void OnThemeChanged() override;
  bool ShouldShowMenu() override;
  void AddedToWidget() override;
  // Draws the common 16 DIP outline Layers glyph in the leading padding.
  //
  // PaintButtonContents() is the supported hook for custom button painting:
  // Button::OnPaint() is `final` and paints the background, the border and the
  // focus ring around whatever this draws, which is what keeps the glyph
  // looking native in every theme.
  void PaintButtonContents(gfx::Canvas* canvas) override;

  // Opens / rebuilds the menu.
  //
  // Reached from two paths: ButtonPressed() (the normal left click, posted),
  // and ToolbarButton's right-click path via ShowContextMenuForViewImpl().
  // Rebuilding here rather than in the callers is what keeps the tab counts and
  // the check mark describing the window as it is at open time.
  void ShowDropDownMenu(ui::mojom::MenuSourceType source_type) override;
  void OnParraMenuClosed();

  // Subscribes to this window's environment changes, exactly once.
  //
  // Called from three places on purpose. ToolbarView constructs this button
  // while the window is still being built, and the window's environment state
  // is created later by BrowserTabStripController::InitFromModel(), so the
  // constructor cannot subscribe. Both AddedToWidget() and OnThemeChanged() run
  // after that point; the first one that has a state to attach to wins, and the
  // guard makes the others no-ops.
  void EnsureSubscribed();

  // Opens the menu. This IS the button's action.
  //
  // ToolbarButton does not open its menu on a plain left click by itself: its
  // OnMousePressed() only schedules ShowDropDownMenu() when
  // `trigger_menu_on_long_press_` is set, and OnMouseReleased() cancels that
  // scheduled task because a click-release is a triggerable event. Relying on
  // the base class therefore produced a button that visibly pressed and then
  // did nothing. The press callback is the supported place to start the menu.
  void ButtonPressed();

  // Performs the switch the menu reported. Called from the environment menu's
  // close notification, so the tab strip re-layout happens with no menu open.
  void OnEnvironmentSelected(const std::string& environment_id);
  void ShowSwitchFeedback(bool success);

  // Opens (or focuses) the profile's settings window. Also called from the
  // environment menu's close notification, for the same reason as the switch.
  void OnManageRequested();

  // The environment this window is on, from WindowEnvironmentState. Never empty
  // while a window exists; `personal` when there is no state.
  std::string QueryActiveEnvironment() const;

  // The definition to display: the stored record for the active id, the
  // reserved Personal environment, or a bare id-only value when the definition
  // is gone.
  Environment ResolveDisplayedEnvironment() const;

  // True when this profile has anything to switch between.
  bool ShouldBeVisible() const;

  raw_ptr<BrowserWindowInterface> window_;

  // Non-owning: ToolbarButton owns the model as `model_` (it must, so that its
  // menu pipeline can run it). Kept here for rebuilding and for reading the
  // selection.
  raw_ptr<EnvironmentMenuModel> menu_model_ = nullptr;

  // Notifies on every environment change for this window, including changes
  // made by session restore or by a native tab activation. The subscription is
  // a member so a window torn down mid-update cannot call back into a dead
  // button.
  base::CallbackListSubscription environment_changed_subscription_;

  std::string displayed_environment_;
  base::RepeatingCallback<void(SkColor)> theme_changed_callback_;
  base::RepeatingCallback<void(const std::u16string&)> feedback_callback_;
  base::OneShotTimer feedback_timer_;
  std::unique_ptr<views::Widget> feedback_widget_;
  std::unique_ptr<views::MenuModelAdapter> parra_menu_adapter_;
  std::unique_ptr<views::MenuRunner> parra_menu_runner_;
  std::u16string full_name_;
  int layout_mode_ = 0;

  // Parsed once per update so paint does not re-parse a string.
  std::optional<SkColor> accent_color_;

  int refused_switch_count_ = 0;

  base::WeakPtrFactory<EnvironmentSwitcherButton> weak_factory_{this};
};

}  // namespace parra

#endif  // CHROME_BROWSER_PARRA_UI_ENVIRONMENT_SWITCHER_BUTTON_H_
