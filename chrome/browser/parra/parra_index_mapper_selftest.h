// Copyright 2026 The Parra Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CHROME_BROWSER_PARRA_PARRA_INDEX_MAPPER_SELFTEST_H_
#define CHROME_BROWSER_PARRA_PARRA_INDEX_MAPPER_SELFTEST_H_

namespace parra {

// Phase 2.2A model-layer validation.
//
// Runs T1..T11 from docs/parra/25-environment-index-mapper-design.md against the
// live TabStripModel of the first browser window and logs PARRA-SELFTEST lines.
// No-op unless --parra-poc-selftest is on the command line.
//
// It only observes and mutates the model through the public TabStripModel API
// plus EnvironmentIndexMapper; it never detaches, destroys or recreates a
// WebContents and never touches StoragePartition.
void MaybeRunIndexMapperSelfTest();

}  // namespace parra

#endif  // CHROME_BROWSER_PARRA_PARRA_INDEX_MAPPER_SELFTEST_H_
