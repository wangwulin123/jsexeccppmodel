#include "jsapi_bridge/jsapi_render_handler.h"

#include <windows.h>

#include <cstdio>
#include <cstring>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "include/cef_frame.h"
#include "include/cef_process_message.h"
#include "include/cef_v8.h"
#include "jsapi/js_value.h"
#include "jsapi_rpc/jsapi_rpc.h"
#include "jsapi_bridge/jsapi_protocol.h"

namespace jsapi_bridge {
namespace {

using jsapi::JsType;
using jsapi::JsValue;
using jsapi::Param;
using jsapi::RpcEnv;
using jsapi::RpcRequest;
using jsapi::RpcResponse;

struct MemberInfo {
  std::string name;
  bool is_property = false;
  int index = -1;
  JsType type = JsType::Null;
  bool async = false;
  bool writable = false;
};

struct ObjectInfo {
  int handle = 0;
  std::vector<MemberInfo> members;

  const MemberInfo* FindByName(const std::string& name) const {
    for (const auto& member : members) {
      if (member.name == name) return &member;
    }
    return nullptr;
  }
};

struct Binding {
  CefRefPtr<CefV8Context> context;
  CefRefPtr<CefFrame> frame;
  RpcEnv env;
};
using BindingPtr = std::shared_ptr<Binding>;

struct CallbackEntry {
  BindingPtr binding;
  CefRefPtr<CefV8Value> function;
};

struct PendingPromise {
  BindingPtr binding;
  CefRefPtr<CefV8Value> promise;
  int seq = 0;
};

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

std::wstring Utf8ToWide(const std::string& text) {
  if (text.empty()) return {};
  const int size = ::MultiByteToWideChar(CP_UTF8, 0, text.data(),
                                         static_cast<int>(text.size()), nullptr, 0);
  std::wstring wide(static_cast<std::size_t>(size), L'\0');
  ::MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()),
                        wide.data(), size);
  return wide;
}

std::string WideToUtf8(const std::wstring& text) {
  if (text.empty()) return {};
  const int size = ::WideCharToMultiByte(CP_UTF8, 0, text.data(),
                                         static_cast<int>(text.size()), nullptr,
                                         0, nullptr, nullptr);
  std::string out(static_cast<std::size_t>(size), '\0');
  ::WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()),
                        out.data(), size, nullptr, nullptr);
  return out;
}

bool ParseObjectInfo(const JsValue& value, ObjectInfo* info, std::string* error) {
  if (value.type() != JsType::Object) {
    if (error) *error = "describe result is not an object";
    return false;
  }
  const JsValue* handle = value.Find("handle");
  const JsValue* members = value.Find("members");
  if (!handle || !members || members->type() != JsType::Array) {
    if (error) *error = "describe result missing handle/members";
    return false;
  }

  info->handle = static_cast<int>(handle->AsInt());
  info->members.clear();
  for (const auto& item : members->AsArray()) {
    MemberInfo member;
    if (const JsValue* name = item.Find("name"); name) {
      member.name = name->AsString();
    }
    if (const JsValue* kind = item.Find("kind"); kind) {
      member.is_property = kind->AsString() == "property";
    }
    if (const JsValue* index = item.Find("index"); index) {
      member.index = static_cast<int>(index->AsInt(-1));
    }
    if (const JsValue* type = item.Find("type"); type) {
      member.type = jsapi::TypeFromName(type->AsString());
    }
    if (const JsValue* async = item.Find("async"); async) {
      member.async = async->AsBool();
    }
    if (const JsValue* writable = item.Find("writable"); writable) {
      member.writable = writable->AsBool();
    }
    info->members.push_back(std::move(member));
  }
  return true;
}

bool ParseResponse(const std::string& json,
                   RpcResponse* response,
                   std::string* error) {
  JsValue root;
  if (!jsapi::ParseJson(json, &root, error)) {
    return false;
  }
  if (root.type() != JsType::Object) {
    if (error) *error = "response is not an object";
    return false;
  }
  if (const JsValue* seq = root.Find("seq"); seq) {
    response->seq = static_cast<int>(seq->AsInt());
  }
  const JsValue* ok = root.Find("ok");
  if (!ok) {
    if (error) *error = "response missing ok";
    return false;
  }
  response->ok = ok->AsBool();
  if (response->ok) {
    if (const JsValue* value = root.Find("value"); value) {
      response->value = *value;
    } else {
      response->value = JsValue::Null();
    }
  } else {
    if (const JsValue* err = root.Find("error"); err) {
      response->error = err->AsString();
    } else {
      response->error = "unknown browser-side error";
    }
  }
  return true;
}

// 同步 RPC 通道（渲染进程侧）。
//
// 渲染进程在主线程上阻塞等待浏览器进程回包，这期间无法处理 CEF 消息循环，
// 所以请求和回包都走共享内存 + 命名事件。每个渲染进程一个回包通道。
class SyncChannel {
 public:
  SyncChannel()
      : map_name_(ReplyMapName(::GetCurrentProcessId())),
        event_name_(ReplyEventName(::GetCurrentProcessId())) {}

  ~SyncChannel() {
    if (view_) {
      ::UnmapViewOfFile(view_);
    }
    if (event_) {
      ::CloseHandle(event_);
    }
    if (map_) {
      ::CloseHandle(map_);
    }
    if (request_view_) {
      ::UnmapViewOfFile(request_view_);
    }
    if (request_event_) {
      ::CloseHandle(request_event_);
    }
    if (request_mutex_) {
      ::CloseHandle(request_mutex_);
    }
    if (request_map_) {
      ::CloseHandle(request_map_);
    }
  }

  std::string map_name_utf8() const { return WideToUtf8(map_name_); }
  std::string event_name_utf8() const { return WideToUtf8(event_name_); }

  bool Send(CefRefPtr<CefFrame> frame,
            const std::string& payload,
            int seq,
            std::string* response,
            std::string* error) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!Ensure(error)) {
      return false;
    }

    auto* header = static_cast<SharedReplyHeader*>(view_);
    header->ready = 0;
    header->seq = 0;
    header->length = 0;

    if (!frame || !frame->IsValid()) {
      if (error) *error = "failed to send sync request to browser";
      return false;
    }
    if (payload.size() > static_cast<std::size_t>(kRequestCapacity)) {
      if (error) *error = "sync jsapi request exceeds shared-memory capacity";
      return false;
    }

    // 直接写共享内存并唤醒浏览器侧等待线程：
    // 若走进程消息，它会排队到渲染主线程返回消息循环后才发出，而这里马上要阻塞。
    auto* request = static_cast<SharedRequestHeader*>(request_view_);
    if (::WaitForSingleObject(request_mutex_, 5000) != WAIT_OBJECT_0) {
      if (error) *error = "sync jsapi request mutex timeout";
      return false;
    }
    request->seq = static_cast<LONG>(seq);
    request->length = static_cast<LONG>(payload.size());
    std::memcpy(request + 1, payload.data(), payload.size());
    ::ReleaseMutex(request_mutex_);
    ::SetEvent(request_event_);

    const DWORD wait = ::WaitForSingleObject(event_, 10000);
    if (wait != WAIT_OBJECT_0) {
      if (error) *error = "sync jsapi call timed out";
      return false;
    }

    if (header->ready != 1) {
      if (error) *error = "sync jsapi reply not ready";
      return false;
    }
    if (header->seq != seq) {
      if (error) *error = "sync jsapi reply seq mismatch";
      return false;
    }

    LONG length = header->length;
    if (length < 0) length = 0;
    if (length > kReplyCapacity) length = kReplyCapacity;
    response->assign(reinterpret_cast<const char*>(header + 1),
                     static_cast<std::size_t>(length));
    return true;
  }

 private:
  bool Ensure(std::string* error) {
    if (view_ && event_) {
      return true;
    }
    map_ = ::CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0,
                                kReplyBufferBytes, map_name_.c_str());
    if (!map_) {
      if (error) {
        *error = "CreateFileMapping failed, error=" +
                 std::to_string(::GetLastError());
      }
      return false;
    }
    view_ = ::MapViewOfFile(map_, FILE_MAP_WRITE, 0, 0, 0);
    if (!view_) {
      if (error) {
        *error = "MapViewOfFile failed, error=" +
                 std::to_string(::GetLastError());
      }
      return false;
    }
    event_ = ::CreateEventW(nullptr, FALSE, FALSE, event_name_.c_str());
    if (!event_) {
      if (error) {
        *error = "CreateEvent failed, error=" +
                 std::to_string(::GetLastError());
      }
      return false;
    }

    request_map_ = ::CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr,
                                        PAGE_READWRITE, 0, kRequestBufferBytes,
                                        RequestMapName().c_str());
    request_event_ = ::CreateEventW(nullptr, FALSE, FALSE,
                                    RequestEventName().c_str());
    request_mutex_ = ::CreateMutexW(nullptr, FALSE, RequestMutexName().c_str());
    if (request_map_ && request_event_ && request_mutex_) {
      request_view_ = ::MapViewOfFile(request_map_, FILE_MAP_WRITE, 0, 0, 0);
    }
    if (!request_view_) {
      if (error) {
        *error = "sync request channel init failed, error=" +
                 std::to_string(::GetLastError());
      }
      return false;
    }
    return true;
  }

  std::wstring map_name_;
  std::wstring event_name_;
  HANDLE map_ = nullptr;
  HANDLE event_ = nullptr;
  void* view_ = nullptr;

  HANDLE request_map_ = nullptr;
  HANDLE request_event_ = nullptr;
  HANDLE request_mutex_ = nullptr;
  void* request_view_ = nullptr;

  std::mutex mutex_;
};

}  // namespace

class JsApiAccessor;
class JsApiFunctionHandler;

class RendererCore {
 public:
  bool BuildRootObject(CefRefPtr<CefFrame> frame, CefRefPtr<CefV8Context> context);

  CefRefPtr<CefV8Value> BuildProxy(const std::shared_ptr<const ObjectInfo>& info,
                                   const BindingPtr& binding,
                                   int depth);
  CefRefPtr<CefV8Value> ResolveHandle(int handle,
                                      const BindingPtr& binding,
                                      int depth);

  bool ToV8(const JsValue& value,
            const BindingPtr& binding,
            CefRefPtr<CefV8Value>* out,
            int depth);
  bool FromV8(const CefRefPtr<CefV8Value>& value,
              const BindingPtr& binding,
              JsValue* out,
              std::string* error);

  bool InvokeSync(const RpcRequest& request,
                  const BindingPtr& binding,
                  RpcResponse* response,
                  std::string* error);
  void InvokeAsync(const RpcRequest& request,
                   const BindingPtr& binding,
                   CefRefPtr<CefV8Value> promise);

  int RegisterCallback(const BindingPtr& binding,
                       CefRefPtr<CefV8Value> function);
  int FindCallbackId(const BindingPtr& binding,
                     const CefRefPtr<CefV8Value>& function);

  void HandleAsyncResponse(CefRefPtr<CefProcessMessage> message);
  void HandleEvent(CefRefPtr<CefProcessMessage> message);
  void ReleaseContext(const CefRefPtr<CefV8Context>& context);

 private:
  bool DescribeHandle(int handle,
                      const BindingPtr& binding,
                      std::shared_ptr<ObjectInfo>* info,
                      std::string* error);

  int NextSeq();
  int NextCallbackId();

  SyncChannel sync_;
  std::mutex seq_mutex_;
  int next_seq_ = 1;
  int next_callback_ = 1;

  std::map<int, PendingPromise> pending_;
  std::map<int, CallbackEntry> callbacks_;
};

int RendererCore::NextSeq() {
  std::lock_guard<std::mutex> lock(seq_mutex_);
  return next_seq_++;
}

int RendererCore::NextCallbackId() {
  std::lock_guard<std::mutex> lock(seq_mutex_);
  return next_callback_++;
}

bool RendererCore::InvokeSync(const RpcRequest& request,
                              const BindingPtr& binding,
                              RpcResponse* response,
                              std::string* error) {
  RpcRequest copy = request;
  copy.env = binding->env;
  copy.seq = NextSeq();
  copy.async = false;

  std::map<std::string, JsValue> extras;
  extras["reply_map"] = JsValue::String(sync_.map_name_utf8());
  extras["reply_event"] = JsValue::String(sync_.event_name_utf8());

  const std::string payload = jsapi::RpcHost::EncodeRequest(copy, extras);
  std::string reply;
  if (!sync_.Send(binding->frame, payload, copy.seq, &reply, error)) {
    return false;
  }

  RpcResponse parsed;
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

void RendererCore::InvokeAsync(const RpcRequest& request,
                               const BindingPtr& binding,
                               CefRefPtr<CefV8Value> promise) {
  RpcRequest copy = request;
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
  RpcRequest request;
  request.op = "describe";
  request.handle = handle;

  RpcResponse response;
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

bool RendererCore::ToV8(const JsValue& value,
                        const BindingPtr& binding,
                        CefRefPtr<CefV8Value>* out,
                        int depth) {
  switch (value.type()) {
    case JsType::Null:
      *out = CefV8Value::CreateNull();
      return true;
    case JsType::Bool:
      *out = CefV8Value::CreateBool(value.AsBool());
      return true;
    case JsType::Int:
      *out = CefV8Value::CreateInt(static_cast<int>(value.AsInt()));
      return true;
    case JsType::Double:
      *out = CefV8Value::CreateDouble(value.AsDouble());
      return true;
    case JsType::String:
      *out = CefV8Value::CreateString(value.AsString());
      return true;
    case JsType::Array: {
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
    case JsType::Object: {
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
    case JsType::ObjectHandle:
      *out = ResolveHandle(value.AsHandle(), binding, depth);
      return true;
    default:
      *out = CefV8Value::CreateNull();
      return true;
  }
}

bool RendererCore::FromV8(const CefRefPtr<CefV8Value>& value,
                          const BindingPtr& binding,
                          JsValue* out,
                          std::string* error) {
  if (!value || value->IsUndefined() || value->IsNull()) {
    *out = JsValue::Null();
    return true;
  }
  if (value->IsBool()) {
    *out = JsValue::Bool(value->GetBoolValue());
    return true;
  }
  if (value->IsInt()) {
    *out = JsValue::Int(value->GetIntValue());
    return true;
  }
  if (value->IsDouble()) {
    *out = JsValue::Double(value->GetDoubleValue());
    return true;
  }
  if (value->IsString()) {
    *out = JsValue::String(value->GetStringValue().ToString());
    return true;
  }
  if (value->IsFunction()) {
    // 同一个 JS 函数对象复用同一个回调句柄，否则 removeEventListener
    // 传入的新句柄匹配不到已注册的监听。
    int id = FindCallbackId(binding, value);
    if (id < 0) {
      id = RegisterCallback(binding, value);
    }
    *out = JsValue::CallbackHandle(id);
    return true;
  }
  if (value->IsArray()) {
    std::vector<JsValue> items;
    const int length = value->GetArrayLength();
    for (int i = 0; i < length; ++i) {
      JsValue item;
      if (!FromV8(value->GetValue(i), binding, &item, error)) {
        return false;
      }
      items.push_back(std::move(item));
    }
    *out = JsValue::Array(std::move(items));
    return true;
  }
  if (value->IsObject()) {
    std::map<std::string, JsValue> fields;
    std::vector<CefString> keys;
    value->GetKeys(keys);
    for (const auto& key : keys) {
      JsValue item;
      if (!FromV8(value->GetValue(key), binding, &item, error)) {
        return false;
      }
      fields[key.ToString()] = std::move(item);
    }
    *out = JsValue::Object(std::move(fields));
    return true;
  }

  // Date / Promise / ArrayBuffer 等复杂类型暂不支持传参。
  *out = JsValue::Null();
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

  JsValue root;
  std::string error;
  if (!jsapi::ParseJson(args->GetString(0).ToString(), &root, &error)) {
    std::fprintf(stderr, "jsapi: bad async response: %s\n", error.c_str());
    return;
  }

  const JsValue* seq_value = root.Find("seq");
  if (!seq_value) return;
  const int seq = static_cast<int>(seq_value->AsInt());
  auto it = pending_.find(seq);
  if (it == pending_.end()) return;

  PendingPromise pending = std::move(it->second);
  pending_.erase(it);

  if (!pending.binding->context->Enter()) {
    return;
  }

  const JsValue* ok = root.Find("ok");
  if (ok && ok->AsBool()) {
    CefRefPtr<CefV8Value> value;
    const JsValue* result = root.Find("value");
    JsValue empty = JsValue::Null();
    ToV8(result ? *result : empty, pending.binding, &value, 0);
    pending.promise->ResolvePromise(value ? value : CefV8Value::CreateNull());
  } else {
    std::string reason = "jsapi async call failed";
    if (const JsValue* err = root.Find("error"); err) {
      reason = err->AsString();
    }
    pending.promise->RejectPromise(reason);
  }
  pending.binding->context->Exit();
}

void RendererCore::HandleEvent(CefRefPtr<CefProcessMessage> message) {
  auto args = message->GetArgumentList();
  if (!args || args->GetSize() < 1) return;

  JsValue root;
  std::string error;
  if (!jsapi::ParseJson(args->GetString(0).ToString(), &root, &error)) {
    std::fprintf(stderr, "jsapi: bad event payload: %s\n", error.c_str());
    return;
  }

  const JsValue* name = root.Find("name");
  const JsValue* callback = root.Find("cb");
  const JsValue* event_args = root.Find("args");
  if (!name || !callback) return;

  auto it = callbacks_.find(static_cast<int>(callback->AsInt()));
  if (it == callbacks_.end()) return;
  const CallbackEntry entry = it->second;

  if (!entry.binding->context->Enter()) {
    return;
  }

  CefV8ValueList v8_args;
  v8_args.push_back(CefV8Value::CreateString(name->AsString()));
  if (event_args) {
    for (const auto& item : event_args->AsArray()) {
      CefRefPtr<CefV8Value> converted;
      ToV8(item, entry.binding, &converted, 0);
      if (converted) {
        v8_args.push_back(converted);
      }
    }
  }
  entry.function->ExecuteFunction(nullptr, v8_args);
  entry.binding->context->Exit();
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

    RpcRequest request;
    request.op = "get";
    request.handle = info_->handle;
    request.index = member->index;

    RpcResponse response;
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

    JsValue converted;
    std::string error;
    if (!core_->FromV8(value, binding_, &converted, &error)) {
      exception = error;
      return true;
    }

    RpcRequest request;
    request.op = "set";
    request.handle = info_->handle;
    request.index = member->index;
    request.args = {converted};

    RpcResponse response;
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

    RpcRequest request;
    request.op = "execute";
    request.handle = info_->handle;
    request.index = member_.index;

    std::string error;
    for (const auto& argument : arguments) {
      JsValue converted;
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

    RpcResponse response;
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

JsApiRenderHandler::JsApiRenderHandler() : impl_(std::make_shared<RendererCore>()) {}

JsApiRenderHandler::~JsApiRenderHandler() = default;

void JsApiRenderHandler::OnContextCreated(CefRefPtr<CefBrowser> /*browser*/,
                                          CefRefPtr<CefFrame> frame,
                                          CefRefPtr<CefV8Context> context) {
  if (!frame || !frame->IsMain()) {
    return;
  }
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
