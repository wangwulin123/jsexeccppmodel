#pragma once

#include <memory>

#include "include/cef_render_process_handler.h"

namespace jsapi_bridge {

class RendererCore;

// 渲染进程侧入口：
//  - OnContextCreated 时向浏览器进程 describe(handle 0)，把 window.Extention 挂到
//    全局对象上；
//  - 方法调用/属性访问转成同步或异步 RPC；
//  - 事件到达时在对应上下文里执行 JS 回调。
class JsApiRenderHandler final : public CefRenderProcessHandler {
 public:
  JsApiRenderHandler();
  ~JsApiRenderHandler() override;

  void OnContextCreated(CefRefPtr<CefBrowser> browser,
                        CefRefPtr<CefFrame> frame,
                        CefRefPtr<CefV8Context> context) override;

  void OnContextReleased(CefRefPtr<CefBrowser> browser,
                         CefRefPtr<CefFrame> frame,
                         CefRefPtr<CefV8Context> context) override;

  bool OnProcessMessageReceived(CefRefPtr<CefBrowser> browser,
                                CefRefPtr<CefFrame> frame,
                                CefProcessId source_process,
                                CefRefPtr<CefProcessMessage> message) override;

 private:
  std::shared_ptr<RendererCore> impl_;

  IMPLEMENT_REFCOUNTING(JsApiRenderHandler);
};

}  // namespace jsapi_bridge
