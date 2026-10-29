// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "components/sessions/core/command_storage_features.h"

#include <optional>
#include <string>
#include <string_view>

#include "base/command_line.h"
#include "base/feature_list.h"
#include "base/logging.h"
#include "build/build_config.h"

namespace sessions {

BASE_FEATURE(kEncryptSessionStorage, base::FEATURE_ENABLED_BY_DEFAULT);

BASE_FEATURE_PARAM(std::string,
                   kEncryptSessionStorageStageParam,
                   &kEncryptSessionStorage,
                   "stage",
                   "write_both_read_only_clear");

using ::sessions::internal::kEncryptSessionStorageStageWriteBothReadOnlyClear;
using ::sessions::internal::
    kEncryptSessionStorageStageWriteBothReadPreferEncrypted;
using ::sessions::internal::
    kEncryptSessionStorageStageWriteEncryptedReadPreferEncrypted;

namespace {

// Parra: test-only override of the storage stage.
//
// Declared here as a literal rather than by including
// chrome/browser/parra/parra_switches.h, because //components/sessions must not
// depend on //chrome. The switch is documented in that header; the string is the
// shared contract and is asserted equal by the tests.
constexpr char kParraSessionReadBackendSwitch[] =
    "parra-poc-session-read-backend";

// Returns the overridden stage, or nullopt when the test switch is absent.
//
// WHY A SWITCH AND NOT JUST A FEATURE: session restore reads the cleartext copy
// by default but FALLS BACK to the encrypted copy when the cleartext read fails
// or yields no commands. A test that manipulates only the cleartext file can
// therefore be silently defeated -- an earlier fault-injection run masked every
// `parra.environment_id` in Default/Sessions and still restored tabs to their
// environments, because the encrypted copy answered instead. Pinning the stage
// makes "I corrupted the file the reader uses" and "the reader saw the
// corruption" the same statement.
//
// `clear` maps to kClearOnly: the encrypted backend is not written and the
// constructor DELETEs the stale Sessions_Encrypted directory, so no fallback to
// ciphertext is possible. `encrypted` maps to
// kWriteEncryptedReadPreferEncrypted for the symmetric case.
std::optional<EncryptSessionStorageStage> GetParraOverriddenStage() {
  const base::CommandLine* command_line =
      base::CommandLine::ForCurrentProcess();
  if (!command_line ||
      !command_line->HasSwitch(kParraSessionReadBackendSwitch)) {
    return std::nullopt;
  }
  const std::string value =
      command_line->GetSwitchValueASCII(kParraSessionReadBackendSwitch);
  if (value == "clear") {
    return EncryptSessionStorageStage::kClearOnly;
  }
  if (value == "encrypted") {
    return EncryptSessionStorageStage::kWriteEncryptedReadPreferEncrypted;
  }
  LOG(ERROR) << "parra-session: unknown " << kParraSessionReadBackendSwitch
             << " value '" << value << "' (expected clear|encrypted)";
  return std::nullopt;
}

}  // namespace

EncryptSessionStorageStage GetEncryptSessionStorageStage() {
  // The test override wins over both the feature flag and its stage parameter:
  // a test must be able to pin the backend even when the feature is enabled by
  // default, which it is.
  if (std::optional<EncryptSessionStorageStage> overridden =
          GetParraOverriddenStage()) {
    return *overridden;
  }
  if (!base::FeatureList::IsEnabled(kEncryptSessionStorage)) {
    return EncryptSessionStorageStage::kClearOnly;
  }
  std::string param_str = kEncryptSessionStorageStageParam.Get();
  if (param_str.empty()) {
    return EncryptSessionStorageStage::kClearOnly;
  } else if (param_str == kEncryptSessionStorageStageWriteBothReadOnlyClear) {
    return EncryptSessionStorageStage::kWriteBothReadOnlyClear;
  } else if (param_str ==
             kEncryptSessionStorageStageWriteBothReadPreferEncrypted) {
    return EncryptSessionStorageStage::kWriteBothReadPreferEncrypted;
  } else if (param_str ==
             kEncryptSessionStorageStageWriteEncryptedReadPreferEncrypted) {
    return EncryptSessionStorageStage::kWriteEncryptedReadPreferEncrypted;
  } else {
    VLOG(1) << "Unknown EncryptSessionStorageStage: " << param_str;
    return EncryptSessionStorageStage::kClearOnly;
  }
}

}  // namespace sessions
