#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "include/cef_process_message.h"
#include "include/cef_v8.h"
#include "jsapi_channel/jsapi_protocol.h"
#include "jsapi_render_core.h"

namespace jsapi_bridge {
namespace {

// 代理被 V8 回收时通知浏览器进程释放临时节点。
// CefV8Value::SetUserData 挂上去的 user data 会随 V8 对象销毁一起释放，
// 正好当作代理的析构钩子；单例句柄由浏览器侧忽略。
class HandleReleaseGuard final : public CefBaseRefCounted {
 public:
  HandleReleaseGuard(CefRefPtr<CefFrame> frame, int handle)
      : frame_(std::move(frame)), handle_(handle) {}

  ~HandleReleaseGuard() override {
    if (!frame_ || !frame_->IsValid()) {
      return;
    }
    auto message = CefProcessMessage::Create(kReleaseObjectMessage);
    message->GetArgumentList()->SetInt(0, handle_);
    frame_->SendProcessMessage(PID_BROWSER, message);
  }

 private:
  CefRefPtr<CefFrame> frame_;
  int handle_ = 0;

  IMPLEMENT_REFCOUNTING(HandleReleaseGuard);
};

// 对象级 accessor：属性名 -> 成员索引 -> 同步 get/set RPC。
class JsApiAccessor final : public CefV8Accessor {
 public:
  JsApiAccessor(RendererCore* core,
                std::shared_ptr<const ObjectInfo> info,
                BindingPtr binding)
      : core_(core), info_(std::move(info)), binding_(std::move(binding)) {}

  bool Get(const CefString& name,
           const CefRefPtr<CefV8Value> /*object*/,
           CefRefPtr<CefV8Value>& retval,
           CefString& exception) override {
    const MemberInfo* member = info_->FindByName(name.ToString());
    if (!member || !member->is_property) return false;

    jsapi::RpcRequest request;
    request.op = "get";
    request.handle = info_->handle;
    request.index = member->index;

    jsapi::RpcResponse response;
    std::string error;
    if (!core_->InvokeSync(request, binding_, &response, &error)) {
      exception = error;
      return true;
    }
    if (!response.ok) {
      exception = response.error;
      return true;
    }

    CefRefPtr<CefV8Value> value;
    core_->ToV8(response.value, binding_, &value, 0);
    retval = value ? value : CefV8Value::CreateNull();
    return true;
  }

  bool Set(const CefString& name,
           const CefRefPtr<CefV8Value> /*object*/,
           const CefRefPtr<CefV8Value> value,
           CefString& exception) override {
    const MemberInfo* member = info_->FindByName(name.ToString());
    if (!member || !member->is_property) return false;

    jsapi::JsValue converted;
    std::string error;
    if (!core_->FromV8(value, binding_, &converted, &error)) {
      exception = error;
      return true;
    }

    jsapi::RpcRequest request;
    request.op = "set";
    request.handle = info_->handle;
    request.index = member->index;
    request.args = {converted};

    jsapi::RpcResponse response;
    if (!core_->InvokeSync(request, binding_, &response, &error)) {
      exception = error;
      return true;
    }
    if (!response.ok) {
      exception = response.error;
      return true;
    }
    return true;
  }

 private:
  RendererCore* core_;
  std::shared_ptr<const ObjectInfo> info_;
  BindingPtr binding_;

  IMPLEMENT_REFCOUNTING(JsApiAccessor);
};

// 函数调用：同步调用阻塞等共享内存回包；声明为 async 时立即返回 Promise。
class JsApiFunctionHandler final : public CefV8Handler {
 public:
  JsApiFunctionHandler(RendererCore* core,
                       std::shared_ptr<const ObjectInfo> info,
                       MemberInfo member,
                       BindingPtr binding)
      : core_(core),
        info_(std::move(info)),
        member_(std::move(member)),
        binding_(std::move(binding)) {}

  bool Execute(const CefString& /*name*/,
               CefRefPtr<CefV8Value> /*object*/,
               const CefV8ValueList& arguments,
               CefRefPtr<CefV8Value>& retval,
               CefString& exception) override {
    if (!binding_->frame || !binding_->frame->IsValid()) {
      exception = "jsapi frame is no longer valid";
      return true;
    }

    jsapi::RpcRequest request;
    request.op = "execute";
    request.handle = info_->handle;
    request.index = member_.index;

    std::string error;
    for (const auto& argument : arguments) {
      jsapi::JsValue converted;
      if (!core_->FromV8(argument, binding_, &converted, &error)) {
        exception = error;
        return true;
      }
      request.args.push_back(std::move(converted));
    }

    if (member_.async) {
      auto promise = CefV8Value::CreatePromise();
      core_->InvokeAsync(request, binding_, promise);
      retval = promise;
      return true;
    }

    jsapi::RpcResponse response;
    if (!core_->InvokeSync(request, binding_, &response, &error)) {
      exception = error;
      return true;
    }
    if (!response.ok) {
      exception = response.error;
      return true;
    }

    CefRefPtr<CefV8Value> value;
    core_->ToV8(response.value, binding_, &value, 0);
    retval = value ? value : CefV8Value::CreateNull();
    return true;
  }

 private:
  RendererCore* core_;
  std::shared_ptr<const ObjectInfo> info_;
  MemberInfo member_;
  BindingPtr binding_;

  IMPLEMENT_REFCOUNTING(JsApiFunctionHandler);
};

}  // namespace

CefRefPtr<CefV8Value> RendererCore::BuildProxy(
    const std::shared_ptr<const ObjectInfo>& info,
    const BindingPtr& binding,
    int depth) {
  if (!info) {
    return nullptr;
  }

  auto accessor = new JsApiAccessor(this, info, binding);
  auto object = CefV8Value::CreateObject(accessor, nullptr);
  if (!object) {
    return nullptr;
  }

  // 根节点 handle 0 常驻，不需要回收通知。
  if (info->handle > 0) {
    object->SetUserData(new HandleReleaseGuard(binding->frame, info->handle));
  }

  for (const auto& member : info->members) {
    if (!member.is_property) {
      auto handler = new JsApiFunctionHandler(this, info, member, binding);
      object->SetValue(member.name,
                       CefV8Value::CreateFunction(member.name, handler),
                       static_cast<CefV8Value::PropertyAttribute>(
                           V8_PROPERTY_ATTRIBUTE_READONLY));
    } else {
      // SetValue(name, attributes) 只是把名字登记到对象级 accessor，
      // 真正的 get/set 由 JsApiAccessor 转发成同步 RPC。
      const auto attributes =
          member.writable
              ? static_cast<CefV8Value::PropertyAttribute>(0)
              : static_cast<CefV8Value::PropertyAttribute>(
                    V8_PROPERTY_ATTRIBUTE_READONLY);
      object->SetValue(member.name, attributes);
    }
  }

  return object;
}

}  // namespace jsapi_bridge
