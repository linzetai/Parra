// Copyright 2026 The Parra Authors
// Use of this source code is governed by a BSD-style license.
#include "chrome/browser/parra/ui/parra_settings_window.h"

#include <map>
#include <memory>
#include <utility>

#include "base/functional/bind.h"
#include "base/no_destructor.h"
#include "chrome/browser/profiles/profile.h"
#include "chrome/browser/profiles/profile_observer.h"
#include "chrome/common/webui_url_constants.h"
#include "third_party/skia/include/core/SkBitmap.h"
#include "third_party/skia/include/core/SkCanvas.h"
#include "third_party/skia/include/core/SkPaint.h"
#include "third_party/skia/include/core/SkPathBuilder.h"
#include "ui/base/models/image_model.h"
#include "ui/gfx/geometry/rect.h"
#include "ui/gfx/image/image_skia.h"
#include "ui/views/controls/webview/webview.h"
#include "ui/views/widget/widget.h"
#include "ui/views/widget/widget_delegate.h"
#include "url/gurl.h"

namespace parra {
namespace {
class SettingsWindow;
using Windows = std::map<Profile*, std::unique_ptr<SettingsWindow>>;
Windows& GetWindows() {
  static base::NoDestructor<Windows> windows;
  return *windows;
}

// A small Parra monogram, used for both the native titlebar and taskbar.
ui::ImageModel CreateIcon() {
  SkBitmap bitmap;
  bitmap.allocN32Pixels(64, 64);
  bitmap.eraseColor(SK_ColorTRANSPARENT);
  SkCanvas canvas(bitmap);
  SkPaint paint;
  paint.setAntiAlias(true);
  paint.setColor(SkColorSetRGB(38, 89, 72));
  canvas.drawRoundRect(SkRect::MakeXYWH(2, 2, 60, 60), 14, 14, paint);
  paint.setColor(SK_ColorWHITE);
  paint.setStyle(SkPaint::kStroke_Style);
  paint.setStrokeWidth(6);
  paint.setStrokeCap(SkPaint::kRound_Cap);
  SkPathBuilder path;
  path.moveTo(23, 49);
  path.lineTo(23, 16);
  path.lineTo(34, 16);
  path.cubicTo(51, 16, 51, 34, 34, 34);
  path.lineTo(23, 34);
  canvas.drawPath(path.detach(), paint);
  return ui::ImageModel::FromImageSkia(
      gfx::ImageSkia::CreateFrom1xBitmap(bitmap));
}

class SettingsWindow : public ProfileObserver {
 public:
  explicit SettingsWindow(Profile* profile) : profile_(profile) {
    profile_->AddObserver(this);
    delegate_ = std::make_unique<views::WidgetDelegate>();
    delegate_->SetTitle(u"Parra · 环境管理");
    delegate_->SetHasWindowSizeControls(true);
    delegate_->SetShowIcon(true);
    delegate_->SetIcon(CreateIcon());
    delegate_->SetAppIcon(CreateIcon());
    auto web_view = std::make_unique<views::WebView>(profile_);
    web_view_ = web_view.get();
    delegate_->SetContentsView(std::move(web_view));
    widget_ = std::make_unique<views::Widget>();
    views::Widget::InitParams params(
        views::Widget::InitParams::CLIENT_OWNS_WIDGET,
        views::Widget::InitParams::TYPE_WINDOW);
    params.name = "ParraSettingsWindow";
    params.bounds = gfx::Rect(100, 100, 1100, 760);
    params.delegate = delegate_.get();
    widget_->Init(std::move(params));
    widget_->MakeCloseSynchronous(
        base::BindOnce(&SettingsWindow::OnClose, base::Unretained(this)));
    web_view_->LoadInitialURL(GURL(chrome::kChromeUIParraURL));
  }
  ~SettingsWindow() override {
    profile_->RemoveObserver(this);
    widget_.reset();
  }
  void Show() {
    widget_->Show();
    widget_->Activate();
  }
  void OnProfileWillBeDestroyed(Profile* profile) override {
    GetWindows().erase(profile);
  }

 private:
  void OnClose(views::Widget::ClosedReason reason) {
    GetWindows().erase(profile_);
  }
  raw_ptr<Profile> profile_;
  raw_ptr<views::WebView> web_view_ = nullptr;
  std::unique_ptr<views::WidgetDelegate> delegate_;
  std::unique_ptr<views::Widget> widget_;
};
}  // namespace

void ShowSettingsWindow(Profile* profile) {
  if (!profile || !profile->IsRegularProfile()) {
    return;
  }
  auto& windows = GetWindows();
  auto it = windows.find(profile);
  if (it == windows.end()) {
    it = windows.emplace(profile, std::make_unique<SettingsWindow>(profile))
             .first;
  }
  it->second->Show();
}
}  // namespace parra
