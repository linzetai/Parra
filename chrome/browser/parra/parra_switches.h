// Copyright 2026 The Parra Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CHROME_BROWSER_PARRA_PARA_SWITCHES_H_
#define CHROME_BROWSER_PARRA_PARA_SWITCHES_H_

#include "base/command_line.h"

// PoC-only command line switches for the Parra environment isolation
// prototype. These are intentionally NOT exposed to users and must not be
// relied upon by production code. See docs/parra/03-phase0-poc-plan.md.
namespace parra::switches {

// Enables the Parra environment PoC. Without this switch, Parra code paths are
// inert and the browser behaves like upstream Chromium.
//
// Format: a semicolon-separated list of environment definitions.
//   --parra-poc-environments="a|https://sheet.test/;b|https://sheet.test/"
//
// Each definition is `<environment_id>|<url>`. Each definition results in one
// initial tab. The environment id is used to derive a stable
// StoragePartitionConfig. Two environments with the same id share a partition.
extern const char kParraPocEnvironments[];

// Host mapping rules applied to the NetworkContext of each environment.
// One entry per environment, matching order with kParraPocEnvironments.
//
// Format: a semicolon-separated list of net::HostMappingRules rule strings.
//   --parra-poc-host-rules="map sheet.test 127.0.0.1:8443;map sheet.test 127.0.0.2:8444"
//
// An empty entry (`;;`) means "no remapping for that environment".
extern const char kParraPocHostRules[];

// Logs a one-line diagnostic to stderr for every navigation that enters or
// leaves an environment partition. Used for verification only.
extern const char kParraPocVerbose[];

// Phase 2.0 lifecycle PoC. Enables the tab-detach experiment that answers
// whether a tab can live outside TabStripModel without breaking Chromium's
// tab lifecycle.
//
// Format: a semicolon-separated list of `<environment_id>|<url>` definitions,
// exactly like kParraPocEnvironments, but the PoC also drives a switch cycle.
//   --parra-poc-lifecycle="a|https://sheet.test:8443/;b|https://sheet.test:8443/"
//
// When present, the browser starts with every environment's tabs, then runs
// `--parra-poc-lifecycle-cycles` rounds of a->b->a->b automatically so the
// behaviour can be observed without any UI or user interaction.
extern const char kParraPocLifecycle[];

// How many switch rounds the lifecycle PoC performs. Default 1. Set to 0 to
// disable automatic cycling and leave the tabs in their initial environment.
extern const char kParraPocLifecycleCycles[];

// Phase 2.1 visibility PoC. Creates three fixed environments (personal, test-a,
// test-b) with three tabs each inside a single TabStripModel, then cycles the
// active environment WITHOUT removing any tab from the model.
//
//   --parra-poc-visibility
//
// This is the plan A' experiment: page liveness comes from the tabs staying in
// the native model, and only the active tab changes.
extern const char kParraPocVisibility[];

// How many full cycle rounds the visibility PoC performs. Default 5. A round
// visits every environment once. 0 disables cycling.
extern const char kParraPocVisibilityCycles[];

// Populate the environment registry but do NOT create any startup tabs.
//
// Required for session-restore runs: the registry must exist so restored tabs
// can resolve their environment, but creating one tab per environment on top of
// the restored ones would inflate the tab count.
extern const char kParraPocNoInitialTabs[];

// Phase 2.2A model-layer self-test: runs T1..T11 of the index-mapper design
// against the live tab strip and logs PARRA-SELFTEST lines. Verification only.
//
//   --parra-poc-selftest
//
// Mutates the tab strip (inserts, moves and closes scratch tabs), so it must not
// be combined with a session-restore run that is being measured.
extern const char kParraPocSelfTest[];

// Phase 2.2B-2.1 tab-operation self-test: runs the close-command containment and
// the creation-invariant checks described in docs/parra/43 and 44, then exits.
//
//   --parra-poc-tabops-selftest
//
// Destroys tabs (that is the thing under test), so it must run alone. It needs the
// 3x3 layout from --parra-poc-visibility.
extern const char kParraPocTabOpsSelfTest[];

// Phase 3 P3.1 safety self-test: verifies the fail-closed partition resolution and
// the OTR partition invariant described in docs/parra/53 and 54.
//
//   --parra-poc-p31-selftest
//
// Read-only with respect to the tab strip: it asks the resolver questions and
// inspects the answers. It DOES construct a throwaway OTR profile to prove that an
// incognito environment partition is in-memory, so it must run alone. No tab is
// created, destroyed, moved or navigated.
//
// Needs the 3x3 layout from --parra-poc-visibility so the resolver can be asked
// about environments that really exist and about ones that do not.
extern const char kParraPocP31SelfTest[];

// Phase 3 P3.2 self-test: verifies the Profile-scoped EnvironmentStore -- the data
// model, CRUD, persistence states, enable/disable semantics and the format rules
// described in docs/parra/56.
//
//   --parra-poc-p32-selftest
//
// Creates and removes definitions in the profile's prefs, so it must run against a
// throwaway profile and alone. It restores the ids it owns before exiting.
extern const char kParraPocP32SelfTest[];

// Auxiliary P3.2 modes used to prove persistence ACROSS PROCESSES.
//
//   --parra-poc-p32-seed="<id>#<host rules>"
//     Creates (or updates) one definition, flushes it to disk, logs SEED-DONE and
//     exits. A single-process test could only prove that the in-memory map still
//     holds what was just written; a second process is what proves the bytes
//     reached the disk.
//
//   --parra-poc-p32-verify="<id>,<id>,..."
//     Reads the listed ids back, asserts each exists, logs the stored order, the
//     store's address, VERIFY-DONE, and exits.
//
// Both need a throwaway profile and mutate it.
extern const char kParraPocP32Seed[];
extern const char kParraPocP32Verify[];

// Opens one tab inside a stored environment and leaves the browser running:
//
//   --parra-poc-p32-open="<id>#<url>"
//
// Used by the network half of the isolation proof (docs/parra/57 §3): the page
// must be served according to the STORED definition, not a command-line one.
extern const char kParraPocP32Open[];

// Sets the `enabled` flag of one stored environment and exits:
//
//   --parra-poc-p32-set-enabled="<id>#<0|1>"
//
// Needed because `enabled` is a per-Profile preference: proving that disabling in
// profile A leaves profile B's identically-named environment working requires
// two processes, each flipping its own profile, rather than one process holding
// both Stores in memory (docs/parra/60 §4).
extern const char kParraPocP32SetEnabled[];

}  // namespace parra::switches

#endif  // CHROME_BROWSER_PARRA_PARA_SWITCHES_H_
