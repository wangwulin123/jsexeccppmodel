#pragma once

// 私有头：两个进程角色各自的 CefApp 实现，仅 jsapi_cef.dll 内部使用。

#include "include/cef_app.h"
#include "include/cef_render_process_handler.h"

#include "jsapi_render_handler.h"

namespace jsapi_bridge {

// 浏览器进程：只做命令行调整，不提供渲染进程 handler。
class JsApiBrowserApp final : public CefApp {
 public:
  void OnBeforeCommandLineProcessing(
      const CefString& process_type,
      CefRefPtr<CefCommandLine> command_line) override;

 private:
  IMPLEMENT_REFCOUNTING(JsApiBrowserApp);
};

// 渲染进程：只提供 JSAPI 的渲染侧 handler，不参与浏览器进程逻辑。
class JsApiRendererApp final : public CefApp {
 public:
  CefRefPtr<CefRenderProcessHandler> GetRenderProcessHandler() override;

  void OnBeforeCommandLineProcessing(
      const CefString& process_type,
      CefRefPtr<CefCommandLine> command_line) override;

 private:
  CefRefPtr<JsApiRenderHandler> render_handler_;

  IMPLEMENT_REFCOUNTING(JsApiRendererApp);
};

}  // namespace jsapi_bridge
