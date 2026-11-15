#pragma once

// 私有头：jsapi_cef.dll 内部使用的 CEF 适配实现，不对外安装。

#include <windows.h>

#include <atomic>
#include <memory>
#include <mutex>
#include <string>

#include "include/cef_browser.h"
#include "include/cef_client.h"
#include "include/cef_life_span_handler.h"
#include "include/cef_request_handler.h"
#include "jsapi_cef/runtime.h"

namespace jsapi_bridge {
class JsApiBrowserBridge;
}

namespace jsapi {
namespace cef {

// RuntimeImpl 暴露给浏览器实例的最小回调集，避免 BrowserHandleImpl 反向依赖
// RuntimeImpl 的完整定义。
class BrowserHost {
 public:
  virtual ~BrowserHost() = default;

  // 浏览器创建完成：建立该 browser_id 的同步通道（必须早于渲染进程首次调用）。
  virtual void OnBrowserCreated(CefRefPtr<CefBrowser> browser) = 0;
  virtual void OnBrowserClosed(CefRefPtr<CefBrowser> browser) = 0;
  virtual void OnRenderTerminated(CefRefPtr<CefBrowser> browser) = 0;
  virtual bool OnProcessMessage(CefRefPtr<CefBrowser> browser,
                                CefRefPtr<CefFrame> frame,
                                CefProcessId source_process,
                                CefRefPtr<CefProcessMessage> message) = 0;
};

// 一个浏览器实例对应的 CefClient。浏览器对象与生命周期回调都在 CEF UI 线程，
// 对外操作（改尺寸/关闭/跳转）从任意线程投递到 CEF UI 线程执行。
class BrowserHandleImpl final : public CefClient,
                                public CefLifeSpanHandler,
                                public CefRequestHandler {
 public:
  BrowserHandleImpl(BrowserHost* host,
                    std::shared_ptr<jsapi_bridge::JsApiBrowserBridge> bridge,
                    const BrowserOptions& options);

  // 把浏览器创建投递到 CEF UI 线程。
  void Start();

  void Resize(int x, int y, int width, int height, double device_scale_factor);
  void LoadUrl(const std::string& url);
  void Close(bool force);
  bool valid() const;

  // 引用计数由 CEF 的 IMPLEMENT_REFCOUNTING 管理，析构需要对外可见以便
  // std::shared_ptr / CefRefPtr 在任意模块释放最后一个引用。
  ~BrowserHandleImpl() override = default;

  // CefClient
  CefRefPtr<CefLifeSpanHandler> GetLifeSpanHandler() override { return this; }
  CefRefPtr<CefRequestHandler> GetRequestHandler() override { return this; }
  bool OnProcessMessageReceived(CefRefPtr<CefBrowser> browser,
                                CefRefPtr<CefFrame> frame,
                                CefProcessId source_process,
                                CefRefPtr<CefProcessMessage> message) override;

  // CefLifeSpanHandler
  void OnAfterCreated(CefRefPtr<CefBrowser> browser) override;
  void OnBeforeClose(CefRefPtr<CefBrowser> browser) override;

  // CefRequestHandler
  void OnRenderProcessTerminated(CefRefPtr<CefBrowser> browser,
                                 TerminationStatus status,
                                 int error_code,
                                 const CefString& error_string) override;

 private:
  BrowserHost* host_ = nullptr;
  std::shared_ptr<jsapi_bridge::JsApiBrowserBridge> bridge_;
  std::shared_ptr<BrowserObserver> observer_;
  BrowserOptions options_;

  mutable std::mutex mutex_;
  CefRefPtr<CefBrowser> browser_;
  std::atomic<bool> created_{false};
  // 浏览器异步创建期间收到的尺寸请求，OnAfterCreated 时补应用。
  CefRect pending_rect_;
  bool has_pending_rect_ = false;

  IMPLEMENT_REFCOUNTING(BrowserHandleImpl);
};

}  // namespace cef
}  // namespace jsapi
