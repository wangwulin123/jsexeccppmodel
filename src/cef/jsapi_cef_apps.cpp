#include "jsapi_cef_apps.h"

#include "jsapi_render_handler.h"

namespace jsapi_bridge {

namespace {

void ApplyCommonSwitches(CefRefPtr<CefCommandLine> command_line) {
  if (!command_line) {
    return;
  }
  command_line->AppendSwitch("no-sandbox");
}

}  // namespace

void JsApiBrowserApp::OnBeforeCommandLineProcessing(
    const CefString& /*process_type*/,
    CefRefPtr<CefCommandLine> command_line) {
  ApplyCommonSwitches(command_line);
}

CefRefPtr<CefRenderProcessHandler>
JsApiRendererApp::GetRenderProcessHandler() {
  if (!render_handler_) {
    render_handler_ = new JsApiRenderHandler();
  }
  return render_handler_;
}

void JsApiRendererApp::OnBeforeCommandLineProcessing(
    const CefString& /*process_type*/,
    CefRefPtr<CefCommandLine> command_line) {
  ApplyCommonSwitches(command_line);
}

}  // namespace jsapi_bridge
