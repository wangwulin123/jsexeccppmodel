#include "jsapi_browser_handle.h"

#include <utility>

#include "include/cef_task.h"
#include "include/wrapper/cef_helpers.h"
#include "jsapi_browser_bridge.h"

namespace jsapi {
namespace cef {

namespace {

// multi_threaded_message_loop 下 CreateBrowser 只能在 CEF UI 线程调用。
class CreateBrowserTask final : public CefTask {
 public:
  CreateBrowserTask(CefRefPtr<BrowserHandleImpl> owner,
                    const BrowserOptions& options)
      : owner_(owner), options_(options) {}

  void Execute() override {
    CefWindowInfo window_info;
    window_info.runtime_style = CEF_RUNTIME_STYLE_ALLOY;
    const CefRect rect(options_.x, options_.y, options_.width, options_.height);
    if (options_.parent_handle) {
      window_info.SetAsChild(static_cast<HWND>(options_.parent_handle), rect);
    } else {
      window_info.SetAsPopup(nullptr, "JSAPI");
      window_info.bounds = rect;
    }
    CefBrowserSettings browser_settings;
    CefBrowserHost::CreateBrowser(window_info, owner_, options_.url,
                                  browser_settings, nullptr, nullptr);
  }

 private:
  CefRefPtr<BrowserHandleImpl> owner_;
  BrowserOptions options_;

  IMPLEMENT_REFCOUNTING(CreateBrowserTask);
};

class BrowserActionTask final : public CefTask {
 public:
  enum class Action { kResize, kClose, kLoadUrl };

  BrowserActionTask(CefRefPtr<CefBrowser> browser, Action action, bool force,
                    CefRect rect = CefRect(), std::string url = std::string())
      : browser_(browser),
        action_(action),
        force_(force),
        rect_(rect),
        url_(std::move(url)) {}

  void Execute() override {
    if (!browser_ || !browser_->IsValid()) {
      return;
    }
    if (action_ == Action::kResize) {
      if (CefWindowHandle handle = browser_->GetHost()->GetWindowHandle()) {
        ::SetWindowPos(handle, nullptr, rect_.x, rect_.y, rect_.width,
                       rect_.height, SWP_NOZORDER | SWP_NOACTIVATE);
      }
      browser_->GetHost()->WasResized();
    } else if (action_ == Action::kClose) {
      browser_->GetHost()->CloseBrowser(force_);
    } else if (!url_.empty()) {
      if (CefRefPtr<CefFrame> frame = browser_->GetMainFrame()) {
        frame->LoadURL(url_);
      }
    }
  }

 private:
  CefRefPtr<CefBrowser> browser_;
  Action action_;
  bool force_;
  CefRect rect_;
  std::string url_;

  IMPLEMENT_REFCOUNTING(BrowserActionTask);
};

}  // namespace

BrowserHandleImpl::BrowserHandleImpl(
    BrowserHost* host,
    std::shared_ptr<jsapi_bridge::JsApiBrowserBridge> bridge,
    const BrowserOptions& options)
    : host_(host),
      bridge_(std::move(bridge)),
      observer_(options.observer),
      options_(options) {
  // 初始区域按物理像素算：控件逻辑尺寸 * 缩放因子。
  options_.x = options.x;
  options_.y = options.y;
  options_.width = static_cast<int>(options.width * options.device_scale_factor);
  options_.height =
      static_cast<int>(options.height * options.device_scale_factor);
}

void BrowserHandleImpl::Start() {
  CefPostTask(TID_UI, new CreateBrowserTask(this, options_));
}

void BrowserHandleImpl::Resize(int x,
                               int y,
                               int width,
                               int height,
                               double device_scale_factor) {
  const CefRect rect(x, y, static_cast<int>(width * device_scale_factor),
                     static_cast<int>(height * device_scale_factor));
  CefRefPtr<CefBrowser> browser;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    browser = browser_;
    if (!browser) {
      // CreateBrowser 是异步的：此刻记下尺寸，OnAfterCreated 时补应用，
      // 否则创建完成前的首次布局会被丢弃，子窗口停在初始矩形上。
      pending_rect_ = rect;
      has_pending_rect_ = true;
      return;
    }
  }
  CefPostTask(TID_UI,
              new BrowserActionTask(browser, BrowserActionTask::Action::kResize,
                                    false, rect));
}

void BrowserHandleImpl::LoadUrl(const std::string& url) {
  CefRefPtr<CefBrowser> browser;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    browser = browser_;
  }
  if (!browser || url.empty()) {
    return;
  }
  CefPostTask(TID_UI, new BrowserActionTask(
                          browser, BrowserActionTask::Action::kLoadUrl, false,
                          CefRect(), url));
}

void BrowserHandleImpl::Close(bool force) {
  CefRefPtr<CefBrowser> browser;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    browser = browser_;
  }
  if (!browser) {
    return;
  }
  CefPostTask(TID_UI,
              new BrowserActionTask(browser, BrowserActionTask::Action::kClose,
                                    force));
}

bool BrowserHandleImpl::valid() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return browser_ != nullptr;
}

bool BrowserHandleImpl::OnProcessMessageReceived(
    CefRefPtr<CefBrowser> browser,
    CefRefPtr<CefFrame> frame,
    CefProcessId source_process,
    CefRefPtr<CefProcessMessage> message) {
  CEF_REQUIRE_UI_THREAD();
  return host_ && host_->OnProcessMessage(browser, frame, source_process,
                                          message);
}

void BrowserHandleImpl::OnAfterCreated(CefRefPtr<CefBrowser> browser) {
  CEF_REQUIRE_UI_THREAD();
  CefRect pending;
  bool apply_pending = false;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    browser_ = browser;
    created_.store(true);
    if (has_pending_rect_) {
      pending = pending_rect_;
      has_pending_rect_ = false;
      apply_pending = true;
    }
  }
  if (apply_pending) {
    if (CefWindowHandle handle = browser->GetHost()->GetWindowHandle()) {
      ::SetWindowPos(handle, nullptr, pending.x, pending.y, pending.width,
                     pending.height, SWP_NOZORDER | SWP_NOACTIVATE);
    }
    browser->GetHost()->WasResized();
  }
  if (host_) {
    host_->OnBrowserCreated(browser);
  }
  browser->GetHost()->SetFocus(true);
  if (observer_) {
    observer_->OnBrowserCreated();
  }
}

void BrowserHandleImpl::OnBeforeClose(CefRefPtr<CefBrowser> browser) {
  CEF_REQUIRE_UI_THREAD();
  if (host_) {
    host_->OnBrowserClosed(browser);
  }
  {
    std::lock_guard<std::mutex> lock(mutex_);
    browser_ = nullptr;
  }
  if (observer_) {
    observer_->OnBeforeClose();
  }
}

void BrowserHandleImpl::OnRenderProcessTerminated(
    CefRefPtr<CefBrowser> browser,
    TerminationStatus /*status*/,
    int /*error_code*/,
    const CefString& /*error_string*/) {
  CEF_REQUIRE_UI_THREAD();
  if (host_) {
    host_->OnRenderTerminated(browser);
  }
  if (observer_) {
    observer_->OnRenderProcessTerminated();
  }
}

}  // namespace cef
}  // namespace jsapi
