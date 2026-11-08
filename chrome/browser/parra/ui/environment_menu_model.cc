// Copyright 2026 The Parra Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chrome/browser/parra/ui/environment_menu_model.h"

#include <utility>

#include "base/logging.h"
#include "base/strings/utf_string_conversions.h"
#include "chrome/browser/parra/environment_index_mapper.h"
#include "chrome/browser/parra/environment_store.h"
#include "chrome/browser/parra/environment_tag.h"
#include "chrome/browser/parra/ui/environment_ui_util.h"
#include "chrome/browser/parra/ui/resources/grit/parra_strings.h"
#include "chrome/browser/parra/window_environment_state.h"
#include "chrome/browser/profiles/profile.h"
#include "chrome/browser/ui/browser_window/public/browser_window_interface.h"
#include "chrome/browser/ui/tabs/tab_strip_model.h"
#include "ui/base/l10n/l10n_util.h"
#include "ui/base/models/image_model.h"
#include "ui/gfx/image/image_skia.h"
#include "ui/gfx/text_elider.h"

namespace parra {

namespace {

constexpr int kMenuLayersSizeDip = 16;

}  // namespace

EnvironmentMenuModel::EnvironmentMenuModel(BrowserWindowInterface* window)
    : ui::SimpleMenuModel(this), window_(window) {
  tab_strip_ = window_ ? window_->GetTabStripModel() : nullptr;
}
EnvironmentMenuModel::~EnvironmentMenuModel() = default;

void EnvironmentMenuModel::Rebuild() {
  // Clear first: SimpleMenuModel has no RemoveAllItems(), and rebuilding from
  // an empty model is the only way to be sure no stale row survives a store
  // change (a deleted environment would otherwise keep its command id).
  Clear();
  entries_.clear();
  last_selected_environment_.clear();
  manage_requested_ = false;

  Profile* profile = window_ ? window_->GetProfile() : nullptr;
  EnvironmentStore* store =
      profile ? EnvironmentStore::GetForProfile(profile) : nullptr;
  if (!store) {
    // No store means no definitions to show. Adding a "Manage..." row alone
    // would offer an action whose effect cannot be displayed, so this is an
    // empty menu.
    return;
  }

  // The window's currently shown environment decides the check mark, NOT the
  // active tab's tag: the two can differ transiently (a tab close forces
  // Chromium to activate a foreign tab before the adapter runs, see
  // AdoptActiveTabEnvironment), and the check mark must describe what the
  // window is showing.
  std::string current = kDefaultEnvironmentId;
  if (WindowEnvironmentState* state =
          WindowEnvironmentState::GetFor(window_.get())) {
    current = state->active_environment();
  }

  // The menu's TITLE, per the product spec ("标题：切换环境").
  //
  // AddTitle() rather than AddItem() + SetEnabledAt(false). The distinction is
  // not cosmetic: a disabled ITEM is still a menu item, so the keyboard starts
  // navigation on it and a disabled row cannot be moved off, which made the
  // first Down press a no-op and left the whole menu unreachable by keyboard.
  // AddTitle() adds a non-focusable title row, which is both what the spec asks
  // for visually and what keeps arrow-key navigation starting on the first real
  // entry.
  //
  // Discovered by driving the menu with Down/Enter during M1 validation: one
  // Down left the highlight on "Switch environment" instead of moving to
  // Personal.
  AddTitle(l10n_util::GetStringUTF16(IDS_PARRA_ENVIRONMENT_MENU_TITLE));

  EnvironmentIndexMapper mapper(tab_strip_);

  // Personal is pinned first. It is the reserved environment that maps to the
  // default partition, so it must always be reachable even in a profile with no
  // other definitions -- which is also what keeps this menu useful on a fresh
  // profile (where it then contains exactly one row).
  const Environment personal = store->PersonalEnvironment();
  AddEnvironmentRow(personal, mapper.GetEnvironmentTabCount(personal.id),
                    current == personal.id);

  for (const Environment& environment : store->environments()) {
    AddEnvironmentRow(environment,
                      mapper.GetEnvironmentTabCount(environment.id),
                      current == environment.id);
  }
  AddSeparator(ui::NORMAL_SEPARATOR);
  AddItem(kManageCommandId,
          l10n_util::GetStringUTF16(IDS_PARRA_ENVIRONMENT_MENU_MANAGE));
}

void EnvironmentMenuModel::AddEnvironmentRow(const Environment& environment,
                                             int tab_count,
                                             bool is_current) {
  const int index = GetItemCount();

  // The command id is derived from the ENVIRONMENT ENTRY's position, NOT from
  // the item's position in the menu.
  //
  // The two used to be the same, which was only true while every menu item was
  // an environment row. The disabled title row and the separator now occupy
  // item slots that have no entry, so `GetItemCount()` would put every command
  // id one or two ahead of the entry it describes -- IsCommandIdChecked() and
  // ExecuteCommand() both resolve through `entries_[command_id -
  // kFirstEnvironmentCommandId]`, so the check mark would land on the wrong row
  // and a click would switch to the wrong environment.
  const int command_id =
      kFirstEnvironmentCommandId + static_cast<int>(entries_.size());

  std::u16string label = EnvironmentDisplayName(environment);

  const std::u16string count_text = l10n_util::GetPluralStringFUTF16(
      IDS_PARRA_ENVIRONMENT_MENU_TAB_COUNT, tab_count);
  if (!environment.enabled) {
    label += u" ";
    label += l10n_util::GetStringUTF16(IDS_PARRA_ENVIRONMENT_MENU_DISABLED);
  }

  std::u16string visible_label;
  gfx::ElideString(label, 18, &visible_label);
  AddItem(command_id, visible_label);
  SetSecondaryLabel(index, count_text);

  // Same Layers outline as the strip entry; only the accent and name differ.
  SetIconForCommandId(
      command_id, ui::ImageModel::FromImageSkia(
                      EnvironmentLayersImage(environment, kMenuLayersSizeDip)));

  Entry entry;
  entry.environment_id = environment.id;
  entry.environment = environment;
  entry.tab_count = tab_count;
  entry.is_current = is_current;
  entry.is_enabled = environment.enabled;
  entries_.push_back(std::move(entry));

  // `personal` cannot be disabled, so `enabled` alone is the right answer here.
  // The check mark marks the CURRENT environment even when it is disabled: the
  // window can legitimately be sitting in an environment that was disabled
  // after the fact (its tabs keep running), and hiding that would misreport
  // where the user is.
  //
  // The check mark itself is NOT set here: SimpleMenuModel asks the delegate
  // (IsCommandIdChecked) for every row when the menu is shown, and this model
  // answers from `entries_`. Storing a second copy in the model would give two
  // places that can disagree.
  SetEnabledAt(index, environment.enabled);
}

bool EnvironmentMenuModel::IsCommandIdChecked(int command_id) const {
  for (size_t i = 0; i < entries_.size(); ++i) {
    if (kFirstEnvironmentCommandId + static_cast<int>(i) == command_id) {
      return entries_[i].is_current;
    }
  }
  return false;
}

bool EnvironmentMenuModel::IsCommandIdEnabled(int command_id) const {
  if (command_id == kManageCommandId) {
    return window_ && window_->GetProfile() &&
           window_->GetProfile()->IsRegularProfile();
  }

  for (size_t i = 0; i < entries_.size(); ++i) {
    if (kFirstEnvironmentCommandId + static_cast<int>(i) == command_id) {
      return entries_[i].is_enabled;
    }
  }
  return false;
}

void EnvironmentMenuModel::ExecuteCommand(int command_id, int event_flags) {
  if (command_id == kManageCommandId) {
    manage_requested_ = true;
    return;
  }

  for (size_t i = 0; i < entries_.size(); ++i) {
    if (kFirstEnvironmentCommandId + static_cast<int>(i) != command_id) {
      continue;
    }

    // Record the id only. The switch happens in MenuClosed(): executing it here
    // would re-lay out the tab strip while the menu is still open, and the tab
    // strip re-layout the switch triggers can move focus and destroy the very
    // widget the menu is anchored to.
    last_selected_environment_ = entries_[i].environment_id;
    return;
  }
}

void EnvironmentMenuModel::MenuClosed(ui::SimpleMenuModel* source) {
  // "管理环境" is dispatched here, before the selection is cleared, and the two
  // are mutually exclusive: chose-manage and chose-environment cannot both have
  // happened, because only one command executed.
  //
  // It has to be MenuClosed and not ExecuteCommand for the same reason the
  // environment selection does: the callback opens a tab (or focuses one),
  // which re-lays out views under a menu that is still tearing down.
  const bool open_manage = manage_requested_;
  manage_requested_ = false;

  // Hand the selection to the owner, then clear it: a stale id surviving into
  // the next open would look like a selection the user never made.
  if (!last_selected_environment_.empty() && selection_callback_) {
    selection_callback_.Run(last_selected_environment_);
  }
  last_selected_environment_.clear();

  if (open_manage) {
    // Logged because the failure mode this replaced was silent: the flag was
    // set, then cleared on close, and nothing consumed it. If the page does not
    // open, this line is what distinguishes "the command never ran" from "the
    // callback did nothing".
    LOG(INFO) << "parra-menu: menu closed with manage requested (callback set="
              << (manage_callback_ ? "yes" : "no") << ")";
    if (manage_callback_) {
      manage_callback_.Run();
    }
  }
}

}  // namespace parra
