#include "qt_host_client.h"

#include <cstdio>
#include <chrono>

#include "include/cef_task.h"
#include "include/wrapper/cef_helpers.h"
#include "jsapi_bridge/jsapi_browser_bridge.h"

namespace {

// multi_threaded_message_loop 下 CreateBrowser 只能在 CEF UI 线程调用。
class CreateBrowserTask final : public CefTask {
 public:
  CreateBrowserTask(CefRefPtr<CefClient> client,
                    const CefWindowInfo& window_info,
                    const CefBrowserSettings& browser_settings,
                    const std::string& url)
      : client_(client),
        window_info_(window_info),
        browser_settings_(browser_settings),
        url_(url) {}

  void Execute() override {
    CefBrowserHost::CreateBrowser(window_info_, client_, url_, browser_settings_,
                                  nullptr, nullptr);
  }

 private:
  CefRefPtr<CefClient> client_;
  CefWindowInfo window_info_;
  CefBrowserSettings browser_settings_;
  std::string url_;

  IMPLEMENT_REFCOUNTING(CreateBrowserTask);
};

class BrowserActionTask final : public CefTask {
 public:
  enum class Action { kResize, kClose };

  BrowserActionTask(CefRefPtr<CefBrowser> browser, Action action, bool force)
      : browser_(browser), action_(action), force_(force) {}

  void Execute() override {
    if (!browser_ || !browser_->IsValid()) {
      return;
    }
    if (action_ == Action::kResize) {
      browser_->GetHost()->WasResized();
    } else {
      browser_->GetHost()->CloseBrowser(force_);
    }
  }

 private:
  CefRefPtr<CefBrowser> browser_;
  Action action_;
  bool force_;

  IMPLEMENT_REFCOUNTING(BrowserActionTask);
};

}  // namespace

QtHostClient::QtHostClient(
    std::shared_ptr<jsapi_bridge::JsApiBrowserBridge> bridge)
    : bridge_(std::move(bridge)) {}

bool QtHostClient::OnProcessMessageReceived(
    CefRefPtr<CefBrowser> browser,
    CefRefPtr<CefFrame> frame,
    CefProcessId source_process,
    CefRefPtr<CefProcessMessage> message) {
  CEF_REQUIRE_UI_THREAD();
  if (!bridge_) {
    return false;
  }
  return bridge_->OnProcessMessageReceived(browser, frame, source_process,
                                           message);
}

void QtHostClient::OnAfterCreated(CefRefPtr<CefBrowser> browser) {
  CEF_REQUIRE_UI_THREAD();
  {
    std::lock_guard<std::mutex> lock(mutex_);
    browsers_.push_back(browser);
    if (!primary_browser_) {
      primary_browser_ = browser;
    }
  }
  browser->GetHost()->SetFocus(true);
}

void QtHostClient::OnBeforeClose(CefRefPtr<CefBrowser> browser) {
  CEF_REQUIRE_UI_THREAD();
  if (bridge_) {
    bridge_->OnBeforeClose(browser);
  }

  {
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto it = browsers_.begin(); it != browsers_.end(); ++it) {
      if ((*it)->IsSame(browser)) {
        browsers_.erase(it);
        break;
      }
    }
    if (primary_browser_ && primary_browser_->IsSame(browser)) {
      primary_browser_ = browsers_.empty() ? nullptr : browsers_.front();
    }
  }
  closed_cv_.notify_all();
}

void QtHostClient::CreateBrowserAsync(const CefWindowInfo& window_info,
                                     const CefBrowserSettings& browser_settings,
                                     const std::string& url) {
  CefPostTask(TID_UI, new CreateBrowserTask(this, window_info, browser_settings,
                                            url));
}

void QtHostClient::ResizePrimaryBrowser() {
  CefRefPtr<CefBrowser> browser = GetPrimaryBrowser();
  if (!browser) {
    return;
  }
  CefPostTask(TID_UI, new BrowserActionTask(browser,
                                            BrowserActionTask::Action::kResize,
                                            false));
}

void QtHostClient::CloseAllBrowsers(bool force_close) {
  std::vector<CefRefPtr<CefBrowser>> browsers;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    browsers = browsers_;
  }
  for (const auto& browser : browsers) {
    CefPostTask(TID_UI,
                new BrowserActionTask(browser,
                                      BrowserActionTask::Action::kClose,
                                      force_close));
  }
}

bool QtHostClient::WaitForAllClosed(int timeout_ms) {
  std::unique_lock<std::mutex> lock(mutex_);
  return closed_cv_.wait_for(lock, std::chrono::milliseconds(timeout_ms),
                             [this] { return browsers_.empty(); });
}

CefRefPtr<CefBrowser> QtHostClient::GetPrimaryBrowser() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return primary_browser_;
}

bool QtHostClient::HasOpenBrowsers() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return !browsers_.empty();
}
