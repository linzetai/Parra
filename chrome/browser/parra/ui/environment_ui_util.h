// Copyright 2026 The Parra Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CHROME_BROWSER_PARRA_UI_ENVIRONMENT_UI_UTIL_H_
#define CHROME_BROWSER_PARRA_UI_ENVIRONMENT_UI_UTIL_H_

#include <optional>
#include <string>

#include "chrome/browser/parra/environment.h"
#include "third_party/skia/include/core/SkColor.h"
#include "ui/gfx/image/image_skia.h"

namespace parra {

// Shared colors for the browser chrome. All surfaces are derived from the
// current environment accent and the theme's neutral toolbar color.
struct BrowserUiTokens {
  SkColor accent;
  SkColor surface;
  SkColor surface_hover;
  SkColor environment_surface;
  SkColor environment_surface_hover;
  SkColor environment_surface_pressed;
  SkColor environment_foreground;
  SkColor text_primary;
  SkColor text_secondary;
  SkColor active_tab_surface;
  SkColor inactive_tab_surface;
  SkColor border;
  SkColor focus_ring;
  SkColor menu_surface;
  SkColor success;
  SkColor danger;
};

BrowserUiTokens MakeBrowserUiTokens(SkColor accent, SkColor neutral);

// Presentation helpers for environment UI.
//
// Deliberately a thin, pure-function layer over the store's value types. Two
// reasons it is separate from the widgets:
//
//   1. These are the only places where a `StoreResult` or a stored "#RRGGBB"
//      string turns into something a user sees, so the "what does this mean for
//      the user" mapping exists once instead of in every dialog.
//   2. They are trivially testable and cannot touch a window, which keeps the
//      risky part (Views plumbing) free of formatting logic.

// Uses the toolbar luminance to keep the window surface neutral in light and
// dark themes.
SkColor EnvironmentSurfaceColor(SkColor environment, SkColor toolbar);

// Parses `Environment::color` ("#RRGGBB") into a colour.
//
// Returns nullopt for an unset or unparseable value. The store validates
// colours on write, so a failure here means data written by another build --
// reporting "no colour" is correct, and inventing one would show the user a
// colour that the environment does not actually have.
std::optional<SkColor> ParseEnvironmentColor(const std::string& color);

// The colour dot for an environment, or nullopt when it has no colour.
//
// `personal` is deliberately allowed to be colourless: it is the neutral
// baseline and colouring it would imply it is a "special" environment rather
// than the default one. Never returns a fallback colour -- a dot that is always
// painted cannot distinguish "this environment has a colour" from "it does
// not".
std::optional<SkColor> EnvironmentDotColor(const Environment& environment);

// The common outline Layers glyph for menu rows. Generated at 3x resolution and
// tinted with the environment accent; its shape is identical across
// environments.
gfx::ImageSkia EnvironmentLayersImage(const Environment& environment,
                                      int size_dip);

// The name to show for `environment`.
//
// Falls back to the id when the stored name is empty, because showing nothing
// would leave the user with an unidentifiable menu entry. The id is a stable,
// always-present value (validated `[a-z0-9_-]{1,32}`), so the fallback is
// legible rather than a placeholder.
std::u16string EnvironmentDisplayName(const Environment& environment);

// A one-line, user-facing description of a store failure.
//
// Grouped deliberately: several `StoreResult` values mean the same thing to a
// user ("that did not work, and nothing changed"), and listing all ten would be
// a menu of developer-facing distinctions. The distinctions that DO matter to
// the user -- disabled vs missing vs read-only -- are kept apart.
std::u16string StoreResultMessage(StoreResult result);

}  // namespace parra

#endif  // CHROME_BROWSER_PARRA_UI_ENVIRONMENT_UI_UTIL_H_
