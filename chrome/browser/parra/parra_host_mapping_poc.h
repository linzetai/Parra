// Copyright 2026 The Parra Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CHROME_BROWSER_PARRA_PARRA_HOST_MAPPING_POC_H_
#define CHROME_BROWSER_PARRA_PARRA_HOST_MAPPING_POC_H_

namespace parra {

// P3.4 PoC-1. No-op unless --parra-poc-host-mapping-update was supplied.
//
// Verifies, on a LIVE browser, that a NetworkContext can take new host mapping
// rules without being destroyed, for both starting states:
//   * created with rules      (a named environment)
//   * created WITHOUT rules   (Personal, i.e. no mapping layer exists)
// and that the change is observable in the bytes a real request receives.
//
// Evidence is always a response body, never "the call returned".
void MaybeRunHostMappingPoc();

// P3.4 PoC-2 scenario E. No-op unless --parra-poc-page-state was supplied.
//
// Repoints a LIVE environment's host mapping twice while its page stays open,
// and logs a timestamped line per change. It makes no assertion about the page:
// an external CDP script (parra-poc/devtools/verify-poc2-page-state.js) samples
// the page before, between and after the changes.
//
// WHY THE ASSERTIONS LIVE OUTSIDE: "the page was not reloaded" and "the page's
// later requests reach the new backend" are different claims. A runner that
// both changes the rules and grades the outcome can satisfy itself with the
// easier of the two.
void MaybeRunPageStatePoc();

}  // namespace parra

#endif  // CHROME_BROWSER_PARRA_PARRA_HOST_MAPPING_POC_H_
