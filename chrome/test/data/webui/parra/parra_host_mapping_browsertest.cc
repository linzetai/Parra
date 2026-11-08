// Copyright 2026 The Parra Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chrome/test/base/web_ui_mocha_browser_test.h"
#include "content/public/test/browser_test.h"

class ParraHostMappingWebUIBrowserTest : public WebUIMochaBrowserTest {
 protected:
  ParraHostMappingWebUIBrowserTest() { set_test_loader_host("parra"); }
};

IN_PROC_BROWSER_TEST_F(ParraHostMappingWebUIBrowserTest, ParserAndCompiler) {
  RunTest("parra/host_mapping_test.js", "mocha.run()");
}
