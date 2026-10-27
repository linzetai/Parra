// Copyright 2026 The Parra Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CHROME_BROWSER_PARRA_PARA_STARTUP_H_
#define CHROME_BROWSER_PARRA_PARA_STARTUP_H_

namespace parra {

// PoC-only: parses --parra-poc-environments / --parra-poc-host-rules and
// populates the process-wide EnvironmentRegistry.
//
// MUST be called before any StoragePartition/NetworkContext is created,
// because ProfileNetworkContextService consults the registry while building
// NetworkContextParams. ChromeBrowserMainParts calls it from
// PreCreateThreads(); calling it later would leave already-created network
// contexts without host mapping rules.
//
// Returns true if the PoC is enabled. Safe to call repeatedly; it is
// idempotent because it only reads the command line.
bool InitializePocEnvironmentRegistry();

// PoC-only: opens one tab per configured environment, in the last active
// browser window, each tab bound to its environment's StoragePartition.
//
// Called once from ChromeBrowserMainParts::PostBrowserStart(). Does nothing
// unless --parra-poc-environments was supplied and parsed successfully, so the
// browser behaves exactly like upstream Chromium otherwise.
//
// The first environment is opened in the currently active tab position by
// replacing the existing about:blank tab's contents; further environments get
// new foreground tabs. This keeps the PoC to zero new UI.
void MaybeOpenPocEnvironmentTabs();

}  // namespace parra

#endif  // CHROME_BROWSER_PARRA_PARA_STARTUP_H_
