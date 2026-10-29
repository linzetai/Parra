// Copyright 2026 The Parra Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CHROME_BROWSER_PARRA_PARA_P32_SELFTEST_H_
#define CHROME_BROWSER_PARRA_PARA_P32_SELFTEST_H_

namespace parra {

// Phase 3 P3.2 self-test: verifies the Profile-scoped EnvironmentStore.
//
// It asserts the properties this phase exists for:
//
//   A. Data model invariants: id immutability, id == partition_name, Personal
//      always present, definitions independent of whether any tab exists.
//   B. CRUD: create / find / list / update / set-enabled, duplicate rejection,
//      invalid host mapping rejection, reserved-id rejection, explicit error
//      types instead of a bare bool.
//   C. Persistence: a create survives a restart, an update survives a restart,
//      stored order is preserved, and PersistState never claims kPersisted
//      without a CommitPendingWrite() reply.
//   D. Fail-closed: unknown and DISABLED environments cannot be used as a new tab
//      target, and neither may silently fall back to Personal.
//   E. Format: unknown fields and a too-new schemaVersion are handled per design,
//      and a corrupt blob does not move existing environment pages to Personal.
//   F. Store identity: two profiles hold different stores.
//
// The tab-strip is not modified. It creates and removes definitions in the
// profile's prefs, so it must run alone and against a throwaway profile.
//
// --parra-poc-p32-selftest
void MaybeRunP32SelfTest();

// Opens one tab inside a STORED environment and leaves the browser running, so a
// CDP script can verify which backend actually served the page and what cookies
// it can see.
//
//   --parra-poc-p32-open="<id>#<url>"
//
// Needed because the network half of the isolation proof must not be driven by a
// command-line environment definition: if it were, the run would prove that the
// SWITCH works rather than that the STORED definition works.
void MaybeRunP32Open();

// Opens a REAL second browser window and parks it on the named environment, then
// navigates it to the given URL:
//
//   --parra-poc-open-window="<id>#<url>"
//
// Uses chrome::NewWindow() -- the Ctrl+N entry point -- because --new-window
// against a running instance is forwarded to that process and does not reliably
// yield a second session window. The resulting window is a genuine Browser with
// its own TabStripModel, BrowserView and WindowEnvironmentState.
//
// Enables the multi-window restore proof in docs/parra/65: two windows on two
// different environments, both persisted, both restored independently.
void MaybeRunP32OpenWindow();

// Window lifetime stress: opens and closes N windows, asserting the live
// WindowEnvironmentState count returns to its baseline each cycle.
//
//   --parra-poc-window-stress=<count>
//
// Catches leaked window state, which is otherwise invisible until its raw window
// pointer is used after the window is gone. See docs/parra/64.
void MaybeRunP32WindowStress();

// P3.3 T1: move a tab to a second window and leave both open, so an outer script
// can close the browser and verify that a restart puts the tab back in the TARGET
// window with its original environment. No-op unless --parra-poc-tab-move=<env>.
void MaybeRunTabMove();

// P3.3 T2: create a tab that is untagged at insertion time, then tag it, and leave
// the browser open. No-op unless --parra-poc-late-tag=<env>.
void MaybeRunLateTag();

}  // namespace parra

#endif  // CHROME_BROWSER_PARRA_PARRA_P32_SELFTEST_H_
