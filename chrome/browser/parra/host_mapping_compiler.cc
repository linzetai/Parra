// Copyright 2026 The Parra Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chrome/browser/parra/host_mapping_compiler.h"

#include <algorithm>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "base/strings/string_split.h"
#include "base/strings/string_util.h"
#include "net/base/host_mapping_rules.h"
#include "net/base/ip_address.h"
#include "net/base/url_util.h"

namespace parra {
namespace {

void AddError(std::vector<HostMappingDiagnostic>* diagnostics,
              size_t line,
              std::string message) {
  diagnostics->push_back(
      {line, HostMappingDiagnosticSeverity::kError, std::move(message)});
}

void AddWarning(std::vector<HostMappingDiagnostic>* diagnostics,
                size_t line,
                std::string message) {
  diagnostics->push_back(
      {line, HostMappingDiagnosticSeverity::kWarning, std::move(message)});
}

bool HasErrors(const std::vector<HostMappingDiagnostic>& diagnostics) {
  for (const auto& diagnostic : diagnostics) {
    if (diagnostic.severity == HostMappingDiagnosticSeverity::kError) {
      return true;
    }
  }
  return false;
}

bool IsIpLiteral(std::string_view value) {
  net::IPAddress address;
  return address.AssignFromIPLiteral(value);
}

bool IsValidHostname(std::string_view hostname, bool allow_wildcard) {
  if (allow_wildcard && hostname.starts_with("*.")) {
    hostname.remove_prefix(2);
  }
  if (hostname.empty() || hostname.size() > 253 || hostname.front() == '.' ||
      hostname.back() == '.') {
    return false;
  }
  for (std::string_view label : base::SplitStringPiece(
           hostname, ".", base::KEEP_WHITESPACE, base::SPLIT_WANT_ALL)) {
    if (label.empty() || label.size() > 63 || label.front() == '-' ||
        label.back() == '-') {
      return false;
    }
    for (char c : label) {
      if (!(base::IsAsciiAlpha(c) || base::IsAsciiDigit(c) || c == '-')) {
        return false;
      }
    }
  }
  return true;
}

bool ParseAddressAndPort(std::string_view token,
                         std::string* replacement,
                         std::string* error) {
  std::string host;
  int port = -1;
  net::IPAddress address;
  const size_t colon_count =
      static_cast<size_t>(std::count(token.begin(), token.end(), ':'));
  if (token.starts_with("[") || colon_count == 1) {
    if (!net::ParseHostAndPort(token, &host, &port)) {
      *error = "invalid IP address or port '" + std::string(token) + "'";
      return false;
    }
    if (port == 0 || port > 65535) {
      *error = "port must be in the range 1..65535";
      return false;
    }
  } else {
    host = std::string(token);
  }

  if (!address.AssignFromIPLiteral(host)) {
    *error = "replacement must be an IPv4 or IPv6 literal";
    return false;
  }

  *replacement = address.ToString();
  if (address.IsIPv6()) {
    *replacement = "[" + *replacement + "]";
  }
  if (port != -1) {
    *replacement += ":" + std::to_string(port);
  }
  return true;
}

size_t LineForOffset(std::string_view source, size_t offset) {
  size_t line = 1;
  for (size_t i = 0; i < offset && i < source.size(); ++i) {
    if (source[i] == '\n') {
      ++line;
    }
  }
  return line;
}

bool CompileChromiumRules(std::string_view source,
                          std::string* compiled_rules,
                          std::vector<HostMappingDiagnostic>* diagnostics) {
  net::HostMappingRules probe;
  std::vector<std::string> compiled;
  size_t start = 0;
  while (start <= source.size()) {
    const size_t comma = source.find(',', start);
    const size_t end = comma == std::string_view::npos ? source.size() : comma;
    const std::string_view raw_rule = source.substr(start, end - start);
    const std::string trimmed(
        base::TrimWhitespaceASCII(raw_rule, base::TRIM_ALL));
    if (trimmed.empty()) {
      if (comma == std::string_view::npos) {
        break;
      }
      start = comma + 1;
      continue;
    }
    if (!probe.AddRuleFromString(trimmed)) {
      AddError(diagnostics, LineForOffset(source, start),
               "invalid Chromium MAP/EXCLUDE rule: '" + trimmed + "'");
    } else {
      compiled.push_back(trimmed);
    }
    if (comma == std::string_view::npos) {
      break;
    }
    start = comma + 1;
  }
  if (HasErrors(*diagnostics)) {
    return false;
  }
  for (const std::string& rule : compiled) {
    if (!compiled_rules->empty()) {
      compiled_rules->append(", ");
    }
    compiled_rules->append(rule);
  }
  return true;
}

bool CompileHostsLines(HostMappingFormat format,
                       std::string_view source,
                       std::string* compiled_rules,
                       std::vector<HostMappingDiagnostic>* diagnostics) {
  const bool whistle = format == HostMappingFormat::kWhistleHost;
  net::HostMappingRules network_probe;
  std::vector<std::string> rules;
  std::vector<std::string> first_rule_for_pattern;
  size_t line_number = 0;
  for (std::string_view raw_line : base::SplitStringPiece(
           source, "\n", base::KEEP_WHITESPACE, base::SPLIT_WANT_ALL)) {
    ++line_number;
    if (!raw_line.empty() && raw_line.back() == '\r') {
      raw_line.remove_suffix(1);
    }
    const size_t comment = raw_line.find('#');
    if (comment != std::string_view::npos) {
      raw_line = raw_line.substr(0, comment);
    }
    const std::vector<std::string_view> fields = base::SplitStringPiece(
        raw_line, " \t", base::TRIM_WHITESPACE, base::SPLIT_WANT_NONEMPTY);
    if (fields.empty()) {
      continue;
    }

    std::string pattern;
    std::string replacement;
    std::string error;
    if (whistle) {
      if (fields.size() != 2) {
        AddError(
            diagnostics, line_number,
            "Whistle Host subset expects exactly: hostname [port] IP[:port]; "
            "paths, filters, regexes and proxy rules are unsupported");
        continue;
      }
      std::string host = std::string(fields[0]);
      if (host.find("://") != std::string::npos ||
          host.find_first_of("/?^$()[]~") != std::string::npos) {
        AddError(diagnostics, line_number,
                 "Unsupported Whistle Host syntax: URL paths, regexes, filters "
                 "and proxy rules are not supported");
        continue;
      }
      int match_port = -1;
      if (host.find(':') != std::string::npos) {
        if (!net::ParseHostAndPort(host, &host, &match_port) ||
            match_port <= 0 || match_port > 65535) {
          AddError(diagnostics, line_number,
                   "invalid hostname or match port '" + std::string(fields[0]) +
                       "'");
          continue;
        }
      }
      if (!IsValidHostname(host, /*allow_wildcard=*/true)) {
        AddError(
            diagnostics, line_number,
            "expected a hostname or leading '*.' wildcard, got '" + host + "'");
        continue;
      }
      pattern = host;
      if (match_port != -1) {
        pattern += ":" + std::to_string(match_port);
      }
      if (!ParseAddressAndPort(fields[1], &replacement, &error)) {
        AddError(diagnostics, line_number, std::move(error));
        continue;
      }
    } else {
      if (fields.size() < 2) {
        AddError(diagnostics, line_number,
                 "Hosts line requires an IP address followed by at least one "
                 "hostname");
        continue;
      }
      if (!IsIpLiteral(fields[0])) {
        AddError(diagnostics, line_number,
                 "first field must be a valid IPv4 or IPv6 address");
        continue;
      }
      net::IPAddress parsed_address;
      if (!parsed_address.AssignFromIPLiteral(fields[0])) {
        AddError(diagnostics, line_number,
                 "Hosts address must be an IP literal without a port");
        continue;
      }
      std::string address = parsed_address.ToString();
      if (parsed_address.IsIPv6()) {
        address = "[" + address + "]";
      }
      for (std::string_view hostname : fields) {
        if (hostname == fields[0]) {
          continue;
        }
        if (!IsValidHostname(hostname, /*allow_wildcard=*/false)) {
          AddError(diagnostics, line_number,
                   "invalid Hosts hostname '" + std::string(hostname) + "'");
          continue;
        }
        pattern = std::string(hostname);
        std::string rule = "map " + pattern + " " + address;
        if (!network_probe.AddRuleFromString(rule)) {
          AddError(
              diagnostics, line_number,
              "compiled mapping is not supported by the Chromium resolver");
          continue;
        }
        const std::string pattern_key = base::ToLowerASCII(pattern);
        auto previous = std::find(first_rule_for_pattern.begin(),
                                  first_rule_for_pattern.end(), pattern_key);
        if (previous != first_rule_for_pattern.end()) {
          const size_t prior_index = static_cast<size_t>(
              std::distance(first_rule_for_pattern.begin(), previous));
          if (rules[prior_index] != rule) {
            AddWarning(
                diagnostics, line_number,
                "conflicting duplicate; first matching Hosts mapping wins");
          } else {
            AddWarning(diagnostics, line_number,
                       "duplicate mapping is ignored");
          }
          continue;
        }
        first_rule_for_pattern.push_back(pattern_key);
        rules.push_back(std::move(rule));
      }
      continue;
    }

    std::string rule = "map " + pattern + " " + replacement;
    if (!network_probe.AddRuleFromString(rule)) {
      AddError(diagnostics, line_number,
               "compiled mapping is not supported by the Chromium resolver");
      continue;
    }
    const std::string pattern_key = base::ToLowerASCII(pattern);
    auto previous = std::find(first_rule_for_pattern.begin(),
                              first_rule_for_pattern.end(), pattern_key);
    if (previous != first_rule_for_pattern.end()) {
      const size_t prior_index = static_cast<size_t>(
          std::distance(first_rule_for_pattern.begin(), previous));
      if (rules[prior_index] != rule) {
        AddWarning(diagnostics, line_number,
                   "conflicting duplicate; first matching Host mapping wins");
      } else {
        AddWarning(diagnostics, line_number, "duplicate mapping is ignored");
      }
      continue;
    }
    first_rule_for_pattern.push_back(pattern_key);
    rules.push_back(std::move(rule));
  }

  if (HasErrors(*diagnostics)) {
    return false;
  }
  for (const std::string& rule : rules) {
    if (!compiled_rules->empty()) {
      compiled_rules->append(", ");
    }
    compiled_rules->append(rule);
  }
  return true;
}

}  // namespace

const char* HostMappingFormatToString(HostMappingFormat format) {
  switch (format) {
    case HostMappingFormat::kStandardHosts:
      return "standard_hosts";
    case HostMappingFormat::kWhistleHost:
      return "whistle_host";
    case HostMappingFormat::kChromiumMap:
      return "chromium_map";
    case HostMappingFormat::kUnsupported:
      return "unsupported";
  }
  return "unsupported";
}

bool HostMappingFormatFromString(std::string_view value,
                                 HostMappingFormat* format) {
  if (value == "standard_hosts") {
    *format = HostMappingFormat::kStandardHosts;
    return true;
  }
  if (value == "whistle_host") {
    *format = HostMappingFormat::kWhistleHost;
    return true;
  }
  if (value == "chromium_map") {
    *format = HostMappingFormat::kChromiumMap;
    return true;
  }
  return false;
}

bool CompileHostMappings(HostMappingFormat format,
                         std::string_view source,
                         std::string* compiled_rules,
                         std::vector<HostMappingDiagnostic>* diagnostics) {
  compiled_rules->clear();
  diagnostics->clear();
  if (format == HostMappingFormat::kUnsupported) {
    AddError(diagnostics, 1, "unsupported host-mapping format");
    return false;
  }
  if (source.empty()) {
    return true;
  }

  switch (format) {
    case HostMappingFormat::kChromiumMap:
      return CompileChromiumRules(source, compiled_rules, diagnostics);
    case HostMappingFormat::kStandardHosts:
    case HostMappingFormat::kWhistleHost:
      return CompileHostsLines(format, source, compiled_rules, diagnostics);
    case HostMappingFormat::kUnsupported:
      return false;
  }
  AddError(diagnostics, 1, "unsupported host-mapping format");
  return false;
}

}  // namespace parra
