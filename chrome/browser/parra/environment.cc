// Copyright 2026 The Parra Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chrome/browser/parra/environment.h"

namespace parra {

// Currently header-only: Environment is a plain value type with no behaviour of
// its own, and StoreResultToString() lives in environment_serialization.cc next
// to the rest of the diagnostic strings so they stay in one place.
//
// This translation unit exists so that the type has an addressable home in the
// build graph (a source_set listing only a header is a common source of "the
// target disappeared" confusion in GN, and it also gives future non-inline
// helpers somewhere to go without touching BUILD.gn again).

}  // namespace parra
