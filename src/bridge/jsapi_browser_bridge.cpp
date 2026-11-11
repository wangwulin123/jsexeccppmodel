#include "jsapi_bridge/jsapi_browser_bridge.h"

#include <windows.h>

#include <cstdio>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

#include "include/cef_browser.h"
#include "include/cef_frame.h"
#include "include/cef_task.h"
#include "jsapi/js_value.h"
#include "jsapi_bridge/jsapi_protocol.h"

namespace jsapi_bridge {
namespace {

std::wstring Utf8ToWide(const std::string& text) {
  if (text.empty()) return {};
  const int size = ::MultiByteToWideChar(CP_UTF8, 0, text.data(),
                                         static_cast<int>(text.size()), nullptr, 0);
  std::wstring wide(static_cast<std::size_t>(size), L'\0');
  ::MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()),
                        wide.data(), size);
  return wide;
}

jsapi::RpcEnv ResolveEnv(CefRefPtr<CefBrowser> browser,
                         CefRefPtr<CefFrame> frame) {
  jsapi::RpcEnv env;
  if (browser) {
    env.browser_id = browser->GetIdentifier();
  }
  if (frame) {
    env.frame_id = frame->GetIdentifier().ToString();
  }
  return env;
}

// 工作线程里没有 CefFrame 对象，只能按标识符回查，防止渲染进程伪造环境。
jsapi::RpcEnv ResolveEnvById(const jsapi::RpcEnv& claimed) {
  jsapi::RpcEnv env;
  auto browser = CefBrowserHost::GetBrowserByIdentifier(claimed.browser_id);
  if (!browser) {
    return claimed;
  }
  env.browser_id = browser->GetIdentifier();
  auto frame = browser->GetFrameByIdentifier(CefString(claimed.frame_id));
  env.frame_id =
      frame ? frame->GetIdentifier().ToString() : claimed.frame_id;
  return env;
}

// 事件可能由工作线程（同步 RPC 内部 emit）触发，而 SendProcessMessage /
// GetBrowserByIdentifier 需要在 UI 线程上使用，所以统一投递回 UI 线程。
class EventDispatchTask final : public CefTask {
 public:
  EventDispatchTask(jsapi::RpcEnv target,
                    std::string event,
                    int callback_id,
                    std::vector<jsapi::JsValue> args)
      : target_(std::move(target)),
        event_(std::move(event)),
        callback_id_(callback_id),
        args_(std::move(args)) {}

  void Execute() override {
    auto browser = CefBrowserHost::GetBrowserByIdentifier(target_.browser_id);
    if (!browser) {
      return;
    }
    auto frame = browser->GetFrameByIdentifier(CefString(target_.frame_id));
    if (!frame || !frame->IsValid()) {
      return;
    }

    jsapi::JsValue root = jsapi::JsValue::Object();
    root.Set("name", jsapi::JsValue::String(event_));
    root.Set("cb", jsapi::JsValue::Int(callback_id_));
    root.Set("args", jsapi::JsValue::Array(args_));

    auto message = CefProcessMessage::Create(kEventMessage);
    message->GetArgumentList()->SetString(0, root.ToJson());
    frame->SendProcessMessage(PID_RENDERER, message);
  }

 private:
  jsapi::RpcEnv target_;
  std::string event_;
  int callback_id_;
  std::vector<jsapi::JsValue> args_;

  IMPLEMENT_REFCOUNTING(EventDispatchTask);
};

}  // namespace

JsApiBrowserBridge::JsApiBrowserBridge() : rpc_(&registry_) {
  registry_.SetEventSink(this);

  stop_event_ = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
  request_map_ = ::CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr,
                                      PAGE_READWRITE, 0, kRequestBufferBytes,
                                      RequestMapName().c_str());
  if (request_map_) {
    request_view_ = ::MapViewOfFile(request_map_, FILE_MAP_WRITE, 0, 0, 0);
  }
  request_event_ =
      ::CreateEventW(nullptr, FALSE, FALSE, RequestEventName().c_str());
  request_mutex_ = ::CreateMutexW(nullptr, FALSE, RequestMutexName().c_str());

  if (request_view_ && request_event_ && request_mutex_ && stop_event_) {
    worker_ = std::thread(&JsApiBrowserBridge::SyncLoop, this);
  } else {
    std::fprintf(stderr, "jsapi: sync request channel init failed, error=%lu\n",
                 ::GetLastError());
  }
}

void JsApiBrowserBridge::SetMainThreadDispatcher(
    std::function<void(std::function<void()>)> dispatcher) {
  std::lock_guard<std::mutex> lock(dispatcher_mutex_);
  main_thread_dispatcher_ = std::move(dispatcher);
}

void JsApiBrowserBridge::RunOnMainThread(std::function<void()> task) {
  std::function<void(std::function<void()>)> dispatcher;
  {
    std::lock_guard<std::mutex> lock(dispatcher_mutex_);
    dispatcher = main_thread_dispatcher_;
  }
  if (dispatcher) {
    // 约定 dispatcher 同步执行：返回时 task 已经跑完，调用方可安全读取结果。
    dispatcher(std::move(task));
    return;
  }
  task();
}

JsApiBrowserBridge::~JsApiBrowserBridge() {
  stopping_.store(true);
  registry_.SetEventSink(nullptr);
  if (stop_event_) {
    ::SetEvent(stop_event_);
  }
  if (worker_.joinable()) {
    worker_.join();
  }

  CloseReplyChannels();

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
  if (stop_event_) {
    ::CloseHandle(stop_event_);
  }
}

bool JsApiBrowserBridge::OnProcessMessageReceived(
    CefRefPtr<CefBrowser> browser,
    CefRefPtr<CefFrame> frame,
    CefProcessId source_process,
    CefRefPtr<CefProcessMessage> message) {
  if (source_process != PID_RENDERER || !message) {
    return false;
  }

  const std::string name = message->GetName().ToString();
  if (name == kAsyncRequestMessage) {
    return HandleAsyncRequest(frame, message);
  }
  if (name == kReleaseObjectMessage) {
    auto args = message->GetArgumentList();
    if (args && args->GetSize() >= 1) {
      const int handle = args->GetInt(0);
      // 析构可能连带销毁插件的业务对象（例如 Qt 控件），必须和执行点同线程。
      RunOnMainThread([this, handle] { registry_.ReleaseObject(handle); });
    }
    return true;
  }
  return false;
}

void JsApiBrowserBridge::OnBeforeClose(CefRefPtr<CefBrowser> /*browser*/) {
  // 示例中节点对象由 bridge 持有到宿主退出；这里保留挂点，
  // 真实产品可以在这里清理该浏览器注册的事件监听与对象句柄。
  // 回包通道按名字懒加载，关掉后下次调用会自动重开，不会读到失效视图。
  CloseReplyChannels();
}

// 同步请求工作线程：等共享内存请求事件 -> 取互斥量读请求 -> 执行 -> 写回包。
void JsApiBrowserBridge::SyncLoop() {
  HANDLE handles[2] = {request_event_, stop_event_};
  for (;;) {
    const DWORD wait = ::WaitForMultipleObjects(2, handles, FALSE, 500);
    if (wait == WAIT_OBJECT_0 + 1) {
      return;
    }
    if (wait != WAIT_OBJECT_0) {
      continue;
    }
    std::string payload;
    if (ReadSyncRequest(&payload) && !payload.empty()) {
      HandleSyncPayload(payload);
    }
  }
}

bool JsApiBrowserBridge::ReadSyncRequest(std::string* payload) {
  if (::WaitForSingleObject(request_mutex_, 5000) != WAIT_OBJECT_0) {
    return false;
  }
  auto* header = static_cast<SharedRequestHeader*>(request_view_);
  LONG length = header->length;
  if (length < 0) {
    length = 0;
  }
  if (length > kRequestCapacity) {
    length = kRequestCapacity;
  }
  *payload = std::string(reinterpret_cast<const char*>(header + 1),
                         static_cast<std::size_t>(length));
  ::ReleaseMutex(request_mutex_);
  return true;
}

void JsApiBrowserBridge::HandleSyncPayload(const std::string& payload) {
  jsapi::JsValue root;
  std::string error;
  if (!jsapi::ParseJson(payload, &root, &error)) {
    std::fprintf(stderr, "jsapi: bad sync request: %s\n", error.c_str());
    return;
  }

  const jsapi::JsValue* reply_map = root.Find("reply_map");
  const jsapi::JsValue* reply_event = root.Find("reply_event");
  if (!reply_map || !reply_event) {
    std::fprintf(stderr, "jsapi: sync request missing reply channel\n");
    return;
  }
  const std::string map_name = reply_map->AsString();
  const std::string event_name = reply_event->AsString();

  jsapi::RpcRequest request;
  // 复用上面已解析的 root，避免同一条 payload 解析两次。
  if (!jsapi::RpcHost::ParseRequest(root, &request, &error)) {
    jsapi::RpcResponse bad;
    if (const jsapi::JsValue* seq = root.Find("seq")) {
      bad.seq = static_cast<int>(seq->AsInt());
    }
    bad.ok = false;
    bad.error = error;
    WriteSyncReply(map_name, event_name, bad);
    return;
  }
  request.env = ResolveEnvById(request.env);

  jsapi::RpcResponse response;
  RunOnMainThread([this, &request, &response] {
    std::lock_guard<std::mutex> lock(process_mutex_);
    response = rpc_.Process(request);
  });
  WriteSyncReply(map_name, event_name, response);
}

void JsApiBrowserBridge::WriteSyncReply(const std::string& map_name,
                                        const std::string& event_name,
                                        const jsapi::RpcResponse& response) {
  std::string encoded = jsapi::RpcHost::EncodeResponse(response);
  if (encoded.size() > static_cast<std::size_t>(kReplyCapacity)) {
    jsapi::RpcResponse too_large;
    too_large.seq = response.seq;
    too_large.ok = false;
    too_large.error = "jsapi response exceeds shared-memory capacity";
    encoded = jsapi::RpcHost::EncodeResponse(too_large);
  }

  std::lock_guard<std::mutex> lock(reply_mutex_);
  ReplyChannel* channel = AcquireReplyChannelLocked(map_name, event_name);
  if (!channel) {
    std::fprintf(stderr, "jsapi: sync reply channel open failed, error=%lu\n",
                 ::GetLastError());
    return;
  }

  auto* header = static_cast<SharedReplyHeader*>(channel->view);
  header->ready = 0;
  header->seq = static_cast<LONG>(response.seq);
  header->length = static_cast<LONG>(encoded.size());
  std::memcpy(header + 1, encoded.data(), encoded.size());
  ::InterlockedExchange(&header->ready, 1);
  ::SetEvent(channel->event);
}

JsApiBrowserBridge::ReplyChannel* JsApiBrowserBridge::AcquireReplyChannelLocked(
    const std::string& map_name, const std::string& event_name) {
  auto it = reply_channels_.find(map_name);
  if (it != reply_channels_.end()) {
    return &it->second;
  }

  const std::wstring wide_map = Utf8ToWide(map_name);
  const std::wstring wide_event = Utf8ToWide(event_name);
  ReplyChannel channel;
  channel.mapping = ::OpenFileMappingW(FILE_MAP_WRITE, FALSE, wide_map.c_str());
  if (channel.mapping) {
    channel.view = ::MapViewOfFile(channel.mapping, FILE_MAP_WRITE, 0, 0, 0);
  }
  channel.event = ::OpenEventW(EVENT_MODIFY_STATE, FALSE, wide_event.c_str());
  if (!channel.mapping || !channel.view || !channel.event) {
    if (channel.view) ::UnmapViewOfFile(channel.view);
    if (channel.event) ::CloseHandle(channel.event);
    if (channel.mapping) ::CloseHandle(channel.mapping);
    return nullptr;
  }
  return &reply_channels_.emplace(map_name, channel).first->second;
}

void JsApiBrowserBridge::CloseReplyChannels() {
  std::lock_guard<std::mutex> lock(reply_mutex_);
  for (auto& entry : reply_channels_) {
    if (entry.second.view) ::UnmapViewOfFile(entry.second.view);
    if (entry.second.event) ::CloseHandle(entry.second.event);
    if (entry.second.mapping) ::CloseHandle(entry.second.mapping);
  }
  reply_channels_.clear();
}

bool JsApiBrowserBridge::HandleAsyncRequest(
    CefRefPtr<CefFrame> frame,
    CefRefPtr<CefProcessMessage> message) {
  auto args = message->GetArgumentList();
  if (!args || args->GetSize() < 1) {
    return true;
  }

  jsapi::RpcRequest request;
  std::string error;
  if (!jsapi::RpcHost::ParseRequest(args->GetString(0).ToString(), &request,
                                    &error)) {
    std::fprintf(stderr, "jsapi: bad async request: %s\n", error.c_str());
    return true;
  }
  if (auto browser = frame ? frame->GetBrowser() : nullptr) {
    request.env = ResolveEnv(browser, frame);
  }

  jsapi::RpcResponse response;
  RunOnMainThread([this, &request, &response] {
    std::lock_guard<std::mutex> lock(process_mutex_);
    response = rpc_.Process(request);
  });

  auto reply = CefProcessMessage::Create(kAsyncResponseMessage);
  reply->GetArgumentList()->SetString(0, jsapi::RpcHost::EncodeResponse(response));
  if (frame && frame->IsValid()) {
    frame->SendProcessMessage(PID_RENDERER, reply);
  }
  return true;
}

void JsApiBrowserBridge::OnEvent(const std::string& event,
                                 int callback_id,
                                 const jsapi::RpcEnv& target,
                                 const std::vector<jsapi::JsValue>& args) {
  if (stopping_.load()) {
    return;
  }
  CefPostTask(TID_UI,
              new EventDispatchTask(target, event, callback_id, args));
}

}  // namespace jsapi_bridge
