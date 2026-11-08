// Copyright 2026 The Parra Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

import {assertEquals, assertFalse, assertTrue} from 'chrome://webui-test/chai_assert.js';
import {parseHostMappings} from 'chrome://parra/host_mapping.js';

suite('ParraHostMappingTest', () => {
  test('StandardHostsCompilesIPv4AndIPv6', () => {
    const result = parseHostMappings(
        '# comment\n203.0.113.10 one.test two.test\n2001:db8::10 six.test',
        'standardHosts');
    assertEquals(0, result.diagnostics.filter((item) => item.severity === 'error').length);
    assertEquals(
        'map one.test 203.0.113.10, map two.test 203.0.113.10, map six.test [2001:db8::10]',
        result.compiledRules);
  });

  test('WhistleSubsetAndUnsupportedProxyRules', () => {
    const accepted = parseHostMappings(
        '*.example.test:443 127.0.0.1:8443', 'whistleHost');
    assertEquals('map *.example.test:443 127.0.0.1:8443', accepted.compiledRules);
    assertEquals(0, accepted.diagnostics.length);

    const ipv6 = parseHostMappings(
      'api.example.test [2001:db8::10]:8443', 'whistleHost');
    assertEquals('map api.example.test [2001:db8::10]:8443', ipv6.compiledRules);

    const rejected = parseHostMappings(
        '# rule\nhttps://example.test/path 127.0.0.1', 'whistleHost');
    assertEquals('', rejected.compiledRules);
    assertTrue(rejected.diagnostics.some((item) =>
      item.line === 2 && item.severity === 'error' &&
      item.message.includes('路径')));
  });

  test('FirstConflictingMappingWinsWithLineDiagnostic', () => {
    const result = parseHostMappings(
        '203.0.113.10 same.test\n203.0.113.11 same.test', 'standardHosts');
    assertEquals('map same.test 203.0.113.10', result.compiledRules);
    assertEquals(1, result.diagnostics.length);
    assertEquals(2, result.diagnostics[0]?.line);
    assertEquals('warning', result.diagnostics[0]?.severity);
  });

  test('ChromiumMapSyntaxIsNotReformatted', () => {
    const source = 'MAP old.test 127.0.0.1, EXCLUDE localhost';
    const result = parseHostMappings(source, 'chromiumMap');
    assertEquals(source, result.compiledRules);
    assertEquals(0, result.diagnostics.length);
  });

  test('MalformedAddressAndUnsupportedModeAreBlocking', () => {
    const address = parseHostMappings('999.1.1.1 bad.test', 'standardHosts');
    assertEquals('', address.compiledRules);
    assertTrue(address.diagnostics.some((item) => item.line === 1));

    const unknown = parseHostMappings('map old.test 127.0.0.1', 'unsupported');
    assertEquals('', unknown.compiledRules);
    assertFalse(unknown.diagnostics.length === 0);
  });
});
