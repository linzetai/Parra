// Copyright 2026 The Parra Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CHROME_BROWSER_PARRA_PARA_P31_SELFTEST_H_
#define CHROME_BROWSER_PARRA_PARA_P31_SELFTEST_H_

namespace parra {

// Phase 3 P3.1 safety self-test: verifies the fail-closed partition resolution and
// the OTR partition invariant.
//
// The properties it asserts are the ones this phase exists for:
//
//   1. An environment that is not defined RESOLVES TO A REFUSAL, not to the
//      default partition. A caller that proceeded on the old API would have
//      created a page in Personal while believing it was isolated.
//   2. An empty id (a stock Chromium navigation) still resolves successfully to
//      the default partition. Fail-closed must not break ordinary browsing.
//   3. The reserved `personal` environment still resolves, whether or not any
//      other environment is configured.
//   4. An OTR profile never produces a persistent environment partition config.
//   5. The structural partition -> environment lookup reports a deleted
//      environment's partition as that environment, not as Personal.
//
// Read-only with respect to the tab strip, but it creates a throwaway OTR profile,
// so it must run alone. No-op unless --parra-poc-p31-selftest is on the command
// line.
void MaybeRunP31SelfTest();

}  // namespace parra

#endif  // CHROME_BROWSER_PARRA_PARA_P31_SELFTEST_H_
