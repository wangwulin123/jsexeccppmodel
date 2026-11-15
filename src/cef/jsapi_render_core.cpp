#include "jsapi_render_core.h"

#include <windows.h>

#include <cstdio>
#include <functional>
#include <utility>

#include "include/cef_task.h"
#include "include/cef_v8.h"
#include "jsapi/js_value.h"
#include "jsapi_channel/jsapi_protocol.h"
#include "jsapi_channel/jsapi_sync_channel.h"

namespace jsapi_bridge {
namespace {

bool ParseObjectInfo(const jsapi::JsValue& value,
                     ObjectInfo* info,
                     std::string* error) {
  if (value.type() != jsapi::JsType::Object) {
    if (error) *error = "describe result is not an object";
    return false;
  }
  const jsapi::JsValue* handle = value.Find("handle");
  const jsapi::JsValue* members = value.Find("members");
  if (!handle || !members || members->type() != jsapi::JsType::Array) {
    if (error) *error = "describe result missing handle/members";
    return false;
  }

  info->handle = static_cast<int>(handle->AsInt());
  info->members.clear();
  for (const auto& item : members->AsArray()) {
    MemberInfo member;
    if (const jsapi::JsValue* name = item.Find("name"); name) {
      member.name = name->AsString();
    }
    if (const jsapi::JsValue* kind = item.Find("kind"); kind) {
      member.is_property = kind->AsString() == "property";
    }
    if (const jsapi::JsValue* index = item.Find("index"); index) {
      member.index = static_cast<int>(index->AsInt(-1));
    }
    if (const jsapi::JsValue* type = item.Find("type"); type) {
      member.type = jsapi::TypeFromName(type->AsString());
    }
    if (const jsapi::JsValue* async = item.Find("async"); async) {
      member.async = async->AsBool();
    }
    if (const jsapi::JsValue* writable = item.Find("writable"); writable) {
      member.writable = writable->AsBool();
    }
    info->members.push_back(std::move(member));
  }
  return true;
}

bool ParseResponse(const std::string& json,
                   jsapi::RpcResponse* response,
                   std::string* error) {
  jsapi::JsValue root;
  if (!jsapi::ParseJson(json, &root, error)) {
    return false;
  }
  if (root.type() != jsapi::JsType::Object) {
    if (error) *error = "response is not an object";
    return false;
  }
  if (const jsapi::JsValue* seq = root.Find("seq"); seq) {
    response->seq = static_cast<int>(seq->AsInt());
  }
  const jsapi::JsValue* ok = root.Find("ok");
  if (!ok) {
    if (error) *error = "response missing ok";
    return false;
  }
  response->ok = ok->AsBool();
  if (response->ok) {
    if (const jsapi::JsValue* value = root.Find("value"); value) {
      response->value = *value;
    } else {
      response->value = jsapi::JsValue::Null();
    }
  } else {
    if (const jsapi::JsValue* err = root.Find("error"); err) {
      response->error = err->AsString();
    } else {
      response->error = "unknown browser-side error";
    }
  }
  return true;
}

// 把任意回调投递到渲染主线程（watcher 线程不能直接碰 V8）。
class FunctionTask final : public CefTask {
 public:
  explicit FunctionTask(std::function<void()> function)
      : function_(std::move(function)) {}

  void Execute() override {
    if (function_) {
      function_();
    }
  }

 private:
  std::function<void()> function_;

  IMPLEMENT_REFCOUNTING(FunctionTask);
};

}  // namespace

RendererCore::RendererCore() = default;

RendererCore::~RendererCore() {
  alive_->store(false);
  std::vector<std::shared_ptr<SyncChannel>> channels;
  {
    std::lock_guard<std::mutex> lock(channels_mutex_);
    for (auto& entry : channels_) {
      channels.push_back(std::move(entry.second));
    }
    channels_.clear();
  }
  for (auto& channel : channels) {
    channel->Stop();
  }
}

std::shared_ptr<SyncChannel> RendererCore::EnsureChannel(
    CefRefPtr<CefFrame> frame) {
  if (!frame) {
    return nullptr;
  }
  auto browser = frame->GetBrowser();
  const int browser_id = browser ? browser->GetIdentifier() : 0;
  if (browser_id <= 0) {
    return nullptr;
  }

  std::lock_guard<std::mutex> lock(channels_mutex_);
  auto it = channels_.find(browser_id);
  if (it != channels_.end()) {
    return it->second;
  }

  auto channel = std::make_shared<SyncChannel>();
  channel->SetInlineDispatch([this](const std::string& name, int callback_id,
                                    const std::vector<jsapi::JsValue>& args,
                                    jsapi::JsValue* result, std::string* error) {
    return DispatchEvent(name, callback_id, args, result, error);
  });
  channel->SetPostDispatch(
      [this, alive = alive_, raw = channel.get()](const std::string& payload) {
        // 第四路：watcher 判定当前没有阻塞中的同步调用，直接投递主线程异步派发。
        CefPostTask(TID_RENDERER, new FunctionTask([this, alive, raw, payload] {
                      if (alive->load()) {
                        raw->DispatchPosted(payload);
                      }
                    }));
      });

  std::string error;
  if (!channel->Start(static_cast<unsigned long>(browser_id), &error)) {
    std::fprintf(stderr, "jsapi: sync channel init failed, browser=%d: %s\n",
                 browser_id, error.c_str());
    return nullptr;
  }
  channels_[browser_id] = channel;
  return channel;
}

int RendererCore::NextSeq() {
  std::lock_guard<std::mutex> lock(seq_mutex_);
  if (next_seq_ == 0) {
    // 渲染进程重启后编号从 1 重新开始，会与上一个进程迟到/残留的回包撞号。
    // 用时间戳播种，保证同一通道上跨进程的 seq 基本不重复。
    next_seq_ = static_cast<int>((::GetTickCount64() & 0x3fffffff) + 1);
    if (next_seq_ <= 0) next_seq_ = 1;
  }
  return next_seq_++;
}

int RendererCore::NextCallbackId() {
  std::lock_guard<std::mutex> lock(seq_mutex_);
  return next_callback_++;
}

bool RendererCore::IsCurrentContext(const CefRefPtr<CefV8Context>& context) {
  if (!context) {
    return false;
  }
  auto current = CefV8Context::GetCurrentContext();
  return current && current->IsSame(context);
}

bool RendererCore::InvokeSync(const jsapi::RpcRequest& request,
                              const BindingPtr& binding,
                              jsapi::RpcResponse* response,
                              std::string* error) {
  jsapi::RpcRequest copy = request;
  copy.env = binding->env;
  copy.seq = NextSeq();
  copy.async = false;

  const std::string payload = jsapi::RpcHost::EncodeRequest(copy);
  std::string reply;
  if (!binding->channel) {
    if (error) *error = "sync channel not available";
    return false;
  }
  if (!binding->channel->Send(payload, copy.seq, &reply, error)) {
    return false;
  }

  jsapi::RpcResponse parsed;
  if (!ParseResponse(reply, &parsed, error)) {
    return false;
  }
  if (parsed.seq != copy.seq) {
    if (error) *error = "sync jsapi response seq mismatch";
    return false;
  }
  *response = std::move(parsed);
  return true;
}

void RendererCore::InvokeAsync(const jsapi::RpcRequest& request,
                               const BindingPtr& binding,
                               CefRefPtr<CefV8Value> promise) {
  jsapi::RpcRequest copy = request;
  copy.env = binding->env;
  copy.async = true;
  copy.seq = NextSeq();

  PendingPromise pending;
  pending.binding = binding;
  pending.promise = std::move(promise);
  pending.seq = copy.seq;
  pending_[copy.seq] = std::move(pending);

  const std::string payload = jsapi::RpcHost::EncodeRequest(copy);
  auto message = CefProcessMessage::Create(kAsyncRequestMessage);
  message->GetArgumentList()->SetString(0, payload);
  if (!binding->frame || !binding->frame->IsValid()) {
    pending_.erase(copy.seq);
    std::fprintf(stderr, "jsapi: failed to send async request\n");
    return;
  }
  binding->frame->SendProcessMessage(PID_BROWSER, message);
}

bool RendererCore::DescribeHandle(int handle,
                                  const BindingPtr& binding,
                                  std::shared_ptr<ObjectInfo>* info,
                                  std::string* error) {
  jsapi::RpcRequest request;
  request.op = "describe";
  request.handle = handle;

  jsapi::RpcResponse response;
  if (!InvokeSync(request, binding, &response, error)) {
    return false;
  }
  if (!response.ok) {
    if (error) *error = response.error;
    return false;
  }

  auto parsed = std::make_shared<ObjectInfo>();
  if (!ParseObjectInfo(response.value, parsed.get(), error)) {
    return false;
  }
  *info = std::move(parsed);
  return true;
}

CefRefPtr<CefV8Value> RendererCore::ResolveHandle(int handle,
                                                  const BindingPtr& binding,
                                                  int depth) {
  if (depth > 6) {
    return CefV8Value::CreateNull();
  }
  std::shared_ptr<ObjectInfo> info;
  std::string error;
  if (!DescribeHandle(handle, binding, &info, &error)) {
    std::fprintf(stderr, "jsapi: describe handle %d failed: %s\n", handle,
                 error.c_str());
    return CefV8Value::CreateNull();
  }
  return BuildProxy(info, binding, depth + 1);
}

bool RendererCore::BuildRootObject(CefRefPtr<CefFrame> frame,
                                   CefRefPtr<CefV8Context> context) {
  BindingPtr binding = std::make_shared<Binding>();
  binding->context = context;
  binding->frame = frame;
  binding->env.browser_id = frame ? frame->GetBrowser()->GetIdentifier() : -1;
  binding->env.frame_id = frame ? frame->GetIdentifier().ToString() : "";
  binding->channel = EnsureChannel(frame);

  std::shared_ptr<ObjectInfo> info;
  std::string error;
  if (!DescribeHandle(0, binding, &info, &error)) {
    std::fprintf(stderr, "jsapi: root describe failed: %s\n", error.c_str());
    return false;
  }

  auto proxy = BuildProxy(info, binding, 0);
  if (!proxy) {
    return false;
  }
  context->GetGlobal()->SetValue(
      "Extention", proxy,
      static_cast<CefV8Value::PropertyAttribute>(
          V8_PROPERTY_ATTRIBUTE_READONLY | V8_PROPERTY_ATTRIBUTE_DONTENUM |
          V8_PROPERTY_ATTRIBUTE_DONTDELETE));
  return true;
}

bool RendererCore::ToV8(const jsapi::JsValue& value,
                        const BindingPtr& binding,
                        CefRefPtr<CefV8Value>* out,
                        int depth) {
  switch (value.type()) {
    case jsapi::JsType::Null:
      *out = CefV8Value::CreateNull();
      return true;
    case jsapi::JsType::Bool:
      *out = CefV8Value::CreateBool(value.AsBool());
      return true;
    case jsapi::JsType::Int:
      *out = CefV8Value::CreateInt(static_cast<int>(value.AsInt()));
      return true;
    case jsapi::JsType::Double:
      *out = CefV8Value::CreateDouble(value.AsDouble());
      return true;
    case jsapi::JsType::String:
      *out = CefV8Value::CreateString(value.AsString());
      return true;
    case jsapi::JsType::Array: {
      auto array = CefV8Value::CreateArray(
          static_cast<int>(value.AsArray().size()));
      int index = 0;
      for (const auto& item : value.AsArray()) {
        CefRefPtr<CefV8Value> converted;
        if (ToV8(item, binding, &converted, depth + 1)) {
          array->SetValue(index, converted);
        }
        ++index;
      }
      *out = array;
      return true;
    }
    case jsapi::JsType::Object: {
      auto object = CefV8Value::CreateObject(nullptr, nullptr);
      for (const auto& item : value.AsObject()) {
        CefRefPtr<CefV8Value> converted;
        if (ToV8(item.second, binding, &converted, depth + 1)) {
          object->SetValue(item.first, converted,
                           static_cast<CefV8Value::PropertyAttribute>(0));
        }
      }
      *out = object;
      return true;
    }
    case jsapi::JsType::ObjectHandle:
      *out = ResolveHandle(value.AsHandle(), binding, depth);
      return true;
    default:
      *out = CefV8Value::CreateNull();
      return true;
  }
}

bool RendererCore::FromV8(const CefRefPtr<CefV8Value>& value,
                          const BindingPtr& binding,
                          jsapi::JsValue* out,
                          std::string* error) {
  if (!value || value->IsUndefined() || value->IsNull()) {
    *out = jsapi::JsValue::Null();
    return true;
  }
  if (value->IsBool()) {
    *out = jsapi::JsValue::Bool(value->GetBoolValue());
    return true;
  }
  if (value->IsInt()) {
    *out = jsapi::JsValue::Int(value->GetIntValue());
    return true;
  }
  if (value->IsDouble()) {
    *out = jsapi::JsValue::Double(value->GetDoubleValue());
    return true;
  }
  if (value->IsString()) {
    *out = jsapi::JsValue::String(value->GetStringValue().ToString());
    return true;
  }
  if (value->IsFunction()) {
    // 同一个 JS 函数对象复用同一个回调句柄，否则 removeEventListener
    // 传入的新句柄匹配不到已注册的监听。
    int id = FindCallbackId(binding, value);
    if (id < 0) {
      id = RegisterCallback(binding, value);
    }
    *out = jsapi::JsValue::CallbackHandle(id);
    return true;
  }
  if (value->IsArray()) {
    std::vector<jsapi::JsValue> items;
    const int length = value->GetArrayLength();
    for (int i = 0; i < length; ++i) {
      jsapi::JsValue item;
      if (!FromV8(value->GetValue(i), binding, &item, error)) {
        return false;
      }
      items.push_back(std::move(item));
    }
    *out = jsapi::JsValue::Array(std::move(items));
    return true;
  }
  if (value->IsObject()) {
    std::map<std::string, jsapi::JsValue> fields;
    std::vector<CefString> keys;
    value->GetKeys(keys);
    for (const auto& key : keys) {
      jsapi::JsValue item;
      if (!FromV8(value->GetValue(key), binding, &item, error)) {
        return false;
      }
      fields[key.ToString()] = std::move(item);
    }
    *out = jsapi::JsValue::Object(std::move(fields));
    return true;
  }

  // Date / Promise / ArrayBuffer 等复杂类型暂不支持传参。
  *out = jsapi::JsValue::Null();
  if (error) *error = "unsupported js value type for jsapi argument";
  return true;
}

int RendererCore::RegisterCallback(const BindingPtr& binding,
                                   CefRefPtr<CefV8Value> function) {
  const int id = NextCallbackId();
  callbacks_[id] = CallbackEntry{binding, std::move(function)};
  return id;
}

int RendererCore::FindCallbackId(const BindingPtr& binding,
                                 const CefRefPtr<CefV8Value>& function) {
  for (const auto& entry : callbacks_) {
    if (entry.second.binding->context->IsSame(binding->context) &&
        entry.second.function->IsSame(function)) {
      return entry.first;
    }
  }
  return -1;
}

void RendererCore::HandleAsyncResponse(CefRefPtr<CefProcessMessage> message) {
  auto args = message->GetArgumentList();
  if (!args || args->GetSize() < 1) return;

  jsapi::JsValue root;
  std::string error;
  if (!jsapi::ParseJson(args->GetString(0).ToString(), &root, &error)) {
    std::fprintf(stderr, "jsapi: bad async response: %s\n", error.c_str());
    return;
  }

  const jsapi::JsValue* seq_value = root.Find("seq");
  if (!seq_value) return;
  const int seq = static_cast<int>(seq_value->AsInt());
  auto it = pending_.find(seq);
  if (it == pending_.end()) {
    // 超时/崩溃后迟到的回执：没有对应的挂起调用，直接丢弃。
    return;
  }

  PendingPromise pending = std::move(it->second);
  pending_.erase(it);

  const bool need_enter = !IsCurrentContext(pending.binding->context);
  if (need_enter && !pending.binding->context->Enter()) {
    return;
  }

  const jsapi::JsValue* ok = root.Find("ok");
  if (ok && ok->AsBool()) {
    CefRefPtr<CefV8Value> value;
    const jsapi::JsValue* result = root.Find("value");
    jsapi::JsValue empty = jsapi::JsValue::Null();
    ToV8(result ? *result : empty, pending.binding, &value, 0);
    pending.promise->ResolvePromise(value ? value : CefV8Value::CreateNull());
  } else {
    std::string reason = "jsapi async call failed";
    if (const jsapi::JsValue* err = root.Find("error"); err) {
      reason = err->AsString();
    }
    pending.promise->RejectPromise(reason);
  }
  if (need_enter) pending.binding->context->Exit();
}

bool RendererCore::DispatchEvent(const std::string& name,
                                 int callback_id,
                                 const std::vector<jsapi::JsValue>& args,
                                 jsapi::JsValue* result,
                                 std::string* error) {
  if (result) *result = jsapi::JsValue::Null();
  auto it = callbacks_.find(callback_id);
  if (it == callbacks_.end()) {
    if (error) *error = "event callback not found";
    return false;
  }
  const CallbackEntry entry = it->second;
  // 已经处于目标上下文（例如同步事件是从该上下文的微任务里发起的）时不要再
  // Enter/Exit：CEF 的 Exit 会触发一次微任务检查点，在微任务内部重入检查点会
  // 让后续微任务不再被调度（表现为页面里 Promise 之后全部挂起）。
  const bool need_enter = !IsCurrentContext(entry.binding->context);
  if (need_enter && !entry.binding->context->Enter()) {
    if (error) *error = "failed to enter event callback context";
    return false;
  }

  CefV8ValueList v8_args;
  v8_args.push_back(CefV8Value::CreateString(name));
  for (const auto& item : args) {
    CefRefPtr<CefV8Value> converted;
    ToV8(item, entry.binding, &converted, 0);
    if (converted) {
      v8_args.push_back(converted);
    }
  }

  bool ok = true;
  CefRefPtr<CefV8Value> retval = entry.function->ExecuteFunction(nullptr, v8_args);
  if (!retval) {
    // 回调抛异常：捕获后转成 error，绝不穿透到外层 C++ 栈。
    std::string message = "event callback threw an exception";
    if (entry.function->HasException()) {
      if (auto exception = entry.function->GetException()) {
        const std::string detail = exception->GetMessage().ToString();
        if (!detail.empty()) {
          message += ": " + detail;
        }
      }
      entry.function->ClearException();
    }
    if (error) *error = message;
    ok = false;
  } else if (result) {
    std::string convert_error;
    FromV8(retval, entry.binding, result, &convert_error);
  }

  if (need_enter) entry.binding->context->Exit();
  return ok;
}

void RendererCore::HandleEvent(CefRefPtr<CefProcessMessage> message) {
  auto args = message->GetArgumentList();
  if (!args || args->GetSize() < 1) return;

  std::string name;
  int callback_id = -1;
  int invoke_id = 0;
  std::vector<jsapi::JsValue> event_args;
  if (!DecodeEventPayload(args->GetString(0).ToString(), &name, &callback_id,
                          &invoke_id, &event_args, nullptr)) {
    std::fprintf(stderr, "jsapi: bad event payload\n");
    return;
  }

  jsapi::JsValue result;
  std::string error;
  DispatchEvent(name, callback_id, event_args, &result, &error);
}

void RendererCore::ReleaseContext(const CefRefPtr<CefV8Context>& context) {
  for (auto it = callbacks_.begin(); it != callbacks_.end();) {
    if (it->second.binding->context->IsSame(context)) {
      it = callbacks_.erase(it);
    } else {
      ++it;
    }
  }
  for (auto it = pending_.begin(); it != pending_.end();) {
    if (it->second.binding->context->IsSame(context)) {
      it = pending_.erase(it);
    } else {
      ++it;
    }
  }
}

}  // namespace jsapi_bridge
