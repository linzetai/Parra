// Copyright 2026 The Parra Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chrome/browser/parra/ui/environment_ui_util.h"

#include <string_view>

#include "base/strings/string_number_conversions.h"
#include "base/strings/utf_string_conversions.h"
#include "chrome/browser/parra/environment.h"
#include "chrome/browser/parra/environment_tag.h"
#include "chrome/browser/parra/ui/resources/grit/parra_strings.h"
#include "third_party/skia/include/core/SkBitmap.h"
#include "third_party/skia/include/core/SkCanvas.h"
#include "third_party/skia/include/core/SkPaint.h"
#include "third_party/skia/include/core/SkPathBuilder.h"
#include "ui/base/l10n/l10n_util.h"
#include "ui/gfx/color_utils.h"
#include "ui/gfx/image/image_skia.h"

namespace parra {

namespace {

// True when `c` is an ASCII hex digit.
//
// Written as a comparison rather than base::IsHexDigit so the accepted set is
// visible at the call site: this parser's whole job is to accept exactly the
// "#RRGGBB" form the store writes.
bool IsHexDigit(char c) {
  return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') ||
         (c >= 'A' && c <= 'F');
}

}  // namespace

SkColor EnvironmentSurfaceColor(SkColor environment, SkColor toolbar) {
  return MakeBrowserUiTokens(environment, toolbar).surface;
}

BrowserUiTokens MakeBrowserUiTokens(SkColor accent, SkColor neutral) {
  const bool dark = color_utils::IsDark(neutral);
  const SkColor base =
      dark ? SkColorSetRGB(0x25, 0x29, 0x28) : SkColorSetRGB(0xf1, 0xf2, 0xef);
  // Preserve the user's hue while ensuring very light and very dark custom
  // accents remain visible against the current window surface.
  const SkColor visible_accent =
      color_utils::BlendForMinContrast(
          accent, base, std::nullopt, color_utils::kMinimumVisibleContrastRatio)
          .color;
  const auto blend = [](SkColor foreground, uint8_t alpha, SkColor background) {
    return color_utils::GetResultingPaintColor(SkColorSetA(foreground, alpha),
                                               background);
  };
  const SkColor surface = blend(visible_accent, dark ? 27 : 25, base);
  const SkColor environment_surface =
      blend(visible_accent, dark ? 96 : 76, surface);
  const SkColor active =
      dark ? SkColorSetRGB(0x34, 0x39, 0x37) : SkColorSetRGB(0xff, 0xff, 0xfe);
  return {
      .accent = visible_accent,
      .surface = surface,
      .surface_hover = blend(visible_accent, dark ? 65 : 54, base),
      .environment_surface = environment_surface,
      .environment_surface_hover =
          blend(visible_accent, dark ? 125 : 105, surface),
      .environment_surface_pressed =
          blend(visible_accent, dark ? 145 : 126, surface),
      .environment_foreground = color_utils::IsDark(environment_surface)
                                    ? SK_ColorWHITE
                                    : SkColorSetRGB(0x20, 0x2c, 0x25),
      .text_primary = dark ? SK_ColorWHITE : SkColorSetRGB(0x25, 0x2b, 0x28),
      .text_secondary = dark ? SkColorSetRGB(0xb6, 0xbe, 0xb9)
                             : SkColorSetRGB(0x5d, 0x68, 0x62),
      .active_tab_surface = active,
      .inactive_tab_surface = surface,
      .border = blend(visible_accent, dark ? 74 : 48,
                      dark ? SkColorSetRGB(0x49, 0x50, 0x4c)
                           : SkColorSetRGB(0xd4, 0xd9, 0xd4)),
      .focus_ring = dark ? SkColorSetRGB(0xd8, 0xe7, 0xde)
                         : SkColorSetRGB(0x24, 0x55, 0x3c),
      .menu_surface = dark ? SkColorSetRGB(0x2c, 0x31, 0x2f)
                           : SkColorSetRGB(0xff, 0xff, 0xfe),
      .success = SkColorSetRGB(0x37, 0x70, 0x54),
      .danger = SkColorSetRGB(0xa8, 0x4c, 0x3e),
  };
}

std::optional<SkColor> ParseEnvironmentColor(const std::string& color) {
  // The store only admits "#RRGGBB" (see environment_serialization.cc). This
  // parser intentionally accepts exactly that and nothing else rather than
  // being lenient: a lenient parser would display a colour for a value the
  // store would have rejected, making a corrupt record look fine in the UI.
  if (color.size() != 7 || color.front() != '#') {
    return std::nullopt;
  }

  // Validated character by character before any numeric conversion, so a string
  // like "0x1234" (which HexStringToUInt would happily read as a prefixed
  // number) is rejected: 'x' is not a hex digit.
  const std::string_view hex = std::string_view(color).substr(1);
  for (char c : hex) {
    if (!IsHexDigit(c)) {
      return std::nullopt;
    }
  }

  uint32_t packed = 0;
  if (!base::HexStringToUInt(hex, &packed)) {
    return std::nullopt;
  }

  return SkColorSetRGB(static_cast<SkColor>(packed >> 16) & 0xFF,
                       static_cast<SkColor>(packed >> 8) & 0xFF,
                       static_cast<SkColor>(packed) & 0xFF);
}

std::optional<SkColor> EnvironmentDotColor(const Environment& environment) {
  if (auto color = ParseEnvironmentColor(environment.color)) {
    return color;
  }
  if (environment.id == "personal") {
    return SkColorSetRGB(0x6b, 0x80, 0x74);
  }
  // FNV-1a gives the same palette choice across processes and windows. The
  // immutable ID, rather than the display name, keeps renames stable.
  constexpr SkColor kPalette[] = {
      SkColorSetRGB(0x37, 0x70, 0x54), SkColorSetRGB(0x80, 0x84, 0x47),
      SkColorSetRGB(0x97, 0x63, 0x4c), SkColorSetRGB(0x67, 0x78, 0x49),
      SkColorSetRGB(0x9a, 0x60, 0x48), SkColorSetRGB(0xb4, 0x84, 0x36),
  };
  uint32_t hash = 2166136261u;
  for (unsigned char c : environment.id) {
    hash = (hash ^ c) * 16777619u;
  }
  return kPalette[hash % (sizeof(kPalette) / sizeof(kPalette[0]))];
}

namespace {

gfx::ImageSkia MakeLayersImage(int size, SkColor foreground) {
  if (size <= 0) {
    return gfx::ImageSkia();
  }

  constexpr int kScale = 3;
  SkBitmap bitmap;
  bitmap.allocN32Pixels(size * kScale, size * kScale, /*is_opaque=*/false);
  bitmap.eraseColor(SK_ColorTRANSPARENT);

  SkPaint paint;
  paint.setAntiAlias(true);
  paint.setStyle(SkPaint::kStroke_Style);
  paint.setStrokeWidth(1.55f);
  paint.setStrokeCap(SkPaint::kRound_Cap);
  paint.setStrokeJoin(SkPaint::kRound_Join);
  paint.setColor(foreground);

  SkCanvas canvas(bitmap);
  canvas.scale(kScale, kScale);
  const float offset = (size - 16.0f) / 2.0f;
  SkPathBuilder layers;
  layers.moveTo(offset + 8.0f, offset + 1.8f);
  layers.lineTo(offset + 14.2f, offset + 5.2f);
  layers.lineTo(offset + 8.0f, offset + 8.6f);
  layers.lineTo(offset + 1.8f, offset + 5.2f);
  layers.close();
  layers.moveTo(offset + 1.8f, offset + 8.4f);
  layers.lineTo(offset + 8.0f, offset + 11.8f);
  layers.lineTo(offset + 14.2f, offset + 8.4f);
  layers.moveTo(offset + 1.8f, offset + 11.2f);
  layers.lineTo(offset + 8.0f, offset + 14.6f);
  layers.lineTo(offset + 14.2f, offset + 11.2f);
  canvas.drawPath(layers.detach(), paint);

  return gfx::ImageSkia::CreateFromBitmap(bitmap, 3.0f);
}

}  // namespace

gfx::ImageSkia EnvironmentLayersImage(const Environment& environment,
                                      int size_dip) {
  return MakeLayersImage(size_dip,
                         EnvironmentDotColor(environment)
                             .value_or(SkColorSetRGB(0x6b, 0x80, 0x74)));
}

std::u16string EnvironmentDisplayName(const Environment& environment) {
  if (environment.id == kDefaultEnvironmentId &&
      environment.name == "Personal") {
    return l10n_util::GetStringUTF16(IDS_PARRA_ENVIRONMENT_PERSONAL_NAME);
  }
  if (!environment.name.empty()) {
    return base::UTF8ToUTF16(environment.name);
  }

  // The store validates that `name` is non-empty, so this is only reachable for
  // a record written by a build with different rules. Falling back to the id
  // keeps the entry identifiable instead of rendering a blank row.
  if (!environment.id.empty()) {
    return base::UTF8ToUTF16(environment.id);
  }

  // Both empty: a caller constructed an Environment by hand. There is no honest
  // name to invent, so return an empty string and let the caller decide; the
  // IDS string below is only used by StoreResultMessage and is unrelated.
  return std::u16string();
}

std::u16string StoreResultMessage(StoreResult result) {
  switch (result) {
    case StoreResult::kOk:
      return std::u16string();

    case StoreResult::kNotFound:
    case StoreResult::kReservedId:
      // "It is not there any more" and "that one is special" produce the same
      // user action: pick something else. The distinction matters in logs, not
      // on screen.
      return l10n_util::GetStringUTF16(IDS_PARRA_ENVIRONMENT_SWITCH_FAILED);

    case StoreResult::kReadOnly:
      // A configuration written by a newer build. Distinct from the generic
      // failure because the user CANNOT fix it by retrying, and telling them to
      // retry would be wrong.
      return l10n_util::GetStringUTF16(
          IDS_PARRA_ENVIRONMENT_SWITCH_FAILED_DISABLED);

    case StoreResult::kInvalidId:
    case StoreResult::kDuplicateId:
    case StoreResult::kInvalidField:
    case StoreResult::kInvalidHostMapping:
    case StoreResult::kPersistFailed:
    case StoreResult::kRefusedTabsOpen:
    default:
      return l10n_util::GetStringUTF16(IDS_PARRA_ENVIRONMENT_SWITCH_FAILED);
  }
}

}  // namespace parra
