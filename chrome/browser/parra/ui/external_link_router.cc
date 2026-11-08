// Copyright 2026 The Parra Authors
// Use of this source code is governed by a BSD-style license.

#include "chrome/browser/parra/ui/external_link_router.h"

#include <deque>
#include <memory>
#include <optional>
#include <string>
#include <utility>

#include "base/functional/bind.h"
#include "base/functional/callback.h"
#include "base/logging.h"
#include "base/memory/weak_ptr.h"
#include "base/no_destructor.h"
#include "base/strings/utf_string_conversions.h"
#include "base/task/single_thread_task_runner.h"
#include "base/time/time.h"
#include "chrome/browser/parra/environment_store.h"
#include "chrome/browser/parra/window_environment_state.h"
#include "chrome/browser/profiles/profile.h"
#include "chrome/browser/ui/browser_tabstrip.h"
#include "chrome/browser/ui/browser_window/public/browser_window_interface.h"
#include "chrome/browser/ui/browser_window/public/browser_window_interface_iterator.h"
#include "chrome/common/pref_names.h"
#include "components/constrained_window/constrained_window_views.h"
#include "components/prefs/pref_service.h"
#include "ui/base/base_window.h"
#include "ui/base/mojom/dialog_button.mojom.h"
#include "ui/base/mojom/ui_base_types.mojom-shared.h"
#include "ui/gfx/geometry/insets.h"
#include "ui/views/controls/button/md_text_button.h"
#include "ui/views/controls/label.h"
#include "ui/views/layout/box_layout.h"
#include "ui/views/view.h"
#include "ui/views/widget/widget.h"
#include "ui/views/window/dialog_delegate.h"
#include "url/gurl.h"

namespace parra {
namespace {

struct PendingLink {
  base::WeakPtr<Profile> profile;
  GURL url;
  int window_attempts = 0;
};

class ExternalLinkRouter {
 public:
  void Enqueue(Profile* profile, const GURL& url) {
    if (!profile || !url.SchemeIsHTTPOrHTTPS() || !url.is_valid()) {
      return;
    }
    pending_.push_back({profile->GetWeakPtr(), url, 0});
    ScheduleNext();
  }

 private:
  friend class base::NoDestructor<ExternalLinkRouter>;
  ExternalLinkRouter() = default;

  void ScheduleNext() {
    if (active_ || pending_.empty() || scheduled_) {
      return;
    }
    scheduled_ = true;
    base::SingleThreadTaskRunner::GetCurrentDefault()->PostTask(
        FROM_HERE, base::BindOnce(&ExternalLinkRouter::ProcessNext,
                                  weak_factory_.GetWeakPtr()));
  }

  BrowserWindowInterface* FindWindow(Profile* profile) {
    BrowserWindowInterface* last =
        GetLastActiveBrowserWindowInterfaceWithAnyProfile();
    if (last && last->GetProfile() == profile &&
        last->GetType() == BrowserWindowInterface::Type::TYPE_NORMAL) {
      return last;
    }
    for (BrowserWindowInterface* window : GetAllBrowserWindowInterfaces()) {
      if (window && window->GetProfile() == profile &&
          window->GetType() == BrowserWindowInterface::Type::TYPE_NORMAL) {
        return window;
      }
    }
    return nullptr;
  }

  void ProcessNext() {
    scheduled_ = false;
    if (active_ || pending_.empty()) {
      return;
    }
    PendingLink& next = pending_.front();
    if (!next.profile) {
      pending_.pop_front();
      ScheduleNext();
      return;
    }
    BrowserWindowInterface* window = FindWindow(next.profile.get());
    // On cold startup a BrowserWindowInterface can exist before its native
    // window is shown. A window-modal picker created then is placed at the
    // desktop center instead of over Parra's browser window.
    if (!window || !window->GetWindow() ||
        !window->GetWindow()->GetNativeWindow() ||
        !window->GetWindow()->IsVisible()) {
      if (++next.window_attempts > 600) {
        LOG(ERROR) << "parra-external: no browser window for " << next.url;
        pending_.pop_front();
        ScheduleNext();
        return;
      }
      scheduled_ = true;
      base::SingleThreadTaskRunner::GetCurrentDefault()->PostDelayedTask(
          FROM_HERE,
          base::BindOnce(&ExternalLinkRouter::ProcessNext,
                         weak_factory_.GetWeakPtr()),
          base::Milliseconds(100));
      return;
    }
    active_ = std::move(next);
    pending_.pop_front();
    active_window_ = window->GetWeakPtr();

    PrefService* prefs = active_->profile->GetPrefs();
    const int policy = prefs->GetInteger(prefs::kParraExternalLinkPolicy);
    if (policy == 1) {
      WindowEnvironmentState* state = WindowEnvironmentState::GetFor(window);
      if (!state) {
        ShowFailure(u"当前窗口环境尚未初始化，链接没有打开。请稍后重试。");
        return;
      }
      OnChosen(state->active_environment());
    } else if (policy == 2) {
      OnChosen(prefs->GetString(prefs::kParraExternalLinkEnvironment));
    } else {
      EnvironmentStore* store =
          EnvironmentStore::GetForProfile(active_->profile.get());
      if (!store) {
        ShowFailure(u"当前 Profile 无法读取环境配置，链接没有打开。");
        return;
      }
      auto picker = std::make_unique<views::DialogDelegate>();
      views::DialogDelegate* delegate = picker.get();
      picker->SetTitle(u"选择打开环境");
      picker->SetModalType(ui::mojom::ModalType::kWindow);
      picker->SetButtons(static_cast<int>(ui::mojom::DialogButton::kCancel));
      picker->SetButtonLabel(ui::mojom::DialogButton::kCancel, u"取消");
      auto selected = std::make_shared<std::string>();
      picker->SetAcceptCallback(base::BindOnce(
          [](base::WeakPtr<ExternalLinkRouter> router,
             std::shared_ptr<std::string> selected_id) {
            if (router) {
              router->OnChosen(*selected_id);
            }
          },
          weak_factory_.GetWeakPtr(), selected));
      picker->SetCancelCallback(base::BindOnce(&ExternalLinkRouter::OnChosen,
                                               weak_factory_.GetWeakPtr(),
                                               std::string()));
      picker->SetCloseCallback(base::BindOnce(&ExternalLinkRouter::OnChosen,
                                              weak_factory_.GetWeakPtr(),
                                              std::string()));
      auto content = std::make_unique<views::View>();
      content->SetLayoutManager(std::make_unique<views::BoxLayout>(
          views::BoxLayout::Orientation::kVertical, gfx::Insets::VH(14, 20),
          8));
      content->AddChildView(std::make_unique<views::Label>(
          u"外部链接目标：" + base::UTF8ToUTF16(active_->url.host())));
      content->AddChildView(
          std::make_unique<views::Label>(u"选择环境后才会打开网页。"));
      auto add_environment = [&](const Environment& environment) {
        const std::u16string name = environment.id == "personal"
                                        ? u"个人环境"
                                        : base::UTF8ToUTF16(environment.name);
        content->AddChildView(std::make_unique<views::MdTextButton>(
            base::BindRepeating(
                [](std::shared_ptr<std::string> selected_id,
                   views::DialogDelegate* dialog, std::string id) {
                  *selected_id = std::move(id);
                  dialog->AcceptDialog();
                },
                selected, base::Unretained(delegate), environment.id),
            u"在 " + name + u" 打开"));
      };
      add_environment(store->PersonalEnvironment());
      for (const Environment& environment : store->environments()) {
        if (environment.enabled) {
          add_environment(environment);
        }
      }
      picker->SetContentsView(std::move(content));
      constrained_window::CreateBrowserModalDialogViews(
          std::move(picker), window->GetWindow()->GetNativeWindow())
          ->Show();
    }
  }

  void ShowFailure(const std::u16string& message) {
    LOG(ERROR) << "parra-external: " << base::UTF16ToUTF8(message);
    if (!active_window_ || !active_window_->GetWindow() ||
        !active_window_->GetWindow()->GetNativeWindow()) {
      Finish();
      return;
    }
    auto failure = std::make_unique<views::DialogDelegate>();
    failure->SetTitle(u"无法打开外部链接");
    failure->SetModalType(ui::mojom::ModalType::kWindow);
    failure->SetButtons(static_cast<int>(ui::mojom::DialogButton::kOk));
    failure->SetButtonLabel(ui::mojom::DialogButton::kOk, u"确定");
    failure->SetAcceptCallback(base::BindOnce(&ExternalLinkRouter::Finish,
                                              weak_factory_.GetWeakPtr()));
    failure->SetCloseCallback(base::BindOnce(&ExternalLinkRouter::Finish,
                                             weak_factory_.GetWeakPtr()));
    auto content = std::make_unique<views::View>();
    content->SetLayoutManager(std::make_unique<views::BoxLayout>(
        views::BoxLayout::Orientation::kVertical, gfx::Insets::VH(14, 20), 0));
    auto label = std::make_unique<views::Label>(message);
    label->SetMultiLine(true);
    label->SetMaximumWidth(360);
    content->AddChildView(std::move(label));
    failure->SetContentsView(std::move(content));
    constrained_window::CreateBrowserModalDialogViews(
        std::move(failure), active_window_->GetWindow()->GetNativeWindow())
        ->Show();
  }

  void OnChosen(std::string environment_id) {
    if (!active_) {
      return;
    }
    if (environment_id.empty()) {
      LOG(INFO) << "parra-external: user cancelled " << active_->url;
      Finish();
      return;
    }
    if (!active_->profile || !active_window_ ||
        active_window_->GetProfile() != active_->profile.get()) {
      Finish();
      return;
    }
    EnvironmentStore* store =
        EnvironmentStore::GetForProfile(active_->profile.get());
    const Environment* environment = environment_id == "personal" || !store
                                         ? nullptr
                                         : store->Find(environment_id);
    if (!store || (environment_id != "personal" &&
                   (!environment || !environment->enabled))) {
      ShowFailure(
          u"指定环境已停用或删除，链接没有打开。请在 Parra "
          u"环境管理中更改策略。");
      return;
    }

    content::WebContents* opened = chrome::AddAndReturnTabAt(
        active_window_.get(), active_->url, -1, true, std::nullopt, false,
        std::nullopt, environment_id);
    if (!opened) {
      ShowFailure(u"无法在所选环境创建页面，当前环境没有改变。");
      return;
    }
    // A forwarded link may arrive while Parra is behind another application.
    // The destination tab is active in its browser, so show that browser as
    // well; otherwise the link appears to do nothing to the user.
    active_window_->GetWindow()->Activate();
    LOG(INFO) << "parra-external: opened " << active_->url
              << " in environment '" << environment_id << "'";
    Finish();
  }

  void Finish() {
    active_.reset();
    active_window_.reset();
    ScheduleNext();
  }

  std::deque<PendingLink> pending_;
  std::optional<PendingLink> active_;
  base::WeakPtr<BrowserWindowInterface> active_window_;
  bool scheduled_ = false;
  base::WeakPtrFactory<ExternalLinkRouter> weak_factory_{this};
};

ExternalLinkRouter& GetRouter() {
  static base::NoDestructor<ExternalLinkRouter> router;
  return *router;
}

}  // namespace

void QueueExternalLink(Profile* profile, const GURL& url) {
  GetRouter().Enqueue(profile, url);
}

}  // namespace parra
