#include "jsapi_render_handler.h"

#include <string>

#include "include/cef_process_message.h"
#include "jsapi_channel/jsapi_protocol.h"
#include "jsapi_render_core.h"

namespace jsapi_bridge {

JsApiRenderHandler::JsApiRenderHandler()
    : impl_(std::make_shared<RendererCore>()) {}

JsApiRenderHandler::~JsApiRenderHandler() = default;

void JsApiRenderHandler::OnContextCreated(CefRefPtr<CefBrowser> /*browser*/,
                                          CefRefPtr<CefFrame> frame,
                                          CefRefPtr<CefV8Context> context) {
  if (!frame || !frame->IsMain()) {
    return;
  }
  // 通道与浏览器进程按 browser_id 对齐，先建通道再建根对象（首个 describe 就是
  // 同步 RPC）。浏览器进程侧在 OnAfterCreated 已经建好同名通道并启动读取线程。
  impl_->EnsureChannel(frame);
  impl_->BuildRootObject(frame, context);
}

void JsApiRenderHandler::OnContextReleased(CefRefPtr<CefBrowser> /*browser*/,
                                           CefRefPtr<CefFrame> /*frame*/,
                                           CefRefPtr<CefV8Context> context) {
  impl_->ReleaseContext(context);
}

bool JsApiRenderHandler::OnProcessMessageReceived(
    CefRefPtr<CefBrowser> /*browser*/,
    CefRefPtr<CefFrame> /*frame*/,
    CefProcessId /*source_process*/,
    CefRefPtr<CefProcessMessage> message) {
  const std::string name = message->GetName().ToString();
  if (name == kAsyncResponseMessage) {
    impl_->HandleAsyncResponse(message);
    return true;
  }
  if (name == kEventMessage) {
    impl_->HandleEvent(message);
    return true;
  }
  return false;
}

}  // namespace jsapi_bridge
