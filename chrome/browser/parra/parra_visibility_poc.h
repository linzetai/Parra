// Copyright 2026 The Parra Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CHROME_BROWSER_PARRA_PARRA_VISIBILITY_POC_H_
#define CHROME_BROWSER_PARRA_PARRA_VISIBILITY_POC_H_

namespace parra {

// Phase 2.1 visibility PoC.
//
// Creates three environments (Personal / TEST-A / TEST-B) with three tabs each
// in the last active window, all inside a single TabStripModel, then cycles the
// active environment so the behaviour can be observed without any UI.
//
// No-op unless --parra-poc-visibility was supplied.
//
// This deliberately does NOT implement EnvironmentRegistry, persistence, or UI.
// It exists to answer one question: can the active tab set be restricted to one
// environment while every tab stays in the native TabStripModel?
void MaybeRunVisibilityPoc();

}  // namespace parra

#endif  // CHROME_BROWSER_PARRA_PARRA_VISIBILITY_POC_H_
