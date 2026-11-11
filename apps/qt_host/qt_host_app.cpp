#include "qt_host_app.h"

#include "jsapi_bridge/jsapi_render_handler.h"

CefRefPtr<CefRenderProcessHandler> QtHostApp::GetRenderProcessHandler() {
  if (!render_handler_) {
    render_handler_ = new jsapi_bridge::JsApiRenderHandler();
  }
  return render_handler_;
}

void QtHostApp::OnBeforeCommandLineProcessing(
    const CefString& /*process_type*/,
    CefRefPtr<CefCommandLine> command_line) {
  // 示例不启用 CEF 的 Windows 沙箱。
  if (command_line) {
    command_line->AppendSwitch("no-sandbox");
  }
}
