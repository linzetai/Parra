// Copyright 2026 The Parra Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

import type {HostMappingFormat} from './parra_api.js';

export interface HostMappingDiagnostic {
  line: number;
  severity: 'error'|'warning';
  message: string;
}

export interface HostMappingParseResult {
  compiledRules: string;
  diagnostics: HostMappingDiagnostic[];
}

function isIpv4(value: string): boolean {
  const octets = value.split('.');
  return octets.length === 4 && octets.every((octet) =>
    /^(0|[1-9]\d{0,2})$/.test(octet) && Number(octet) <= 255);
}

function isIpv6(value: string): boolean {
  if (!value.includes(':') || value.includes(':::')) {
    return false;
  }
  let address = value;
  let ipv4TailGroups = 0;
  if (value.includes('.')) {
    const lastColon = value.lastIndexOf(':');
    const tail = value.slice(lastColon + 1);
    if (lastColon < 0 || !isIpv4(tail)) {
      return false;
    }
    address = value.slice(0, lastColon);
    ipv4TailGroups = 2;
  }
  if (/[^0-9a-f:]/i.test(address)) {
    return false;
  }
  const compressed = value.includes('::');
  if (compressed && value.indexOf('::') !== value.lastIndexOf('::')) {
    return false;
  }
  const groups = address.split(':').filter(Boolean);
  return groups.every((group) => group.length <= 4) &&
    (compressed ? groups.length + ipv4TailGroups < 8 :
                  groups.length + ipv4TailGroups === 8);
}

function canonicalIpv6(value: string): string {
  try {
    return new URL(`http://[${value}]/`).hostname.slice(1, -1);
  } catch {
    return value.toLowerCase();
  }
}

function isIp(value: string): boolean {
  return isIpv4(value) || isIpv6(value);
}

function isHostname(value: string, allowWildcard: boolean): boolean {
  let hostname = value;
  if (allowWildcard && hostname.startsWith('*.')) {
    hostname = hostname.slice(2);
  }
  if (!hostname || hostname.length > 253 || hostname.startsWith('.') ||
      hostname.endsWith('.')) {
    return false;
  }
  return hostname.split('.').every((label) => label.length > 0 && label.length <= 63 &&
    !label.startsWith('-') && !label.endsWith('-') &&
    /^[a-z\d-]+$/i.test(label));
}

function parseIpAndPort(value: string): string|null {
  let host = value;
  let port = '';
  if (value.startsWith('[')) {
    const match = /^\[([0-9a-f:]+)\](?::(\d+))?$/i.exec(value);
    if (!match) {
      return null;
    }
    host = match[1] ?? '';
    port = match[2] ?? '';
    if (!isIpv6(host)) {
      return null;
    }
    host = `[${canonicalIpv6(host)}]`;
  } else if (value.includes(':') && value.split(':').length === 2) {
    const index = value.lastIndexOf(':');
    host = value.slice(0, index);
    port = value.slice(index + 1);
  }
  if (port && (!/^\d+$/.test(port) || Number(port) < 1 || Number(port) > 65535)) {
    return null;
  }
  if (!value.startsWith('[') && !isIp(host)) {
    return null;
  }
  if (!value.startsWith('[') && isIpv6(host)) {
    host = `[${canonicalIpv6(host)}]`;
  }
  return host + (port ? `:${Number(port)}` : '');
}

function addDiagnostic(
  diagnostics: HostMappingDiagnostic[], line: number,
  severity: HostMappingDiagnostic['severity'], message: string,
): void {
  diagnostics.push({line, severity, message});
}

function parseChromium(text: string, diagnostics: HostMappingDiagnostic[]): string[] {
  const rules: string[] = [];
  let lineNumber = 0;
  for (const row of text.split(/\r?\n/)) {
    ++lineNumber;
    for (const part of row.split(',')) {
      const rule = part.trim();
      if (!rule) {
        continue;
      }
      const fields = rule.split(/\s+/);
      const command = (fields[0] ?? '').toLowerCase();
      if (command === 'map' && fields.length === 3) {
        rules.push(rule);
      } else if (command === 'exclude' && fields.length === 2) {
        rules.push(rule);
      } else {
        addDiagnostic(diagnostics, lineNumber, 'error',
          'Chromium 格式应为 MAP 主机 替换地址[:端口] 或 EXCLUDE 主机。');
      }
    }
  }
  return rules;
}

export function parseHostMappings(
  text: string, format: HostMappingFormat,
): HostMappingParseResult {
  const diagnostics: HostMappingDiagnostic[] = [];
  if (format === 'chromiumMap') {
    const rules = parseChromium(text, diagnostics);
    return {
      compiledRules: diagnostics.some((d) => d.severity === 'error') ? '' : rules.join(', '),
      diagnostics,
    };
  }
  if (format === 'unsupported') {
    addDiagnostic(diagnostics, 1, 'error', '当前配置使用不支持的映射格式；为保护旧数据，不能保存。');
    return {compiledRules: '', diagnostics};
  }

  const whistle = format === 'whistleHost';
  const rules: string[] = [];
  const firstRuleByPattern = new Map<string, string>();
  const lines = text.split(/\r?\n/);
  for (let index = 0; index < lines.length; ++index) {
    const lineNumber = index + 1;
    const row = (lines[index] ?? '').split('#', 1)[0] ?? '';
    const fields = row.trim().split(/\s+/).filter(Boolean);
    if (fields.length === 0) {
      continue;
    }

    if (whistle) {
      if (fields.length !== 2) {
        addDiagnostic(diagnostics, lineNumber, 'error',
          'Whistle Host 子集只接受“主机名[:端口] IP[:端口]”；路径、正则、过滤器和代理规则不支持。');
        continue;
      }
      let pattern = fields[0] ?? '';
      if (pattern.includes('://') || pattern.includes('/') ||
          pattern.includes('?') || pattern.includes('^') || pattern.includes('$') ||
          pattern.includes('(') || pattern.includes(')') || pattern.includes('[') ||
          pattern.includes(']') || pattern.startsWith('~')) {
        addDiagnostic(diagnostics, lineNumber, 'error',
          'Whistle Host 子集不支持 URL 路径、正则、过滤器或代理规则。');
        continue;
      }
      let host = pattern;
      let matchPort = '';
      if (pattern.includes(':')) {
        const colon = pattern.lastIndexOf(':');
        host = pattern.slice(0, colon);
        matchPort = pattern.slice(colon + 1);
        if (!/^\d+$/.test(matchPort) || Number(matchPort) < 1 || Number(matchPort) > 65535) {
          addDiagnostic(diagnostics, lineNumber, 'error', '主机匹配端口必须是 1..65535 的整数。');
          continue;
        }
      }
      if (!isHostname(host, true)) {
        addDiagnostic(diagnostics, lineNumber, 'error', '主机名无效；仅允许普通主机名或前缀通配符 *.example.com。');
        continue;
      }
      const replacement = parseIpAndPort(fields[1] ?? '');
      if (!replacement) {
        addDiagnostic(diagnostics, lineNumber, 'error', '目标必须是有效 IPv4/IPv6 地址，可选端口范围 1..65535。');
        continue;
      }
      pattern = host + (matchPort ? `:${Number(matchPort)}` : '');
      const compiled = `map ${pattern} ${replacement}`;
      const patternKey = pattern.toLowerCase();
      const previous = firstRuleByPattern.get(patternKey);
      if (previous !== undefined) {
        addDiagnostic(diagnostics, lineNumber, 'warning', previous === compiled ?
          '重复映射已忽略。' : '存在冲突映射；按文件顺序优先使用第一条。');
        continue;
      }
      firstRuleByPattern.set(patternKey, compiled);
      rules.push(compiled);
      continue;
    }

    if (fields.length < 2) {
      addDiagnostic(diagnostics, lineNumber, 'error', 'Hosts 行需要 IP 地址以及至少一个主机名。');
      continue;
    }
    const address = fields[0] ?? '';
    if (!isIp(address)) {
      addDiagnostic(diagnostics, lineNumber, 'error', 'Hosts 行首字段必须是有效 IPv4 或 IPv6 地址。');
      continue;
    }
    const target = isIpv6(address) ? `[${address}]` : address;
    for (const host of fields.slice(1)) {
      if (!isHostname(host, false)) {
        addDiagnostic(diagnostics, lineNumber, 'error', `Hosts 主机名无效：“${host}”。`);
        continue;
      }
      const compiled = `map ${host} ${target}`;
      const patternKey = host.toLowerCase();
      const previous = firstRuleByPattern.get(patternKey);
      if (previous !== undefined) {
        addDiagnostic(diagnostics, lineNumber, 'warning', previous === compiled ?
          '重复映射已忽略。' : '存在冲突映射；按文件顺序优先使用第一条。');
        continue;
      }
      firstRuleByPattern.set(patternKey, compiled);
      rules.push(compiled);
    }
  }

  const hasErrors = diagnostics.some((diagnostic) => diagnostic.severity === 'error');
  return {compiledRules: hasErrors ? '' : rules.join(', '), diagnostics};
}
