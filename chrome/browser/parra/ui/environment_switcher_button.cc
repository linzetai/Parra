// Copyright 2026 The Parra Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chrome/browser/parra/ui/environment_switcher_button.h"

#include <memory>
#include <utility>

#include "base/check.h"
#include "base/command_line.h"
#include "base/functional/bind.h"
#include "base/location.h"
#include "base/logging.h"
#include "base/strings/utf_string_conversions.h"
#include "base/task/single_thread_task_runner.h"
#include "base/time/time.h"
#include "chrome/browser/parra/environment_store.h"
#include "chrome/browser/parra/environment_tab_operations.h"
#include "chrome/browser/parra/environment_tag.h"
#include "chrome/browser/parra/environment_visibility_controller.h"
#include "chrome/browser/parra/parra_storage_partition_util.h"
#include "chrome/browser/parra/ui/environment_menu_model.h"
#include "chrome/browser/parra/ui/environment_ui_util.h"
#include "chrome/browser/parra/ui/resources/grit/parra_strings.h"
#include "chrome/browser/parra/window_environment_state.h"
#include "chrome/browser/profiles/profile.h"
#include "chrome/browser/ui/browser_window/public/browser_window_interface.h"
#include "chrome/browser/ui/tabs/tab_strip_model.h"
#include "chrome/browser/ui/view_ids.h"
#include "chrome/browser/ui/views/toolbar/toolbar_button.h"
#include "chrome/common/webui_url_constants.h"
#include "content/public/browser/navigation_controller.h"
#include "content/public/browser/web_contents.h"
#include "content/public/common/url_constants.h"
#include "third_party/skia/include/core/SkPathBuilder.h"
#include "ui/base/l10n/l10n_util.h"
#include "ui/base/metadata/metadata_impl_macros.h"
#include "ui/base/models/menu_model.h"
#include "ui/base/mojom/menu_source_type.mojom.h"
#include "ui/base/page_transition_types.h"
#include "ui/base/window_open_disposition.h"
#include "ui/color/color_id.h"
#include "ui/color/color_provider.h"
#include "ui/gfx/canvas.h"
#include "ui/gfx/color_utils.h"
#include "ui/gfx/geometry/insets.h"
#include "ui/gfx/geometry/point.h"
#include "ui/gfx/geometry/rect.h"
#include "ui/gfx/geometry/rect_f.h"
#include "ui/gfx/geometry/rounded_corners_f.h"
#include "ui/native_theme/native_theme.h"
#include "ui/views/accessibility/view_accessibility.h"
#include "ui/views/background.h"
#include "ui/views/border.h"
#include "ui/views/controls/highlight_path_generator.h"
#include "ui/views/controls/label.h"
#include "ui/views/controls/menu/menu_item_view.h"
#include "ui/views/controls/menu/menu_model_adapter.h"
#include "ui/views/controls/menu/menu_runner.h"
#include "ui/views/controls/menu/submenu_view.h"
#include "ui/views/layout/fill_layout.h"
#include "ui/views/vector_icons.h"
#include "ui/views/view_class_properties.h"
#include "ui/views/widget/widget.h"
#include "url/gurl.h"

namespace parra {

namespace {

// Geometry. Kept as constants here rather than as layout-provider metrics
// because they are component-local: only this control uses them, and adding a
// global layout constant would make a Parra detail part of the shared metric
// set.
constexpr int kIdentityIconSize = 16;
constexpr int kIconLabelSpacing = 0;
constexpr int kChevronSize = 12;
constexpr int kLabelChevronSpacing = 2;
constexpr int kMinButtonWidth = 66;
constexpr int kMaxButtonWidth = 156;

}  // namespace

EnvironmentSwitcherButton::EnvironmentSwitcherButton(
    BrowserWindowInterface* window)
    : ToolbarButton(
          base::BindRepeating(&EnvironmentSwitcherButton::ButtonPressed,
                              base::Unretained(this)),
          std::make_unique<EnvironmentMenuModel>(window),
          // Deliberately null, NOT window->GetTabStripModel().
          //
          // ToolbarButton::ShowMenuForModel() bails out right after
          // setting the pressed state when a non-null tab strip has no
          // active WebContents. An EMPTY environment has exactly that
          // shape, so passing the tab strip would make the menu -- and
          // therefore the only way out of an empty environment --
          // unavailable, while leaving the button stuck in the pressed
          // state. This button never needs the tab strip: its model is
          // rebuilt from the window on every open.
          nullptr,
          /*trigger_menu_on_long_press=*/false),
      window_(window) {
  SetID(VIEW_ID_PARRA_ENVIRONMENT_SWITCHER);
  SetFocusBehavior(views::View::FocusBehavior::ALWAYS);

  // The menu model is owned by ToolbarButton (as `model_`), because that is the
  // only shape its menu pipeline supports: ShowDropDownMenu() runs
  // model_.get(). The button keeps a non-owning pointer for rebuilding and for
  // reading the selection.
  menu_model_ = static_cast<EnvironmentMenuModel*>(menu_model());
  CHECK(menu_model_);
  menu_model_->set_selection_callback(
      base::BindRepeating(&EnvironmentSwitcherButton::OnEnvironmentSelected,
                          base::Unretained(this)));
  menu_model_->set_manage_callback(base::BindRepeating(
      &EnvironmentSwitcherButton::OnManageRequested, base::Unretained(this)));

  // ToolbarButton's own UpdateColorsAndInsets() dereferences
  // kInternalPaddingKey unconditionally, so the property must exist from the
  // very first layout pass. UpdateColorsAndInsets() below also sets it; doing
  // it here too means the very first base-class call (which can happen inside
  // this constructor's SetText path) cannot see it missing.
  SetProperty(views::kInternalPaddingKey, gfx::Insets());

  // Rounded highlight that follows the button's own corner radius, so the ink
  // drop matches the toolbar's other controls instead of being a rectangle.
  views::InstallRoundRectHighlightPathGenerator(this, gfx::Insets(),
                                                GetRoundedCornerRadius());

  // Text is elided rather than wrapped or clipped: the spec requires the entry
  // never to disappear when the window narrows, so the label gives way first.
  label()->SetElideBehavior(gfx::ELIDE_TAIL);
  label()->SetMaximumWidthSingleLine(kMaxButtonWidth);
  label()->SetHorizontalAlignment(gfx::ALIGN_LEFT);
  label()->SetFontList(
      label()->font_list().DeriveWithWeight(gfx::Font::Weight::SEMIBOLD));

  // The chevron is the image half of the button. The shared outline Layers
  // glyph is painted before the label; it keeps the same shape in every
  // environment.
  SetImageLabelSpacing(kChevronSize + kLabelChevronSpacing);
  SetMinSize(gfx::Size(kMinButtonWidth, 0));

  // Try to subscribe now. This succeeds when the window's environment state
  // already exists and silently does nothing when it does not (see
  // EnsureSubscribed()).
  EnsureSubscribed();

  UpdateFromWindow();

  SetVisible(ShouldBeVisible());
}

EnvironmentSwitcherButton::~EnvironmentSwitcherButton() {
  feedback_timer_.Stop();
  if (feedback_widget_) {
    feedback_widget_->CloseNow();
  }
}

std::string EnvironmentSwitcherButton::QueryActiveEnvironment() const {
  if (!window_) {
    return std::string();
  }
  if (WindowEnvironmentState* state =
          WindowEnvironmentState::GetFor(window_.get())) {
    return state->active_environment();
  }
  // No window state means Parra never initialised this window. Reporting
  // `personal` is correct rather than a guess: an uninitialised window is an
  // ordinary Chromium window, and an ordinary window IS the Personal
  // environment.
  return std::string(kDefaultEnvironmentId);
}

bool EnvironmentSwitcherButton::ShouldBeVisible() const {
  Profile* profile = window_ ? window_->GetProfile() : nullptr;
  return profile && profile->IsRegularProfile();
}

void EnvironmentSwitcherButton::UpdateFromWindow() {
  displayed_environment_ = QueryActiveEnvironment();

  // Resolve the definition so the label can show the user-visible NAME and the
  // stored colour. A missing definition (deleted while its tabs are still open)
  // is not an error here: the window really is on an environment that no longer
  // exists, and showing its raw id is more honest than showing nothing.
  Environment environment = ResolveDisplayedEnvironment();
  accent_color_ = EnvironmentDotColor(environment);
  if (!accent_color_) {
    accent_color_ = SkColorSetRGB(0x6b, 0x80, 0x74);
  }
  UpdateEnvironmentBackground();
  if (theme_changed_callback_) {
    theme_changed_callback_.Run(*accent_color_);
  }

  std::u16string name = EnvironmentDisplayName(environment);

  // A nameless environment can only come from a record this build does not
  // understand. Rather than rendering an empty button (which would look like a
  // layout bug), fall back to the localized generic label.
  if (name.empty()) {
    name = l10n_util::GetStringUTF16(IDS_PARRA_ENVIRONMENT_SWITCHER_BUTTON);
  }

  full_name_ = name;
  // Keep the actual name intact for accessibility and use the available width
  // to elide it. A fixed three-character prefix made distinct names identical.
  label()->SetMaximumWidthSingleLine(layout_mode_ == 2   ? 96
                                     : layout_mode_ == 1 ? 94
                                                         : 116);
  SetMaxSize(gfx::Size(layout_mode_ == 2   ? 136
                       : layout_mode_ == 1 ? 132
                                           : kMaxButtonWidth,
                       0));
  // A descriptive prefix before a separator makes long configured names useful
  // in the narrow strip (for example, "工作环境：跨团队…" -> "工作环境").
  std::u16string visible_name = full_name_;
  if (layout_mode_ == 2) {
    const size_t separator = visible_name.find_first_of(u"：:·/|");
    if (separator != std::u16string::npos && separator > 1) {
      visible_name.resize(separator);
    }
  }
  SetText(visible_name);

  const std::u16string tooltip =
      l10n_util::GetStringFUTF16(IDS_PARRA_ENVIRONMENT_SWITCHER_TOOLTIP, name);
  SetTooltipText(tooltip);
  GetViewAccessibility().SetName(tooltip);

  // The chevron is drawn with the same foreground colour as the text, so the
  // control picks up light/dark/high-contrast themes without a custom colour.
  //
  // NOT DONE IN THE CONSTRUCTOR. UpdateIconsWithStandardColors() resolves the
  // colour through GetColorProvider(), which is null until the view is attached
  // to a widget that has a theme -- and ToolbarView constructs this button
  // before that point. Calling it from the constructor dereferenced null and
  // killed the process with an access violation during startup (0xC0000005)
  // with no CHECK message. OnThemeChanged() below runs at exactly the moment
  // the provider becomes available, and UpdateFromWindow() re-applies it on
  // every environment change.
  if (GetColorProvider()) {
    UpdateIconsWithStandardColors(views::kArrowDropDownIcon);
  }

  SetVisible(ShouldBeVisible());

  // The width depends on the label, which just changed. UpdateColorsAndInsets()
  // runs as part of the layout that InvalidateLayout() schedules, and it is
  // what applies the glyph's leading padding.
  InvalidateLayout();
  PreferredSizeChanged();
  SchedulePaint();
}

Environment EnvironmentSwitcherButton::ResolveDisplayedEnvironment() const {
  Environment environment;
  environment.id = displayed_environment_;

  Profile* profile = window_ ? window_->GetProfile() : nullptr;
  if (EnvironmentStore* store =
          profile ? EnvironmentStore::GetForProfile(profile) : nullptr) {
    if (const Environment* stored = store->Find(displayed_environment_)) {
      return *stored;
    }
    if (displayed_environment_ == kDefaultEnvironmentId) {
      return store->PersonalEnvironment();
    }
  } else if (displayed_environment_ == kDefaultEnvironmentId) {
    // No store: a build-time or very-early-startup shell. Personal still has a
    // display name, so the button is not blank.
    environment.name = "Personal";
  }

  return environment;
}

bool EnvironmentSwitcherButton::SwitchTo(const std::string& environment_id) {
  if (!window_ || environment_id.empty()) {
    return false;
  }

  if (base::CommandLine::ForCurrentProcess()->HasSwitch(
          "parra-ui-test-refuse-switch")) {
    LOG(WARNING) << "parra-ui: test switch refusal for '" << environment_id
                 << "'; the window stays on '" << displayed_environment_ << "'";
    return false;
  }

  // Use the existing switcher rather than reimplementing the steps. It already:
  //   * refuses a deleted or disabled environment (fail closed),
  //   * remembers the outgoing environment's tab,
  //   * activates the target's remembered tab,
  //   * collapses the change into ONE notification, which is what makes the tab
  //     strip re-lay out exactly once at the end.
  EnvironmentVisibilityController controller(window_->GetTabStripModel());
  if (!controller.SwitchTo(environment_id)) {
    ++refused_switch_count_;
    // The window is deliberately left where it was. Switching to something else
    // "because the target was unavailable" would be a silent downgrade.
    LOG(WARNING) << "parra-ui: environment switch to '" << environment_id
                 << "' was refused; the window stays on '"
                 << displayed_environment_ << "'";
    return false;
  }

  // The switch changed the window's environment, which notified
  // WindowEnvironmentState, which called UpdateFromWindow() through this
  // button's subscription. Updating again here would be redundant, but calling
  // it is harmless and makes the button correct even if a future change drops
  // the notification.
  UpdateFromWindow();
  return true;
}

void EnvironmentSwitcherButton::AddedToWidget() {
  ToolbarButton::AddedToWidget();
  EnsureSubscribed();
  UpdateFromWindow();
}

void EnvironmentSwitcherButton::SetThemeChangedCallback(
    base::RepeatingCallback<void(SkColor)> callback) {
  theme_changed_callback_ = std::move(callback);
  UpdateFromWindow();
}

void EnvironmentSwitcherButton::SetFeedbackCallback(
    base::RepeatingCallback<void(const std::u16string&)> callback) {
  feedback_callback_ = std::move(callback);
}

void EnvironmentSwitcherButton::BindEnvironmentState() {
  EnsureSubscribed();
  UpdateFromWindow();
}

void EnvironmentSwitcherButton::SetAvailableStripWidth(int width) {
  const int mode = width < 650 ? 2 : width < 900 ? 1 : 0;
  if (layout_mode_ == mode) {
    return;
  }
  layout_mode_ = mode;
  UpdateFromWindow();
}

void EnvironmentSwitcherButton::EnsureSubscribed() {
  if (environment_changed_subscription_) {
    return;
  }
  if (!window_) {
    return;
  }

  WindowEnvironmentState* state = WindowEnvironmentState::GetFor(window_.get());
  if (!state) {
    return;
  }

  environment_changed_subscription_ =
      state->AddActiveEnvironmentChangedCallback(
          base::BindRepeating(&EnvironmentSwitcherButton::UpdateFromWindow,
                              base::Unretained(this)));
}

void EnvironmentSwitcherButton::ButtonPressed() {
  // Posted rather than called inline: ShowDropDownMenu() runs a nested message
  // loop (MenuRunner::RunMenuAt), and starting a nested loop from inside the
  // button's own mouse-event handling re-enters the event dispatch that is
  // still on the stack. Posting returns to the message loop first, which is
  // what the base class does for its long-press path too.
  base::SingleThreadTaskRunner::GetCurrentDefault()->PostTask(
      FROM_HERE, base::BindOnce(&EnvironmentSwitcherButton::ShowDropDownMenu,
                                weak_factory_.GetWeakPtr(),
                                ui::mojom::MenuSourceType::kMouse));
}

void EnvironmentSwitcherButton::ShowDropDownMenu(
    ui::mojom::MenuSourceType source_type) {
  if (!window_ || !menu_model_) {
    return;
  }

  // Rebuilt on every open so the tab counts and the check mark describe the
  // window as it is now. A cached model would keep showing the counts from
  // window creation time.
  menu_model_->Rebuild();

  // The native menu remains responsible for keyboard, hover and disabled state.
  // A fixed minimum width gives the secondary tab count enough room, while a
  // minor text mark puts the current selection on the trailing edge.
  SetState(STATE_PRESSED);
  parra_menu_adapter_ = std::make_unique<views::MenuModelAdapter>(
      menu_model_,
      base::BindRepeating(&EnvironmentSwitcherButton::OnParraMenuClosed,
                          weak_factory_.GetWeakPtr()));
  parra_menu_adapter_->set_triggerable_event_flags(GetTriggerableEventFlags());
  auto root = parra_menu_adapter_->CreateMenu();
  root->GetSubmenu()->set_minimum_preferred_width(264);
  const auto& entries = menu_model_->entries();
  for (size_t i = 0; i < entries.size(); ++i) {
    if (auto* row = root->GetMenuItemByID(
            EnvironmentMenuModel::kFirstEnvironmentCommandId +
            static_cast<int>(i))) {
      row->set_vertical_margin(9);
      root->SetTooltip(EnvironmentDisplayName(entries[i].environment),
                       EnvironmentMenuModel::kFirstEnvironmentCommandId +
                           static_cast<int>(i));
      if (entries[i].is_current) {
        row->SetMinorText(u"✓");
      }
    }
  }
  parra_menu_runner_ = std::make_unique<views::MenuRunner>(
      std::move(root), views::MenuRunner::HAS_MNEMONICS);
  parra_menu_runner_->RunMenuAt(GetWidget(), nullptr, GetAnchorBoundsInScreen(),
                                views::MenuAnchorPosition::kTopLeft,
                                source_type);
}

void EnvironmentSwitcherButton::OnParraMenuClosed() {
  parra_menu_runner_.reset();
  parra_menu_adapter_.reset();
  SetState(STATE_NORMAL);
}

void EnvironmentSwitcherButton::OnEnvironmentSelected(
    const std::string& environment_id) {
  if (environment_id.empty()) {
    return;
  }

  if (environment_id == displayed_environment_) {
    // Choosing the environment already shown is a no-op, not a failure: the
    // user asked for the state they are already in.
    return;
  }

  ShowSwitchFeedback(SwitchTo(environment_id));
}

void EnvironmentSwitcherButton::ShowSwitchFeedback(bool success) {
  if (!GetWidget()) {
    return;
  }
  const std::u16string message =
      success ? l10n_util::GetStringFUTF16(IDS_PARRA_ENVIRONMENT_SWITCHED,
                                           full_name_)
              : l10n_util::GetStringUTF16(IDS_PARRA_ENVIRONMENT_SWITCH_FAILED);
  feedback_timer_.Stop();
  if (feedback_widget_) {
    feedback_widget_->CloseNow();
    feedback_widget_.reset();
  }
  auto label = std::make_unique<views::Label>(message);
  label->SetBorder(views::CreateEmptyBorder(gfx::Insets::TLBR(9, 13, 9, 13)));
  const auto tokens = MakeBrowserUiTokens(
      accent_color_.value_or(SkColorSetRGB(0x6b, 0x80, 0x74)),
      GetColorProvider()->GetColor(kColorToolbar));
  label->SetEnabledColor(success ? tokens.text_primary : tokens.danger);
  auto surface = std::make_unique<views::View>();
  surface->SetLayoutManager(std::make_unique<views::FillLayout>());
  surface->SetBackground(views::CreateRoundedRectBackground(
      tokens.menu_surface, gfx::RoundedCornersF(9)));
  surface->SetBorder(views::CreateRoundedRectBorder(1, 9, tokens.border));
  surface->AddChildView(std::move(label));
  const gfx::Size size = surface->GetPreferredSize();
  views::Widget::InitParams params(
      views::Widget::InitParams::CLIENT_OWNS_WIDGET,
      views::Widget::InitParams::TYPE_POPUP);
  params.parent = GetWidget()->GetNativeView();
  params.activatable = views::Widget::InitParams::Activatable::kNo;
  params.opacity = views::Widget::InitParams::WindowOpacity::kTranslucent;
  gfx::Rect anchor = GetBoundsInScreen();
  params.bounds =
      gfx::Rect(anchor.x(), anchor.bottom() + 6, size.width(), size.height());
  feedback_widget_ = std::make_unique<views::Widget>();
  feedback_widget_->Init(std::move(params));
  feedback_widget_->SetContentsView(std::move(surface));
  feedback_widget_->ShowInactive();
  feedback_timer_.Start(
      FROM_HERE, base::Seconds(2),
      base::BindOnce(
          [](base::WeakPtr<EnvironmentSwitcherButton> button) {
            if (button && button->feedback_widget_) {
              button->feedback_widget_->CloseNow();
              button->feedback_widget_.reset();
            }
          },
          weak_factory_.GetWeakPtr()));
}

void EnvironmentSwitcherButton::OnManageRequested() {
  if (!window_ || !window_->GetProfile() ||
      !window_->GetProfile()->IsRegularProfile()) {
    return;
  }
  TabStripModel* model = window_->GetTabStripModel();
  if (!model) {
    return;
  }

  if (EnvironmentTabOperations::FindBrowserLevelTab(model) !=
      TabStripModel::kNoTab) {
    model->ActivateTabAt(EnvironmentTabOperations::FindBrowserLevelTab(model));
    return;
  }

  auto contents = content::WebContents::Create(
      content::WebContents::CreateParams(window_->GetProfile()));
  BrowserLevelTab::Mark(contents.get());
  content::WebContents* opened =
      EnvironmentTabOperations::InsertOrActivateBrowserLevelTab(
          model, std::move(contents));
  if (!opened) {
    LOG(ERROR) << "parra-menu: could not open environment management tab";
    return;
  }
  content::NavigationController::LoadURLParams params{
      GURL(chrome::kChromeUIParraURL)};
  params.transition_type = ui::PAGE_TRANSITION_AUTO_BOOKMARK;
  opened->GetController().LoadURLWithParams(params);
}

bool EnvironmentSwitcherButton::ShouldShowMenu() {
  // Always true: this button's only purpose is to open the menu, and the model
  // is supplied at construction. Returning the base implementation's answer
  // (model_ != nullptr) would also work, but stating it here makes the intent
  // explicit and keeps the button from silently becoming a no-op if the model
  // plumbing ever changes. This is also what makes the right-click path
  // (ToolbarButton::ShowContextMenuForViewImpl -> ShowDropDownMenu) work
  // without any extra code.
  return window_ != nullptr && menu_model_ != nullptr;
}

void EnvironmentSwitcherButton::UpdateColorsAndInsets() {
  // Set the leading padding BEFORE delegating:
  // ToolbarButton::UpdateColorsAndInsets() reads kInternalPaddingKey to build
  // its paint insets and border, so the value has to be in place when it runs.
  //
  // The value is SET, never accumulated. An earlier draft added to whatever was
  // already there, which made the button grow by 14px on every theme change
  // until the label was pushed out of the toolbar.
  //
  // kInternalPaddingKey must always be PRESENT, never cleared: the base
  // implementation dereferences the property unconditionally
  // (`*GetProperty(views::kInternalPaddingKey)`), so removing it -- which an
  // earlier draft did for the "no icon" case -- crashed the browser at startup.
  // Always reserve the same 16 DIP outline icon position.
  SetImageLabelSpacing(layout_mode_ == 2 ? 8
                                         : kChevronSize + kLabelChevronSpacing);
  SetProperty(
      views::kInternalPaddingKey,
      gfx::Insets::TLBR(0, kIdentityIconSize + kIconLabelSpacing, 0, 0));

  ToolbarButton::UpdateColorsAndInsets();
  UpdateEnvironmentBackground();
}

void EnvironmentSwitcherButton::UpdateEnvironmentBackground() {
  if (!accent_color_ || !GetColorProvider() ||
      GetNativeTheme()->forced_colors() !=
          ui::ColorProviderKey::ForcedColors::kNone) {
    return;
  }
  const auto tokens = MakeBrowserUiTokens(
      *accent_color_, GetColorProvider()->GetColor(kColorToolbar));
  const SkColor fill =
      GetState() == STATE_PRESSED   ? tokens.environment_surface_pressed
      : GetState() == STATE_HOVERED ? tokens.environment_surface_hover
                                    : tokens.environment_surface;
  SetBackground(views::CreateRoundedRectBackground(
      fill, gfx::RoundedCornersF(0), gfx::Insets::TLBR(3, 0, 0, 0)));
  SetEnabledTextColors(color_utils::IsDark(fill)
                           ? SK_ColorWHITE
                           : tokens.environment_foreground);
}

void EnvironmentSwitcherButton::StateChanged(ButtonState old_state) {
  ToolbarButton::StateChanged(old_state);
  UpdateEnvironmentBackground();
}

void EnvironmentSwitcherButton::PaintButtonContents(gfx::Canvas* canvas) {
  if (!accent_color_ || !GetColorProvider()) {
    return;
  }

  // Three outlined rhombi, a single Layers glyph. Draw in DIP coordinates with
  // antialiasing so 100%, 125% and 150% scale factors keep the same stroke
  // weight.
  const gfx::Insets insets = GetInsets();
  // Keep the glyph at its original horizontal centre as the label moves closer.
  const float x = static_cast<float>(insets.left()) / 2.0f - 3.5f;
  const float y = static_cast<float>(height()) / 2.0f - 8.0f;
  const bool forced_colors = GetNativeTheme()->forced_colors() !=
                             ui::ColorProviderKey::ForcedColors::kNone;
  const SkColor fill = GetColorProvider()->GetColor(kColorToolbar);
  const auto tokens = MakeBrowserUiTokens(*accent_color_, fill);

  cc::PaintFlags flags;
  flags.setAntiAlias(true);
  flags.setStyle(cc::PaintFlags::kStroke_Style);
  flags.setStrokeWidth(1.55f);
  flags.setStrokeCap(cc::PaintFlags::kRound_Cap);
  flags.setStrokeJoin(cc::PaintFlags::kRound_Join);
  flags.setColor(forced_colors
                     ? GetColorProvider()->GetColor(kColorToolbarButtonIcon)
                 : color_utils::IsDark(tokens.environment_surface)
                     ? SK_ColorWHITE
                     : tokens.environment_foreground);

  SkPathBuilder layers;
  layers.moveTo(x + 8.0f, y + 1.8f);
  layers.lineTo(x + 14.2f, y + 5.2f);
  layers.lineTo(x + 8.0f, y + 8.6f);
  layers.lineTo(x + 1.8f, y + 5.2f);
  layers.close();
  layers.moveTo(x + 1.8f, y + 8.4f);
  layers.lineTo(x + 8.0f, y + 11.8f);
  layers.lineTo(x + 14.2f, y + 8.4f);
  layers.moveTo(x + 1.8f, y + 11.2f);
  layers.lineTo(x + 8.0f, y + 14.6f);
  layers.lineTo(x + 14.2f, y + 11.2f);
  canvas->DrawPath(layers.detach(), flags);
}

void EnvironmentSwitcherButton::OnThemeChanged() {
  ToolbarButton::OnThemeChanged();

  // A second chance to subscribe, and the point at which the theme is known.
  // Both are needed here: OnThemeChanged() is guaranteed to run once the view
  // is attached, which is later than the window-state creation that the
  // constructor can miss.
  const bool was_subscribed = !!environment_changed_subscription_;
  EnsureSubscribed();

  // Re-read now that a state may finally be reachable. Without this the label
  // keeps the value read in the constructor -- "Personal" -- even though the
  // window has since moved to another environment, because subscribing only
  // delivers FUTURE changes.
  if (!was_subscribed && environment_changed_subscription_) {
    UpdateFromWindow();
  }

  // The colour provider exists from here on (View::OnThemeChanged is only
  // called once the view can resolve theme colours), which is why the chevron
  // is painted here rather than in the constructor.
  UpdateIconsWithStandardColors(views::kArrowDropDownIcon);

  if (accent_color_) {
    SchedulePaint();
  }
}

BEGIN_METADATA(EnvironmentSwitcherButton)
END_METADATA

}  // namespace parra
