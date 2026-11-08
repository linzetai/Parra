// Copyright 2026 The Parra Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CHROME_BROWSER_PARRA_HOST_MAPPING_COMPILER_H_
#define CHROME_BROWSER_PARRA_HOST_MAPPING_COMPILER_H_

#include <string>
#include <string_view>
#include <vector>

#include "chrome/browser/parra/environment.h"

namespace parra {

enum class HostMappingDiagnosticSeverity { kWarning, kError };

struct HostMappingDiagnostic {
  size_t line = 0;
  HostMappingDiagnosticSeverity severity =
      HostMappingDiagnosticSeverity::kError;
  std::string message;
};

// Parses and validates the selected source format and, only on full success,
// emits Chromium HostMappingRules syntax. `source` is never modified.
bool CompileHostMappings(HostMappingFormat format,
                         std::string_view source,
                         std::string* compiled_rules,
                         std::vector<HostMappingDiagnostic>* diagnostics);

const char* HostMappingFormatToString(HostMappingFormat format);
bool HostMappingFormatFromString(std::string_view value,
                                 HostMappingFormat* format);

}  // namespace parra

#endif  // CHROME_BROWSER_PARRA_HOST_MAPPING_COMPILER_H_
