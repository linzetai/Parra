// Copyright 2026 The Parra Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chrome/browser/parra/parra_lifecycle_poc.h"

#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include "base/command_line.h"
#include "base/functional/bind.h"
#include "base/logging.h"
#include "base/memory/weak_ptr.h"
#include "base/no_destructor.h"
#include "base/strings/string_number_conversions.h"
#include "base/strings/string_split.h"
#include "base/task/sequenced_task_runner.h"
#include "base/timer/timer.h"
#include "chrome/browser/parra/parra_storage_partition_util.h"
#include "chrome/browser/parra/parra_switches.h"
#include "chrome/browser/profiles/profile.h"
#include "chrome/browser/ui/browser_window/public/browser_window_interface.h"
#include "chrome/browser/ui/browser_window/public/browser_window_interface_iterator.h"
#include "chrome/browser/ui/tabs/tab_enums.h"
#include "chrome/browser/ui/tabs/tab_model.h"
#include "chrome/browser/ui/tabs/tab_strip_model.h"
#include "content/public/browser/navigation_controller.h"
#include "content/public/browser/site_instance.h"
#include "content/public/browser/web_contents.h"
#include "ui/base/page_transition_types.h"
#include "url/gurl.h"

namespace parra {

namespace {

using EnvironmentUrls = std::vector<std::pair<std::string, std::string>>;

// Parses "a|url;b|url" into {{a,url},{b,url}}.
EnvironmentUrls ParseEnvironments(const std::string& value) {
  EnvironmentUrls result;
  for (const std::string& entry : base::SplitString(
           value, ";", base::TRIM_WHITESPACE, base::SPLIT_WANT_NONEMPTY)) {
    const size_t sep = entry.find('|');
    if (sep == std::string::npos || sep == 0 || sep + 1 >= entry.size())
      continue;
    result.emplace_back(entry.substr(0, sep), entry.substr(sep + 1));
  }
  return result;
}

}  // namespace

// static
ParraLifecyclePoc* ParraLifecyclePoc::GetInstance() {
  if (!base::CommandLine::ForCurrentProcess()->HasSwitch(
          switches::kParraPocLifecycle)) {
    return nullptr;
  }
  static base::NoDestructor<ParraLifecyclePoc> instance;
  return instance.get();
}

ParraLifecyclePoc::ParraLifecyclePoc() = default;

ParraLifecyclePoc::~ParraLifecyclePoc() {
  // Release every held tab while the owning Browser is still alive. Without
  // this, tearing down the Browser would leave WebContents owned by a
  // destroyed TabStripModel's profile.
  for (auto& [environment, tabs] : detached_tabs_) {
    for (auto& tab : tabs) {
      if (tab) {
        LOG(INFO) << "parra-lifecycle: releasing held tab of '" << environment
                  << "'";
        tab.reset();
      }
    }
  }
  detached_tabs_.clear();
}

void ParraLifecyclePoc::Initialize() {
  if (initialized_)
    return;
  initialized_ = true;

  const base::CommandLine* command_line =
      base::CommandLine::ForCurrentProcess();
  const std::string spec =
      command_line->GetSwitchValueASCII(switches::kParraPocLifecycle);
  const EnvironmentUrls environments = ParseEnvironments(spec);

  LOG(INFO) << "parra-lifecycle: initialized with " << environments.size()
            << " environment(s)";

  for (const auto& [id, url] : environments)
    LOG(INFO) << "parra-lifecycle:   env '" << id << "' url=" << url;

  active_environment_ = environments.empty() ? std::string() : environments[0].first;
}

void ParraLifecyclePoc::RegisterTab(const std::string& environment,
                                    content::WebContents* contents) {
  if (!contents)
    return;
  last_active_[environment] = contents;
  LOG(INFO) << "parra-lifecycle: registered tab id=" << contents->GetUniqueToken().ToString()
            << " env='" << environment << "'";
}

void ParraLifecyclePoc::ParkTab(const std::string& environment,
                                std::unique_ptr<tabs::TabModel> tab) {
  if (!tab)
    return;
  ++detach_count_;
  LOG(INFO) << "parra-lifecycle: parked tab web_contents_id="
            << tab->GetContents()->GetUniqueToken().ToString() << " env='" << environment
            << "'";
  detached_tabs_[environment].push_back(std::move(tab));
}

void ParraLifecyclePoc::DetachCurrentTabs(const std::string& environment) {
  BrowserWindowInterface* browser =
      GetLastActiveBrowserWindowInterfaceWithAnyProfile();
  if (!browser)
    return;
  TabStripModel* model = browser->GetTabStripModel();
  if (!model)
    return;

  // Remember which tab was active so switching back restores it.
  if (content::WebContents* active = model->GetActiveWebContents())
    last_active_[environment] = active;

  // VARIANT UNDER TEST: keep one tab in the strip instead of emptying it.
  //
  // Upstream closes the window as soon as the strip becomes empty (see
  // Browser::TabStripEmpty()), so a naive "detach everything" switch destroys
  // the window on the very first switch. Keeping a single anchor tab avoids
  // that, at the cost of the tab strip always showing one foreign tab.
  const bool keep_anchor =
      base::CommandLine::ForCurrentProcess()->HasSwitch("parra-poc-anchor-tab");
  const int reserve = keep_anchor ? 1 : 0;

  // Detach from the END down to `reserve`. Detaching index i invalidates
  // indices > i, but since we walk downwards the remaining indices we still
  // want are unaffected.
  //
  // `model->count()` shrinks on every iteration, so the loop bound must be
  // recomputed rather than cached, and `reserve` is a hard floor.
  while (model->count() > reserve) {
    const int index = model->count() - 1;
    if (index < 0)
      break;
    std::unique_ptr<tabs::TabModel> tab = model->DetachTabAtForInsertion(index);
    if (!tab)
      break;
    ++detach_count_;
    LOG(INFO) << "parra-lifecycle: detached tab index=" << index
              << " web_contents_id=" << tab->GetContents()->GetUniqueToken().ToString()
              << " -> env='" << environment << "' strip_count=" << model->count();
    detached_tabs_[environment].push_back(std::move(tab));
  }
}

void ParraLifecyclePoc::AttachTabs(const std::string& environment) {
  BrowserWindowInterface* browser =
      GetLastActiveBrowserWindowInterfaceWithAnyProfile();
  if (!browser)
    return;
  TabStripModel* model = browser->GetTabStripModel();
  if (!model)
    return;

  auto it = detached_tabs_.find(environment);
  if (it == detached_tabs_.end())
    return;

  std::vector<std::unique_ptr<tabs::TabModel>>& tabs = it->second;

  // Insert AFTER the anchor tab if one is being kept, so the anchor stays at
  // index 0 and the environment's own tabs keep their relative order.
  const int base_index =
      base::CommandLine::ForCurrentProcess()->HasSwitch("parra-poc-anchor-tab")
          ? model->count()
          : 0;

  // Take ownership of the vector first. Moving elements out of a vector we keep
  // iterating is fragile; a local copy makes the loop trivially correct.
  std::vector<std::unique_ptr<tabs::TabModel>> moving = std::move(tabs);
  tabs.clear();

  int insert_at = base_index;
  for (std::unique_ptr<tabs::TabModel>& tab : moving) {
    if (!tab)
      continue;
    content::WebContents* contents = tab->GetContents();
    model->InsertDetachedTabAt(insert_at, std::move(tab),
                              AddTabTypes::ADD_NONE);
    ++attach_count_;
    LOG(INFO) << "parra-lifecycle: attached tab index=" << insert_at
              << " web_contents_id=" << contents->GetUniqueToken().ToString()
              << " from env='" << environment << "'";
    ++insert_at;
  }

  // Restore the previously active tab.
  auto active_it = last_active_.find(environment);
  if (active_it != last_active_.end() && active_it->second) {
    const int index = model->GetIndexOfWebContents(active_it->second);
    if (index != TabStripModel::kNoTab)
      model->ActivateTabAt(index);
  }
}

bool ParraLifecyclePoc::SwitchToEnvironment(const std::string& environment) {
  ++switch_count_;
  LOG(INFO) << "parra-lifecycle: switch #" << switch_count_ << " '"
            << active_environment_ << "' -> '" << environment << "'";

  if (active_environment_ == environment) {
    LOG(INFO) << "parra-lifecycle: already active, nothing to do";
    return true;
  }

  // Detach the outgoing environment first. If this is the only environment with
  // tabs, the strip becomes momentarily empty, which is exactly the condition
  // the PoC is trying to characterise.
  if (!active_environment_.empty())
    DetachCurrentTabs(active_environment_);

  active_environment_ = environment;
  AttachTabs(environment);

  LogSnapshot("after-switch");
  return true;
}

void ParraLifecyclePoc::LogSnapshot(const char* reason) const {
  BrowserWindowInterface* browser =
      GetLastActiveBrowserWindowInterfaceWithAnyProfile();
  TabStripModel* model = browser ? browser->GetTabStripModel() : nullptr;

  LOG(INFO) << "parra-lifecycle: snapshot (" << reason << ") active='"
            << active_environment_ << "' strip_count="
            << (model ? model->count() : -1) << " switches=" << switch_count_
            << " detaches=" << detach_count_ << " attaches=" << attach_count_;

  for (const auto& [environment, tabs] : detached_tabs_) {
    for (const auto& tab : tabs) {
      if (!tab || !tab->GetContents())
        continue;
      LOG(INFO) << "parra-lifecycle:   held env='" << environment
                << "' web_contents_id=" << tab->GetContents()->GetUniqueToken().ToString();
    }
  }
}

// static
void ParraLifecyclePoc::RunFromCommandLine() {
  ParraLifecyclePoc* poc = GetInstance();
  if (!poc)
    return;

  BrowserWindowInterface* browser =
      GetLastActiveBrowserWindowInterfaceWithAnyProfile();
  if (!browser) {
    LOG(ERROR) << "parra-lifecycle: no browser window; aborting";
    return;
  }
  Profile* profile = browser->GetProfile();
  TabStripModel* model = browser->GetTabStripModel();
  if (!profile || !model) {
    LOG(ERROR) << "parra-lifecycle: no profile/tab strip; aborting";
    return;
  }

  const base::CommandLine* command_line =
      base::CommandLine::ForCurrentProcess();
  const EnvironmentUrls environments = ParseEnvironments(
      command_line->GetSwitchValueASCII(switches::kParraPocLifecycle));
  if (environments.size() < 2) {
    LOG(ERROR) << "parra-lifecycle: need at least 2 environments, got "
               << environments.size();
    return;
  }

  // Group the flat spec into ordered (environment, urls) so `a;a;a;b;b`
  // yields A with 3 tabs and B with 2.
  std::vector<std::string> env_order;
  std::map<std::string, std::vector<std::string>> env_urls;
  for (const auto& [id, url] : environments) {
    if (!env_urls.contains(id))
      env_order.push_back(id);
    env_urls[id].push_back(url);
  }

  // Remember and close the startup placeholder so the counts are exact.
  const int placeholder_index = model->count() == 1 ? 0 : TabStripModel::kNoTab;

  for (const std::string& id : env_order) {
    const int before = model->count();
    for (const std::string& url_string : env_urls[id]) {
      const GURL url(url_string);
      if (!url.is_valid()) {
        LOG(ERROR) << "parra-lifecycle: invalid url '" << url_string << "'";
        continue;
      }
      const content::StoragePartitionConfig config =
          GetStoragePartitionConfigForEnvironment(profile, id);
      auto site_instance =
          content::SiteInstance::CreateForFixedStoragePartition(profile, url,
                                                               config);
      content::WebContents::CreateParams params(profile,
                                                std::move(site_instance));
      std::unique_ptr<content::WebContents> contents =
          content::WebContents::Create(params);
      content::WebContents* raw = contents.get();
      model->AddWebContents(std::move(contents), -1, ui::PAGE_TRANSITION_TYPED,
                            AddTabTypes::ADD_ACTIVE);
      content::NavigationController::LoadURLParams load_params(url);
      load_params.transition_type = ui::PAGE_TRANSITION_TYPED;
      raw->GetController().LoadURLWithParams(load_params);
      poc->RegisterTab(id, raw);
    }

    // Every environment after the first starts OUT of the tab strip, which is
    // the whole point of the experiment: its tabs live only in our container.
    if (id != env_order.front()) {
      const int created = model->count() - before;
      for (int i = 0; i < created; ++i) {
        const int index = model->count() - 1;
        std::unique_ptr<tabs::TabModel> tab =
            model->DetachTabAtForInsertion(index);
        if (!tab)
          break;
        if (base::CommandLine::ForCurrentProcess()->HasSwitch(
                "parra-poc-immediate-reinsert")) {
          // Control experiment: put the tab straight back. If the process
          // survives this, the crash comes from HOLDING a detached TabModel
          // rather than from the detach call itself.
          LOG(INFO) << "parra-lifecycle: immediate reinsert";
          model->InsertDetachedTabAt(model->count(), std::move(tab),
                                    AddTabTypes::ADD_NONE);
        } else {
          poc->ParkTab(id, std::move(tab));
        }
      }
    }
  }

  if (placeholder_index != TabStripModel::kNoTab) {
    const int index = model->count() > 0 ? 0 : TabStripModel::kNoTab;
    if (index != TabStripModel::kNoTab)
      model->CloseWebContentsAt(index, TabCloseTypes::CLOSE_NONE);
  }

  poc->SetActiveEnvironmentForTesting(env_order.front());
  poc->LogSnapshot("initial");

  // Observe browser closure so the log records WHY the window went away.
  // Without this the process simply vanishes and we cannot tell an intentional
  // close from a crash.
  poc->close_subscription_ = browser->RegisterBrowserDidClose(
      base::BindRepeating([](BrowserWindowInterface*) {
        LOG(INFO) << "parra-lifecycle: BrowserDidClose fired";
      }));
  LOG(INFO) << "parra-lifecycle: registered BrowserDidClose observer";

  int cycles = 1;
  base::StringToInt(
      command_line->GetSwitchValueASCII(switches::kParraPocLifecycleCycles),
      &cycles);
  if (cycles <= 0) {
    LOG(INFO) << "parra-lifecycle: auto-cycle disabled";
    return;
  }

  // Drive the switch sequence on the message loop so navigations can commit
  // between rounds; doing it inline during startup would race the first load.
  LOG(INFO) << "parra-lifecycle: starting " << cycles << " cycle(s) over "
            << env_order.size() << " environments";

  struct State {
    int remaining = 0;
    int cursor = 0;
  };
  auto state = std::make_shared<State>();
  state->remaining = cycles * static_cast<int>(env_order.size());

  auto env_order_ptr = std::make_shared<std::vector<std::string>>(env_order);
  poc->cycle_timer_ = std::make_unique<base::RepeatingTimer>();
  poc->cycle_timer_->Start(
      FROM_HERE, base::Seconds(1),
      base::BindRepeating(
          [](ParraLifecyclePoc* poc_ptr, std::shared_ptr<State> s,
             std::shared_ptr<std::vector<std::string>> order,
             base::RepeatingTimer* timer) {
            BrowserWindowInterface* live =
                GetLastActiveBrowserWindowInterfaceWithAnyProfile();
            LOG(INFO) << "parra-lifecycle: tick remaining=" << s->remaining
                      << " cursor=" << s->cursor
                      << " browser=" << (live ? "alive" : "GONE");
            if (s->remaining <= 0) {
              timer->Stop();
              poc_ptr->LogSnapshot("final");
              LOG(INFO) << "parra-lifecycle: cycles complete";
              return;
            }
            if (!live) {
              timer->Stop();
              LOG(ERROR) << "parra-lifecycle: browser disappeared; stopping";
              return;
            }
            s->cursor = (s->cursor + 1) % static_cast<int>(order->size());
            poc_ptr->SwitchToEnvironment((*order)[s->cursor]);
            --s->remaining;
          },
          poc, state, env_order_ptr, poc->cycle_timer_.get()));
}

}  // namespace parra
