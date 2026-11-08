// Copyright 2026 The Parra Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CHROME_BROWSER_UI_WEBUI_PARRA_PARRA_BRAND_ICON_H_
#define CHROME_BROWSER_UI_WEBUI_PARRA_PARRA_BRAND_ICON_H_

#include "base/memory/ref_counted_memory.h"
#include "base/memory/scoped_refptr.h"
#include "chrome/grit/chrome_unscaled_resources.h"
#include "ui/base/resource/resource_bundle.h"
#include "ui/base/resource/resource_scale_factor.h"

// WebUI favicons are requested in 16 DIP at the current display scale. Each
// PNG is rendered directly from the checked-in Parra SVG at its target size.
inline scoped_refptr<base::RefCountedMemory> GetParraFaviconBytes(
    ui::ResourceScaleFactor scale_factor) {
  const float scale = ui::GetScaleForResourceScaleFactor(scale_factor);
  const int id = scale < 1.125f   ? IDR_PARRA_FAVICON_16
                 : scale < 1.375f ? IDR_PARRA_FAVICON_20
                 : scale < 1.75f  ? IDR_PARRA_FAVICON_24
                 : scale < 2.5f   ? IDR_PARRA_FAVICON_32
                                  : IDR_PARRA_FAVICON_48;
  return ui::ResourceBundle::GetSharedInstance().LoadDataResourceBytes(id);
}

#endif  // CHROME_BROWSER_UI_WEBUI_PARRA_PARRA_BRAND_ICON_H_
