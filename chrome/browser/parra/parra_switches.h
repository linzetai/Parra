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

}  // namespace parra::switches

#endif  // CHROME_BROWSER_PARRA_PARA_SWITCHES_H_
