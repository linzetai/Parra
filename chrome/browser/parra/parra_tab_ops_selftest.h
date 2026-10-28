// Copyright 2026 The Parra Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CHROME_BROWSER_PARRA_PARRA_TAB_OPS_SELFTEST_H_
#define CHROME_BROWSER_PARRA_PARRA_TAB_OPS_SELFTEST_H_

namespace parra {

// Phase 2.2B-2.1 self-test: verifies the two properties this phase exists for.
//
//   1. Close commands (Other Tabs / Tabs To Right / multi-select Close / All Tabs)
//      never destroy a WebContents belonging to another environment.
//   2. A new tab whose requested environment cannot be honoured is REFUSED rather
//      than created in the wrong StoragePartition, and the refusal is not hidden
//      by rewriting its environment tag.
//
// Requires the 3x3 layout from --parra-poc-visibility. Destroys tabs (that is
// what it measures), so it must not be combined with a run whoe tab count is being
// measured. Logs PARRA-TABOPS lines and then closes the browser.
//
// No-op unless --parra-poc-tabops-selftest is on the command line.
void MaybeRunTabOpsSelfTest();

}  // namespace parra

#endif  // CHROME_BROWSER_PARRA_PARRA_TAB_OPS_SELFTEST_H_
