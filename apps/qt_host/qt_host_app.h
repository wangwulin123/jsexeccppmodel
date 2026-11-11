#pragma once

#include "include/cef_app.h"
#include "include/cef_render_process_handler.h"

#include "jsapi_bridge/jsapi_render_handler.h"

// CefApp：浏览器进程负责 Qt 窗口与 JSAPI 执行，renderer/GPU 子进程复用同一个 exe。
class QtHostApp final : public CefApp {
 public:
  QtHostApp() = default;

  CefRefPtr<CefRenderProcessHandler> GetRenderProcessHandler() override;

  void OnBeforeCommandLineProcessing(
      const CefString& process_type,
      CefRefPtr<CefCommandLine> command_line) override;

 private:
  CefRefPtr<jsapi_bridge::JsApiRenderHandler> render_handler_;

  IMPLEMENT_REFCOUNTING(QtHostApp);
};
