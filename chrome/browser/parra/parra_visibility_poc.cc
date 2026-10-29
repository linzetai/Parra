// Copyright 2026 The Parra Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chrome/browser/parra/parra_visibility_poc.h"

#include <memory>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include "base/command_line.h"
#include "base/functional/bind.h"
#include "base/logging.h"
#include "base/no_destructor.h"
#include "base/strings/string_number_conversions.h"
#include "base/timer/timer.h"
#include "chrome/browser/parra/environment_index_mapper.h"
#include "chrome/browser/parra/environment_session_persistence.h"
#include "chrome/browser/parra/environment_visibility_controller.h"
#include "chrome/browser/parra/parra_storage_partition_util.h"
#include "chrome/browser/parra/parra_switches.h"
#include "chrome/browser/parra/window_environment_state.h"
#include "chrome/browser/profiles/profile.h"
#include "chrome/browser/sessions/session_service.h"
#include "chrome/browser/sessions/session_service_factory.h"
#include "components/sessions/content/session_tab_helper.h"
#include "chrome/browser/ui/browser_window/public/browser_window_interface.h"
#include "chrome/browser/ui/browser_window/public/browser_window_interface_iterator.h"
#include "chrome/browser/ui/tabs/tab_enums.h"
#include "chrome/browser/ui/tabs/tab_strip_model.h"
#include "content/public/browser/navigation_controller.h"
#include "content/public/browser/render_frame_host.h"
#include "content/public/browser/security_principal.h"
#include "content/public/browser/site_instance.h"
#include "content/public/browser/web_contents.h"
#include "ui/base/page_transition_types.h"
#include "url/gurl.h"

namespace parra {

namespace {

// Key used in SessionTab::extra_data to record the owning environment.
constexpr char kEnvironmentExtraDataKey[] = "parra.environment_id";

// Authoritative probe of the StoragePartition a live WebContents is actually
// using.
//
// Why this exists: "which environment do we think this tab belongs to" (our own
// user data) and "which partition does Chromium actually route its storage and
// network through" are two different facts, and they can disagree. Diagnostics
// that only look at our own bookkeeping cannot detect that disagreement, and
// diagnostics that only count partition directories on disk can be fooled by
// stale directories. The only trustworthy source is the SiteInstance's
// SecurityPrincipal.
//
// The partition is a property of the committed document, so this returns an
// empty string before the first commit.
std::string ActualPartitionConfigOf(content::WebContents* contents) {
  if (!contents)
    return "<null-contents>";

  content::RenderFrameHost* main_frame = contents->GetPrimaryMainFrame();
  if (!main_frame)
    return "<no-main-frame>";

  scoped_refptr<content::SiteInstance> site_instance =
      main_frame->GetSiteInstance();
  if (!site_instance)
    return "<no-site-instance>";

  const content::StoragePartitionConfig& config =
      site_instance->GetSecurityPrincipal().GetStoragePartitionConfig();
  // StoragePartitionConfig only supports stream output, not ToString().
  std::ostringstream out;
  out << config;
  return out.str();
}

// The fixed environment layout used by this PoC. Deliberately hardcoded: the
// point is to prove the visibility mechanism, not to build configuration.
struct EnvironmentSpec {
  const char* id;
  const char* url;
  int tab_count;
};

constexpr EnvironmentSpec kEnvironments[] = {
    {"personal", "https://sheet.test:8443/", 3},
    {"test-a", "https://sheet.test:8443/", 3},
    {"test-b", "https://sheet.test:8443/", 3},
};

// Writes every tab's environment id into its SessionTab::extra_data.
//
// Why this and not a Parra-owned manifest: the session service is already the
// single source of truth for which tabs exist and in what order. Recording the
// environment here means the two can never disagree -- there is no second list
// of tabs to keep in sync, and a tab that is closed is simply gone from both.
//
// The read side already exists: session_service_commands.cc restores
// extra_data into SessionTab, and browser_tabrestore.cc hands it to
// CreateRestoredTab() before the first navigation is committed.
void WriteEnvironmentExtraData(Profile* profile,
                              BrowserWindowInterface* browser,
                              TabStripModel* model) {
  SessionService* service = SessionServiceFactory::GetForProfile(profile);
  if (!service) {
    LOG(ERROR) << "parra-vis: no SessionService; environment ids not persisted";
    return;
  }

  const SessionID window_id = browser->GetSessionID();
  if (!window_id.is_valid()) {
    LOG(ERROR) << "parra-vis: window has no session id; skipping extra_data";
    return;
  }

  int written = 0;
  for (int i = 0; i < model->count(); ++i) {
    content::WebContents* contents = model->GetWebContentsAt(i);
    if (!contents)
      continue;

    const SessionID tab_id = sessions::SessionTabHelper::IdForTab(contents);
    if (!tab_id.is_valid()) {
      LOG(ERROR) << "parra-vis: tab at index " << i
                 << " has no session id; skipping extra_data";
      continue;
    }

    const std::string environment =
        EnvironmentIndexMapper::GetEnvironmentForContents(contents);
    service->AddTabExtraData(window_id, tab_id, kEnvironmentExtraDataKey,
                             environment);
    ++written;
    LOG(INFO) << "parra-vis: extra_data index=" << i
              << " tab_id=" << tab_id.id() << " env='" << environment << "'";
  }
  LOG(INFO) << "parra-vis: wrote environment extra_data for " << written
            << " tab(s)";
}

// Owns the PoC state for the process. Kept alive past RunFromCommandLine()
// because the switch timer and the controller must outlive startup.
class VisibilityPocRunner {
 public:
  static VisibilityPocRunner* GetInstance() {
    if (!base::CommandLine::ForCurrentProcess()->HasSwitch(
            switches::kParraPocVisibility)) {
      return nullptr;
    }
    static base::NoDestructor<VisibilityPocRunner> instance;
    return instance.get();
  }

  void Run();

 private:
  void OnTick();

  void CreateEnvironmentTabs(BrowserWindowInterface* browser,
                             Profile* profile,
                             TabStripModel* model,
                             const EnvironmentSpec& spec,
                             int tab_count);

  std::unique_ptr<EnvironmentVisibilityController> controller_;
  std::unique_ptr<base::RepeatingTimer> timer_;

  // Model indices of every tab, per environment, in creation order.
  std::vector<std::string> environment_order_;

  int remaining_switches_ = 0;
  int cursor_ = 0;
};

void VisibilityPocRunner::CreateEnvironmentTabs(BrowserWindowInterface* browser,
                                                Profile* profile,
                                                TabStripModel* model,
                                                const EnvironmentSpec& spec,
                                                int tab_count) {
  // Logged on every call: the tab count going in is what explains where the first
  // inserted index comes from. When a launch method mangles the command line (for
  // example Start-Process dropping the quotes around --parra-poc-host-rules), the
  // symptom is a strip that already had tabs before the PoC ran, and without this
  // line that shows up much later as "personal has 7 tabs".
  LOG(INFO) << "parra-vis: CreateEnvironmentTabs env='" << spec.id
            << "' count_before=" << model->count() << " to_create=" << tab_count;

  const content::StoragePartitionConfig config =
      GetStoragePartitionConfigForEnvironment(profile, spec.id);

  for (int i = 0; i < tab_count; ++i) {
    const GURL url(spec.url);
    if (!url.is_valid()) {
      LOG(ERROR) << "parra-vis: invalid url '" << spec.url << "'";
      return;
    }

    std::unique_ptr<content::WebContents> contents;
    if (config.is_default()) {
      // CreateForFixedStoragePartition() CHECKs !is_default(), so an
      // environment that maps to the default partition must use the ordinary
      // path. See site_instance_impl.cc:253.
      content::WebContents::CreateParams params(profile);
      contents = content::WebContents::Create(params);
    } else {
      auto site_instance = content::SiteInstance::CreateForFixedStoragePartition(
          profile, url, config);
      content::WebContents::CreateParams params(profile,
                                                std::move(site_instance));
      contents = content::WebContents::Create(params);
    }
    content::WebContents* raw = contents.get();

    // Tag BEFORE inserting: the mapper must never observe an untagged tab it is
    // expected to place in a non-default environment.
    EnvironmentIndexMapper::SetEnvironmentForContents(raw, spec.id);

    model->AddWebContents(std::move(contents), -1, ui::PAGE_TRANSITION_TYPED,
                          AddTabTypes::ADD_ACTIVE);

    content::NavigationController::LoadURLParams load_params(url);
    load_params.transition_type = ui::PAGE_TRANSITION_TYPED;
    raw->GetController().LoadURLWithParams(load_params);

    LOG(INFO) << "parra-vis: created tab env='" << spec.id << "' n=" << i
              << " model_index=" << model->count() - 1
              << " token=" << raw->GetUniqueToken().ToString()
              << " partition=" << config;
  }
}

void VisibilityPocRunner::Run() {
  BrowserWindowInterface* browser =
      GetLastActiveBrowserWindowInterfaceWithAnyProfile();
  if (!browser) {
    LOG(ERROR) << "parra-vis: no browser window; aborting";
    return;
  }
  Profile* profile = browser->GetProfile();
  TabStripModel* model = browser->GetTabStripModel();
  if (!profile || !model) {
    LOG(ERROR) << "parra-vis: no profile/tab strip; aborting";
    return;
  }

  // The startup placeholder must not be closed: closing the last tab while the
  // Browser is still initialising trips `!IsDeleteScheduled()` in
  // Browser::OnTabInsertedAt. Instead the placeholder is adopted as the first
  // Personal tab, which also keeps the strip non-empty at all times.
  //
  // Adopting alone is not enough: the placeholder sits on chrome://newtab/ and
  // has never visited the test origin, so it would silently shrink the sample
  // to 8 usable tabs and make isolation look wrong. Navigate it explicitly.
  content::WebContents* placeholder =
      model->count() == 1 ? model->GetWebContentsAt(0) : nullptr;

  // Logged unconditionally: `count` here is the state the whole 3x3 layout is
  // built on top of. A count other than 1 means the strip already had tabs before
  // the PoC ran, which is a launch-method problem rather than a Parra bug, and is
  // worth distinguishing from one in the log.
  LOG(INFO) << "parra-vis: placeholder check model_count=" << model->count()
            << " placeholder=" << (placeholder ? "yes" : "no");

  if (placeholder) {
    EnvironmentIndexMapper::SetEnvironmentForContents(
        placeholder, kEnvironments[0].id);

    const GURL placeholder_url(kEnvironments[0].url);
    if (placeholder_url.is_valid()) {
      content::NavigationController::LoadURLParams load_params(placeholder_url);
      load_params.transition_type = ui::PAGE_TRANSITION_TYPED;
      placeholder->GetController().LoadURLWithParams(load_params);
    }

    LOG(INFO) << "parra-vis: adopted startup placeholder as first '"
              << kEnvironments[0].id << "' tab token="
              << placeholder->GetUniqueToken().ToString()
              << " navigated_to=" << kEnvironments[0].url;
  }

  for (const EnvironmentSpec& spec : kEnvironments) {
    environment_order_.emplace_back(spec.id);
    // The first environment contributed the placeholder above, so it needs one
    // fewer new tab.
    const int extra = (placeholder && std::string(spec.id) == kEnvironments[0].id)
                          ? spec.tab_count - 1
                          : spec.tab_count;
    CreateEnvironmentTabs(browser, profile, model, spec, extra);
  }

  LOG(INFO) << "parra-vis: total tabs in strip=" << model->count()
            << " (all three environments live in ONE TabStripModel)";

  // Emit the authoritative tab identity table.
  //
  // Each line answers three questions that must not be conflated:
  //   env=      which logical environment we recorded for this tab
  //   actual=   which StoragePartition Chromium is really using
  //   token=    stable WebContents identity, so CDP targets can be matched
  //
  // Emitted after the first load has had a chance to commit, because the
  // partition is only observable once a document exists.
  for (int i = 0; i < model->count(); ++i) {
    content::WebContents* contents = model->GetWebContentsAt(i);
    LOG(INFO) << "parra-id: index=" << i
              << " env=" << EnvironmentIndexMapper::GetEnvironmentForContents(contents)
              << " actual=" << ActualPartitionConfigOf(contents)
              << " token="
              << (contents ? contents->GetUniqueToken().ToString() : "<null>")
              << " url="
              << (contents ? contents->GetLastCommittedURL().possibly_invalid_spec()
                           : std::string());
  }

  controller_ = std::make_unique<EnvironmentVisibilityController>(model);
  // Personal is the initial environment; activate its first tab explicitly so
  // the strip does not start on a test environment's tab.
  controller_->mapper().SetActiveEnvironment(kEnvironments[0].id);

  // Persist environment ownership into session restore.
  //
  // This is the ONLY supported hook that survives a browser restart: every tab's
  // SessionTab carries an extra_data map, and browser_tabrestore.cc hands it to
  // CreateRestoredTab() *before* the first navigation is restored. That ordering
  // is what lets a future implementation pin the SiteInstance partition before
  // any request leaves the process.
  WriteEnvironmentExtraData(profile, browser, model);

  // P3.3: also record the window's active environment at startup.
  //
  // The per-tab write above happens on every tab insertion (see browser.cc), but
  // the WINDOW's active environment only changes on an explicit switch. Recording
  // it here means a window that never switches still has a recorded value, instead
  // of restoring to Personal just because nothing was ever written. Without this,
  // "the user never left test-a" is indistinguishable from "nothing was recorded"
  // and the window would reopen on Personal.
  if (WindowEnvironmentState* window_state =
          WindowEnvironmentState::GetFor(browser)) {
    EnvironmentSessionPersistence::RecordWindowActiveEnvironment(
        profile, browser, window_state->active_environment());
  }

  const int cycles = [] {
    int value = 5;
    base::StringToInt(
        base::CommandLine::ForCurrentProcess()->GetSwitchValueASCII(
            switches::kParraPocVisibilityCycles),
        &value);
    return value;
  }();

  if (cycles <= 0) {
    LOG(INFO) << "parra-vis: cycling disabled";
    return;
  }

  remaining_switches_ = cycles * static_cast<int>(environment_order_.size());
  LOG(INFO) << "parra-vis: starting " << remaining_switches_
            << " switch(es) across " << environment_order_.size()
            << " environments";

  timer_ = std::make_unique<base::RepeatingTimer>();
  timer_->Start(FROM_HERE, base::Seconds(1),
                base::BindRepeating(&VisibilityPocRunner::OnTick,
                                    base::Unretained(this)));
}

void VisibilityPocRunner::OnTick() {
  if (!controller_) {
    if (timer_)
      timer_->Stop();
    return;
  }

  BrowserWindowInterface* browser =
      GetLastActiveBrowserWindowInterfaceWithAnyProfile();
  if (!browser) {
    LOG(ERROR) << "parra-vis: browser disappeared; stopping";
    if (timer_)
      timer_->Stop();
    return;
  }

  if (remaining_switches_ <= 0) {
    LOG(INFO) << "parra-vis: cycles complete, total switches="
              << controller_->switch_count();

    // Leave the window on a caller-chosen environment, if one was named.
    //
    // The cycle above always ends on the FIRST environment (the cursor wraps
    // through the list), so every run would otherwise finish on `personal`. That
    // makes the window-active-environment restore check vacuous, because restoring
    // `personal` is indistinguishable from restoring nothing at all. This lets a
    // test park the window on a non-default environment before quitting.
    const std::string final_environment =
        base::CommandLine::ForCurrentProcess()->GetSwitchValueASCII(
            switches::kParraPocFinalEnvironment);
    if (!final_environment.empty()) {
      if (controller_->SwitchTo(final_environment)) {
        LOG(INFO) << "parra-vis: left window on '" << final_environment
                  << "' for the restart check";
      } else {
        LOG(ERROR) << "parra-vis: could not switch to '" << final_environment
                   << "' for the restart check";
      }
    }

    // Final authoritative identity dump. By now every tab has committed at
    // least one document, so `actual=` is populated for all of them. This is
    // the table the verification scripts reason about.
    TabStripModel* model = browser->GetTabStripModel();
    for (int i = 0; model && i < model->count(); ++i) {
      content::WebContents* contents = model->GetWebContentsAt(i);
      LOG(INFO) << "parra-final: index=" << i
                << " env="
                << EnvironmentIndexMapper::GetEnvironmentForContents(contents)
                << " actual=" << ActualPartitionConfigOf(contents)
                << " token="
                << (contents ? contents->GetUniqueToken().ToString() : "<null>")
                << " url="
                << (contents
                        ? contents->GetLastCommittedURL().possibly_invalid_spec()
                        : std::string());
    }

    if (timer_)
      timer_->Stop();
    return;
  }

  cursor_ = (cursor_ + 1) % static_cast<int>(environment_order_.size());
  const std::string& target = environment_order_[cursor_];
  LOG(INFO) << "parra-vis: tick remaining=" << remaining_switches_
            << " -> '" << target << "'";
  controller_->SwitchTo(target);
  --remaining_switches_;
}

}  // namespace

void MaybeRunVisibilityPoc() {
  VisibilityPocRunner* runner = VisibilityPocRunner::GetInstance();
  if (!runner)
    return;
  runner->Run();
}

}  // namespace parra
