// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "services/network/test/test_network_context.h"

#include <utility>

namespace network {

void TestNetworkContext::SetHostMappingRules(
    const std::string& rules,
    uint64_t revision,
    SetHostMappingRulesCallback callback) {
  std::move(callback).Run(false,
                          "TestNetworkContext does not apply host rules",
                          revision, 0,
                          mojom::HostOverridePolicy::kUnknown,
                          "test stub has no proxy policy");
}

}  // namespace network
