// Copyright 2016 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// Brand-specific types and constants for Chromium.

#ifndef CHROME_INSTALL_STATIC_CHROMIUM_INSTALL_MODES_H_
#define CHROME_INSTALL_STATIC_CHROMIUM_INSTALL_MODES_H_

#include <array>

#include "chrome/app/chrome_dll_resource.h"
#include "chrome/common/chrome_icon_resources_win.h"
#include "chrome/install_static/install_constants.h"

namespace install_static {

// The brand-specific company name to be included as a component of the install
// and user data directory paths. May be empty if no such dir is to be used.
inline constexpr wchar_t kCompanyPathName[] = L"";

// The brand-specific product name to be included as a component of the install
// and user data directory paths.
inline constexpr wchar_t kProductPathName[] = L"Parra";

// The brand-specific safe browsing client name.
inline constexpr char kSafeBrowsingName[] = "chromium";

// Note: This list of indices must be kept in sync with the brand-specific
// resource strings in chrome/installer/util/prebuild/create_string_rc.
enum InstallConstantIndex {
  CHROMIUM_INDEX,
  NUM_INSTALL_MODES,
};

inline constexpr auto kOldTracingServiceIids = std::to_array<IID>({
    // Replaced in 2026-09. Delete after 2028-09.
    // {A3FD580A-FFD4-4075-9174-75D0B199D3CB}
    {0xa3fd580a,
     0xffd4,
     0x4075,
     {0x91, 0x74, 0x75, 0xd0, 0xb1, 0x99, 0xd3, 0xcb}},
});

inline constexpr auto kInstallModes = std::to_array<InstallConstants>({
    // The primary (and only) install mode for Chromium.
    {
        .size = sizeof(InstallConstants),
        .index = CHROMIUM_INDEX,  // The one and only mode for Chromium.
        .install_switch =
            "",  // No install switch for the primary install mode.
        .install_suffix =
            L"",  // Empty install_suffix for the primary install mode.
        .logo_suffix = L"",  // No logo suffix for the primary install mode.
        .app_guid =
            L"",  // Empty app_guid since no integration with Google Update.
        .base_app_name = L"Parra",              // A distinct base_app_name.
        .base_app_id = L"Parra",                // A distinct base_app_id.
        .browser_prog_id_prefix = L"ParraHTM",  // Browser ProgID prefix.
        .browser_prog_id_description =
            L"Parra HTML Document",         // Browser ProgID description.
        .direct_launch_url_scheme = "",     // No preview-only custom scheme.
        .pdf_prog_id_prefix = L"ParraPDF",  // PDF ProgID prefix.
        .pdf_prog_id_description =
            L"Parra PDF Document",  // PDF ProgID description.
        .active_setup_guid =
            L"{EAFC61DD-3126-4D05-8271-6773DE47BA9F}",  // Active Setup
                                                        // GUID.
        .toast_activator_clsid = {0xb8af08f9,
                                  0x0f69,
                                  0x44b3,
                                  {0x9a, 0x37, 0x23, 0x4b, 0xae, 0x02, 0x65,
                                   0x84}},
        .elevator_clsid = {0x8035061d,
                           0xc2df,
                           0x4e80,
                           {0xa1, 0x54, 0x10, 0x6e, 0x00, 0x49, 0x74, 0x96}},
        .elevator_iid = {0xfe21e3b0,
                         0x6ba8,
                         0x49be,
                         {0x85, 0x68, 0x5b, 0xc2, 0x90, 0x93, 0x27, 0xbf}},
        .old_elevator_iids = {},
        .tracing_service_clsid = {0x2a1398da,
                                  0x92c6,
                                  0x47e7,
                                  {0x83, 0xa1, 0x2c, 0x04, 0x12, 0x03, 0xdc,
                                   0x0b}},
        .tracing_service_iid = {0x40204626,
                                0x09ee,
                                0x4d51,
                                {0xb4, 0x17, 0xec, 0x43, 0xff, 0xfe, 0x6a,
                                 0x12}},
        .old_tracing_service_iids = {},
        .default_channel_name =
            L"",  // Empty default channel name since no update integration.
        .channel_strategy = ChannelStrategy::UNSUPPORTED,
        .supports_system_level = true,  // Supports system-level installs.
        .supports_set_as_default_browser =
            true,  // Supports in-product set as default browser UX.
        .app_icon_resource_index =
            icon_resources::kApplicationIndex,  // App icon resource index.
        .app_icon_resource_id = IDR_MAINFRAME,  // App icon resource id.
        .html_doc_icon_resource_index =
            icon_resources::kHtmlDocIndex,  // HTML doc icon resource index.
        .pdf_doc_icon_resource_index =
            icon_resources::kPDFDocIndex,  // PDF doc icon resource index.
        .sandbox_sid_prefix =
            L"S-1-15-2-1968620702-1763429242-2671861020-961533473-"
            L"3397182077-2980720609-2112389876-",
    },
});

}  // namespace install_static

#endif  // CHROME_INSTALL_STATIC_CHROMIUM_INSTALL_MODES_H_
