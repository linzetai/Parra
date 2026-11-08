// Copyright 2026 The Parra Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CHROME_BROWSER_UI_WEBUI_PARRA_PARRA_UI_H_
#define CHROME_BROWSER_UI_WEBUI_PARRA_PARRA_UI_H_

#include "chrome/browser/ui/webui/parra/parra.mojom.h"
#include "chrome/common/webui_url_constants.h"
#include "content/public/browser/webui_config.h"
#include "content/public/common/url_constants.h"
#include "mojo/public/cpp/bindings/pending_receiver.h"
#include "mojo/public/cpp/bindings/pending_remote.h"
#include "mojo/public/cpp/bindings/receiver.h"
#include "ui/webui/mojo_web_ui_controller.h"

class ParraPageHandler;
class ParraUI;

// chrome://parra's WebUIConfig.
//
// WHY DefaultWebUIConfig AND NOT DefaultInternalWebUIConfig
//
// DefaultInternalWebUIConfig (used by e.g. chrome://indigo-internals) marks a
// page as an INTERNAL DEBUGGING page. Those are disabled by default in official
// builds, and navigating to one redirects to chrome://debug-webuis-disabled/ --
// which is exactly what happened the first time this config was written with
// the internal base:
//
//   committed url: chrome://debug-webuis-disabled/?host=chrome://parra/
//
// Environment management is a product surface, not a diagnostics page, so it
// must not be gated behind the debug-WebUI switch. DefaultWebUIConfig gives the
// plain behaviour and still lets IsWebUIEnabled() narrow availability per
// profile.
class ParraUIConfig : public content::DefaultWebUIConfig<ParraUI> {
 public:
  ParraUIConfig()
      : DefaultWebUIConfig(content::kChromeUIScheme,
                           chrome::kChromeUIParraHost) {}

  // Enabled for regular profiles only. The environment definitions are
  // Profile-scoped, so the page would show the same list in an incognito window
  // while its writes went to the regular profile's prefs -- an edit that
  // outlives the window it was made in.
  bool IsWebUIEnabled(content::BrowserContext* browser_context) override;
};

// chrome://parra -- the environment management page.
//
// A STANDALONE WebUI, not an embedded section of another page.
//
// The page owns a whole task ("manage my environments"), it needs room for a
// list plus a detail pane, and it should be reachable by typing the url.
// Hosting it inside Settings would mean the environments code depends on the
// Settings WebUI's lifecycle and navigation model for no benefit.
//
// This class is deliberately thin: it owns the data source and one handler, and
// does no validation. Everything the page can ask for is delegated to
// EnvironmentStore through ParraPageHandler, so there is exactly one
// implementation of the rules.
class ParraUI : public ui::MojoWebUIController,
                public parra::mojom::PageHandlerFactory {
 public:
  explicit ParraUI(content::WebUI* web_ui);
  ~ParraUI() override;

  ParraUI(const ParraUI&) = delete;
  ParraUI& operator=(const ParraUI&) = delete;

  // Binds the page's factory request. Called by the WebUI's binder map.
  void BindInterface(
      mojo::PendingReceiver<parra::mojom::PageHandlerFactory> receiver);

 private:
  // parra::mojom::PageHandlerFactory:
  void CreatePageHandler(
      mojo::PendingRemote<parra::mojom::Page> page,
      mojo::PendingReceiver<parra::mojom::PageHandler> handler) override;

  std::unique_ptr<ParraPageHandler> parra_page_handler_;

  // One factory receiver per page. `reset()` in BindInterface replaces a
  // previous one, which is what happens on reload: a second factory request
  // must not leave the first receiver bound, or the old handler would keep a
  // stale Page remote alive.
  mojo::Receiver<parra::mojom::PageHandlerFactory> page_factory_receiver_{this};

  WEB_UI_CONTROLLER_TYPE_DECL();
};

#endif  // CHROME_BROWSER_UI_WEBUI_PARRA_PARRA_UI_H_
