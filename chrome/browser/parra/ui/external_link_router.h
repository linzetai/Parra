// Copyright 2026 The Parra Authors
// Use of this source code is governed by a BSD-style license.

#ifndef CHROME_BROWSER_PARRA_UI_EXTERNAL_LINK_ROUTER_H_
#define CHROME_BROWSER_PARRA_UI_EXTERNAL_LINK_ROUTER_H_

class GURL;
class Profile;

namespace parra {

// Receives only HTTP/HTTPS URLs parsed from the external command line. The
// first navigation is withheld until an environment has been chosen.
void QueueExternalLink(Profile* profile, const GURL& url);

}  // namespace parra

#endif  // CHROME_BROWSER_PARRA_UI_EXTERNAL_LINK_ROUTER_H_
