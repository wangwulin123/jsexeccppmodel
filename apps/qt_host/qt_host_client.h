#pragma once

#include <condition_variable>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "include/cef_browser.h"
#include "include/cef_client.h"
#include "include/cef_life_span_handler.h"

namespace jsapi_bridge {
class JsApiBrowserBridge;
}

// CefClient：把浏览器进程收到的 JSAPI 进程消息转交给 JsApiBrowserBridge。
//
// multi_threaded_message_loop 模式下 CEF 在自己的 UI 线程上跑消息循环，
// 因此浏览器对象、生命周期回调都在 CEF UI 线程，Qt 主线程访问需要加锁，
// 创建/关闭/改尺寸等操作必须投递到 CEF UI 线程执行。
class QtHostClient final : public CefClient, public CefLifeSpanHandler {
 public:
  explicit QtHostClient(std::shared_ptr<jsapi_bridge::JsApiBrowserBridge> bridge);

  // CefClient
  CefRefPtr<CefLifeSpanHandler> GetLifeSpanHandler() override { return this; }

  bool OnProcessMessageReceived(CefRefPtr<CefBrowser> browser,
                                CefRefPtr<CefFrame> frame,
                                CefProcessId source_process,
                                CefRefPtr<CefProcessMessage> message) override;

  // CefLifeSpanHandler
  void OnAfterCreated(CefRefPtr<CefBrowser> browser) override;
  void OnBeforeClose(CefRefPtr<CefBrowser> browser) override;

  // 以下线程安全：可从 Qt 主线程调用，内部投递到 CEF UI 线程。
  void CreateBrowserAsync(const CefWindowInfo& window_info,
                          const CefBrowserSettings& browser_settings,
                          const std::string& url);
  void ResizePrimaryBrowser();
  void CloseAllBrowsers(bool force_close);
  bool WaitForAllClosed(int timeout_ms);

  CefRefPtr<CefBrowser> GetPrimaryBrowser() const;
  bool HasOpenBrowsers() const;

 private:
  std::shared_ptr<jsapi_bridge::JsApiBrowserBridge> bridge_;

  mutable std::mutex mutex_;
  std::condition_variable closed_cv_;
  std::vector<CefRefPtr<CefBrowser>> browsers_;
  CefRefPtr<CefBrowser> primary_browser_;

  IMPLEMENT_REFCOUNTING(QtHostClient);
};
