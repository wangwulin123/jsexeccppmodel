#include "jsapi_browser_bridge.h"

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
#include "jsapi_channel/jsapi_protocol.h"

namespace jsapi_bridge {
namespace {

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

// 事件环形缓冲放不下（异步事件单条 > 1MB，或槽满）时的回退路径。
class EventDispatchTask final : public CefTask {
 public:
  EventDispatchTask(jsapi::RpcEnv target, std::string payload)
      : target_(std::move(target)), payload_(std::move(payload)) {}

  void Execute() override {
    auto browser = CefBrowserHost::GetBrowserByIdentifier(target_.browser_id);
    if (!browser) {
      return;
    }
    auto frame = browser->GetFrameByIdentifier(CefString(target_.frame_id));
    if (!frame || !frame->IsValid()) {
      return;
    }
    auto message = CefProcessMessage::Create(kEventMessage);
    message->GetArgumentList()->SetString(0, payload_);
    frame->SendProcessMessage(PID_RENDERER, message);
  }

 private:
  jsapi::RpcEnv target_;
  std::string payload_;

  IMPLEMENT_REFCOUNTING(EventDispatchTask);
};

}  // namespace

// 执行栈深度按线程独立：同步请求跑在各浏览器的会话线程上，异步请求跑在宿主
// UI 线程上，两条执行线的层级互不干扰。
thread_local int JsApiBrowserBridge::nest_level_ = 0;

// ---------------------------------------------------------------------------
// ChannelState
// ---------------------------------------------------------------------------
JsApiBrowserBridge::ChannelState::ChannelState(JsApiBrowserBridge* bridge,
                                               unsigned long channel_key)
    : owner(bridge),
      key(channel_key),
      reader(channel_key),
      reply(channel_key),
      events(channel_key) {
  nest_event = ::CreateEventW(nullptr, FALSE, FALSE, nullptr);
  wake_event = ::CreateEventW(nullptr, FALSE, FALSE, nullptr);
  session_event = ::CreateEventW(nullptr, FALSE, FALSE, nullptr);
  abandon_event = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
  stop_event = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
}

JsApiBrowserBridge::ChannelState::~ChannelState() {
  StopReader();
  StopSession();
  if (nest_event) ::CloseHandle(nest_event);
  if (wake_event) ::CloseHandle(wake_event);
  if (session_event) ::CloseHandle(session_event);
  if (abandon_event) ::CloseHandle(abandon_event);
  if (stop_event) ::CloseHandle(stop_event);
}

void JsApiBrowserBridge::ChannelState::StartReader() {
  if (!reader_thread.joinable()) {
    reader_thread = std::thread(&ChannelState::ReaderLoop, this);
  }
}

void JsApiBrowserBridge::ChannelState::StopReader() {
  if (!reader_thread.joinable()) {
    return;
  }
  abandoned.store(true);
  if (stop_event) ::SetEvent(stop_event);
  if (abandon_event) ::SetEvent(abandon_event);
  reader_thread.join();
}

void JsApiBrowserBridge::ChannelState::StartSession() {
  if (!session_thread.joinable()) {
    session_thread = std::thread(&ChannelState::SessionLoop, this);
  }
}

void JsApiBrowserBridge::ChannelState::StopSession() {
  if (!session_thread.joinable()) {
    return;
  }
  if (stop_event) ::SetEvent(stop_event);
  if (abandon_event) ::SetEvent(abandon_event);
  session_thread.join();
}

void JsApiBrowserBridge::ChannelState::ReaderLoop() {
  HANDLE handles[4] = {reader.event(), wake_event, abandon_event, stop_event};
  while (!abandoned.load()) {
    const DWORD wait = ::WaitForMultipleObjects(4, handles, FALSE, 200);
    if (wait == WAIT_OBJECT_0 + 2 || wait == WAIT_OBJECT_0 + 3) {
      return;
    }
    if (wait != WAIT_OBJECT_0 && wait != WAIT_OBJECT_0 + 1) {
      continue;
    }
    if (owner) {
      // 读取线程只负责收与路由，永不执行函数体：它必须始终是请求通道的等待者，
      // 否则插件同步事件期间到达的嵌套请求无人读取。
      owner->OnRequestReady(this);
    }
  }
}

void JsApiBrowserBridge::ChannelState::SessionLoop() {
  HANDLE handles[3] = {session_event, abandon_event, stop_event};
  while (!abandoned.load()) {
    const DWORD wait = ::WaitForMultipleObjects(3, handles, FALSE, 200);
    if (wait == WAIT_OBJECT_0 + 1 || wait == WAIT_OBJECT_0 + 2) {
      return;
    }
    if (!owner) {
      continue;
    }
    for (;;) {
      QueuedItem item;
      {
        std::lock_guard<std::mutex> lock(queue_mutex);
        if (top_level.empty()) {
          break;
        }
        item = std::move(top_level.front());
        top_level.pop_front();
      }
      owner->RunTopLevel(this, item.seq, std::move(item.payload));
    }
    // 异步执行栈（宿主 UI 线程）结束后可能留下嵌套请求，或执行期间到达的请求
    // 因 execute_level > 0 进了嵌套队列但没有等待者：这里按顶层补执行。
    owner->DrainNestedAsTopLevel(this);
  }
}

// ---------------------------------------------------------------------------
// JsApiBrowserBridge
// ---------------------------------------------------------------------------
JsApiBrowserBridge::JsApiBrowserBridge() : rpc_(&registry_) {
  registry_.SetEventSink(this);
}

JsApiBrowserBridge::~JsApiBrowserBridge() {
  stopping_.store(true);
  registry_.SetEventSink(nullptr);

  std::vector<unsigned long> keys;
  {
    std::lock_guard<std::mutex> lock(states_mutex_);
    for (const auto& entry : states_) {
      keys.push_back(entry.first);
    }
  }
  for (const unsigned long key : keys) {
    RemoveState(key);
  }
}

void JsApiBrowserBridge::SetMainThreadDispatcher(
    std::function<void(std::function<void()>)> dispatcher) {
  std::lock_guard<std::mutex> lock(dispatcher_mutex_);
  main_thread_dispatcher_ = std::move(dispatcher);
}

void JsApiBrowserBridge::SetMainThreadPoster(
    std::function<void(std::function<void()>)> poster) {
  std::lock_guard<std::mutex> lock(dispatcher_mutex_);
  main_thread_poster_ = std::move(poster);
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

void JsApiBrowserBridge::OnAfterCreated(CefRefPtr<CefBrowser> browser) {
  if (!browser) {
    return;
  }
  // 通道名只依赖 browser_id：浏览器进程先建好通道与读取线程，渲染进程随后
  // 用同一个 browser_id 打开同名对象，因此不依赖任何"渲染进程先发消息"的握手。
  const unsigned long key =
      static_cast<unsigned long>(browser->GetIdentifier());
  if (key == 0) {
    return;
  }

  std::shared_ptr<ChannelState> state;
  {
    std::lock_guard<std::mutex> lock(states_mutex_);
    if (states_.find(key) != states_.end()) {
      return;
    }
    state = std::make_shared<ChannelState>(this, key);
    std::string error;
    if (!state->reader.Ensure(&error) || !state->reply.Ensure(&error) ||
        !state->events.Ensure(&error)) {
      std::fprintf(stderr, "jsapi: channel init for browser %lu failed: %s\n",
                   key, error.c_str());
      return;
    }
    state->env.browser_id = browser->GetIdentifier();
    if (auto main_frame = browser->GetMainFrame()) {
      state->env.frame_id = main_frame->GetIdentifier().ToString();
    }
    states_[key] = state;
  }

  state->StartSession();
  state->StartReader();
  std::fprintf(stderr, "jsapi: sync channel ready, browser=%lu frame=%s\n", key,
               state->env.frame_id.c_str());
  // 通道建立前可能已经有挂起请求（pending 电平兜底），唤醒读取线程补收一次。
  ::SetEvent(state->wake_event);
}

void JsApiBrowserBridge::OnBeforeClose(CefRefPtr<CefBrowser> browser) {
  if (!browser) {
    return;
  }
  RemoveState(static_cast<unsigned long>(browser->GetIdentifier()));
}

void JsApiBrowserBridge::OnRenderProcessTerminated(
    CefRefPtr<CefBrowser> browser) {
  if (!browser) {
    return;
  }
  const unsigned long key =
      static_cast<unsigned long>(browser->GetIdentifier());
  std::shared_ptr<ChannelState> state;
  {
    std::lock_guard<std::mutex> lock(states_mutex_);
    auto it = states_.find(key);
    if (it == states_.end()) {
      return;
    }
    state = it->second;
  }
  // 通道不回收：重载后新的渲染进程会打开同名对象，读取线程继续服务。
  // 这里只清掉崩溃渲染进程留下的执行栈状态与滞留请求。
  {
    std::lock_guard<std::mutex> lock(state->queue_mutex);
    state->nested.clear();
    state->top_level.clear();
  }
  state->execute_level.store(0);
  state->logged_first_request.store(false);
  // 标记渲染进程已下线并唤醒所有等待者：正在同步事件里等待的插件函数立即
  // 以 "render process terminated" 失败返回，而不是白等满 10s 超时。
  state->renderer_attached.store(false);
  if (state->nest_event) ::SetEvent(state->nest_event);
  if (state->session_event) ::SetEvent(state->session_event);
  // 清掉旧页面尚未派发的异步事件：新渲染进程的 callback_id 会重新从 1 开始，
  // 残留事件会被误派发到不相干的回调上。
  state->events.Clear();
  // 渲染进程内的回调表随进程一起没了，该浏览器的所有监听器都已失效。
  registry_.DiscardListeners(browser->GetIdentifier(), std::string());
  // 作废可能滞留的回包（本次崩溃时正在执行的同步调用）。
  state->reply.Reset();
  std::fprintf(stderr, "jsapi: render process terminated, channel kept, browser=%lu\n",
               key);
}

void JsApiBrowserBridge::RemoveState(unsigned long key) {
  std::shared_ptr<ChannelState> state;
  {
    std::lock_guard<std::mutex> lock(states_mutex_);
    auto it = states_.find(key);
    if (it == states_.end()) {
      return;
    }
    state = it->second;
    states_.erase(it);
  }
  // 先置放弃标志，唤醒正在等待的同步事件，再回收读取/会话线程。
  state->abandoned.store(true);
  if (state->abandon_event) {
    ::SetEvent(state->abandon_event);
  }
  state->StopReader();
  state->StopSession();
}

std::shared_ptr<JsApiBrowserBridge::ChannelState>
JsApiBrowserBridge::FindStateByEnv(const jsapi::RpcEnv& target) {
  std::lock_guard<std::mutex> lock(states_mutex_);
  if (target.browser_id >= 0) {
    auto it = states_.find(static_cast<unsigned long>(target.browser_id));
    if (it != states_.end()) {
      return it->second;
    }
  }
  if (!target.frame_id.empty()) {
    for (const auto& entry : states_) {
      if (entry.second->env.frame_id == target.frame_id) {
        return entry.second;
      }
    }
  }
  return nullptr;
}

void JsApiBrowserBridge::OnRequestReady(ChannelState* state) {
  for (;;) {
    MessageKind kind = kMessageRpc;
    long seq = 0;
    long invoke_id = 0;
    std::string payload;
    if (!state->reader.TryRead(&kind, &seq, &invoke_id, &payload)) {
      break;
    }
    if (state->abandoned.load()) {
      return;
    }
    // 收到存活渲染进程的请求即视为重新接入（崩溃重载后恢复同步事件能力）。
    state->renderer_attached.store(true);
    if (kind == kMessageEventResult) {
      CompleteInvoke(state, static_cast<int>(invoke_id), payload);
      continue;
    }

    if (state->execute_level.load() == 0) {
      // 顶层请求：交给会话线程执行，函数体默认就跑在那里（ThreadAffinity
      // ::Worker）；只有声明 UI 亲和的成员才会切到宿主 UI 线程。读取线程
      // 投递后立刻回到等待，保证执行期间嵌套请求仍能被读入。
      {
        std::lock_guard<std::mutex> lock(state->queue_mutex);
        state->top_level.push_back(
            QueuedItem{seq, static_cast<long>(kind), std::move(payload)});
      }
      ::SetEvent(state->session_event);
    } else {
      // 嵌套请求：交给正在等待的同步事件内联执行。
      {
        std::lock_guard<std::mutex> lock(state->queue_mutex);
        state->nested.push_back(QueuedItem{seq, static_cast<long>(kind),
                                           std::move(payload)});
      }
      ::SetEvent(state->nest_event);
    }
  }
}

void JsApiBrowserBridge::CompleteInvoke(ChannelState* state,
                                        int invoke_id,
                                        const std::string& payload) {
  int decoded_id = invoke_id;
  bool ok = false;
  jsapi::JsValue value;
  std::string error;
  if (!DecodeEventResult(payload, &decoded_id, &ok, &value, &error)) {
    return;
  }

  std::shared_ptr<InvokeState> invoke;
  {
    std::lock_guard<std::mutex> lock(invoke_mutex_);
    auto it = invokes_.find(decoded_id);
    if (it == invokes_.end()) {
      // 超时之后迟到的回执：没有等待者，直接丢弃。
      return;
    }
    invoke = it->second;
  }
  invoke->ok = ok;
  invoke->value = std::move(value);
  invoke->error = std::move(error);
  invoke->done = true;
  ::SetEvent(state->nest_event);
}

jsapi::RpcResponse JsApiBrowserBridge::ProcessPayload(ChannelState* state,
                                                      const std::string& payload) {
  jsapi::RpcResponse response;
  jsapi::JsValue root;
  std::string error;
  if (!jsapi::ParseJson(payload, &root, &error)) {
    response.ok = false;
    response.error = error;
    return response;
  }
  jsapi::RpcRequest request;
  if (!jsapi::RpcHost::ParseRequest(root, &request, &error)) {
    if (const jsapi::JsValue* seq = root.Find("seq")) {
      response.seq = static_cast<int>(seq->AsInt());
    }
    response.ok = false;
    response.error = error;
    return response;
  }
  // 请求里的 env 由渲染进程按当前 document 生成，导航/刷新后 frame id 会变，
  // 必须原样采用；通道建立时缓存的 env 只是创建那一刻的快照，拿它覆盖会让
  // 新页面的请求带上旧 frame id，事件被派发到上一个页面的监听器。
  // browser_id 以通道为准（渲染进程拿不到 browser 时可能填 -1）。
  request.env.browser_id = static_cast<int>(state->key);
  if (request.env.frame_id.empty()) {
    request.env.frame_id = state->env.frame_id;
  }

  // 新 document 建根对象（describe(0)）时清掉同浏览器其它 frame 的监听器：
  // 旧文档的 V8 上下文已销毁，其 callback_id 不再存在，留着只会让事件先命中
  // 失效监听器。按浏览器隔离，不影响同进程的其它页面。
  if (request.op == "describe" && request.handle == 0) {
    registry_.DiscardListeners(request.env.browser_id, request.env.frame_id);
  }

  // 成员按声明的亲和执行：UI 亲和切到宿主 UI 线程并同步返回，其余留在当前
  // 会话线程。注册表与对象函数表内部各自持锁，这里不再加全局锁——否则各页面
  // 的会话线程会互相排队，且嵌套 UI 亲和的等待会与外层锁形成死锁。
  return rpc_.Process(request, [this](const std::function<void()>& body) {
    RunOnMainThread([&body] { body(); });
  });
}

void JsApiBrowserBridge::WriteReply(ChannelState* state,
                                    long seq,
                                    const jsapi::RpcResponse& response) {
  std::string encoded = jsapi::RpcHost::EncodeResponse(response);
  if (encoded.size() > static_cast<std::size_t>(kReplyCapacity)) {
    jsapi::RpcResponse too_large;
    too_large.seq = response.seq;
    too_large.ok = false;
    too_large.error = "jsapi response exceeds shared-memory capacity";
    encoded = jsapi::RpcHost::EncodeResponse(too_large);
  }
  (void)seq;
  if (!state->reply.Write(response.seq, encoded)) {
    std::fprintf(stderr, "jsapi: write sync reply failed, browser=%lu\n",
                 state->key);
  }
}

void JsApiBrowserBridge::RunTopLevel(ChannelState* state,
                                     long seq,
                                     std::string payload) {
  if (!state || state->abandoned.load()) {
    return;
  }
  if (!state->logged_first_request.exchange(true)) {
    std::fprintf(stderr, "jsapi: sync request channel active, browser=%lu\n",
                 state->key);
  }
  state->execute_level.store(1);
  nest_level_ = 1;

  jsapi::RpcResponse response = ProcessPayload(state, payload);
  if (response.seq == 0) {
    response.seq = static_cast<int>(seq);
  }
  WriteReply(state, seq, response);

  nest_level_ = 0;
  state->execute_level.store(0);
}

void JsApiBrowserBridge::DrainNestedAsTopLevel(ChannelState* state) {
  // 顶层结束后滞留的嵌套请求没有等待者可内联，按顶层重新执行一遍。
  // 只在会话线程调用，保证函数体线程亲和不变。
  for (;;) {
    QueuedItem item;
    {
      std::lock_guard<std::mutex> lock(state->queue_mutex);
      if (state->nested.empty()) {
        break;
      }
      item = std::move(state->nested.front());
      state->nested.pop_front();
    }
    if (item.kind == static_cast<long>(kMessageEventResult)) {
      // 该层已结束才到的回执，没有等待者。
      continue;
    }
    RunTopLevel(state, item.seq, std::move(item.payload));
  }
}

void JsApiBrowserBridge::DrainNestedQueue(ChannelState* state,
                                          int current_invoke,
                                          InvokeState* current) {
  for (;;) {
    QueuedItem item;
    {
      std::lock_guard<std::mutex> lock(state->queue_mutex);
      if (state->nested.empty()) {
        break;
      }
      item = std::move(state->nested.front());
      state->nested.pop_front();
    }

    if (item.kind == static_cast<long>(kMessageEventResult)) {
      int invoke_id = 0;
      bool ok = false;
      jsapi::JsValue value;
      std::string error;
      if (!DecodeEventResult(item.payload, &invoke_id, &ok, &value, &error)) {
        continue;
      }
      if (invoke_id == current_invoke) {
        current->ok = ok;
        current->value = std::move(value);
        current->error = std::move(error);
        current->done = true;
        continue;
      }
      std::shared_ptr<InvokeState> other;
      {
        std::lock_guard<std::mutex> lock(invoke_mutex_);
        auto it = invokes_.find(invoke_id);
        if (it != invokes_.end()) {
          other = it->second;
        }
      }
      if (other) {
        other->ok = ok;
        other->value = std::move(value);
        other->error = std::move(error);
        other->done = true;
        ::SetEvent(state->nest_event);
      }
      continue;
    }

    // 嵌套 RPC：必须正好位于同步事件的等待层内，否则判定计数器失配。
    if (state->execute_level.load() != nest_level_ + 1) {
      jsapi::RpcResponse bad;
      bad.seq = static_cast<int>(item.seq);
      bad.ok = false;
      bad.error = "level misaligned, sync nesting rejected";
      WriteReply(state, item.seq, bad);
      continue;
    }
    nest_level_ += 1;
    jsapi::RpcResponse response = ProcessPayload(state, item.payload);
    if (response.seq == 0) {
      response.seq = static_cast<int>(item.seq);
    }
    nest_level_ -= 1;
    WriteReply(state, item.seq, response);
  }
}

int JsApiBrowserBridge::OnEventSync(const std::string& event,
                                    int callback_id,
                                    const jsapi::RpcEnv& target,
                                    const std::vector<jsapi::JsValue>& args,
                                    int timeout_ms,
                                    jsapi::JsValue* result,
                                    std::string* error) {
  if (result) *result = jsapi::JsValue::Null();
  if (error) error->clear();

  auto state = FindStateByEnv(target);
  if (!state || state->abandoned.load()) {
    // 目标页面还没建立通道：按"无监听者"立即失败，不白等超时。
    return 0;
  }
  if (!state->renderer_attached.load()) {
    // 渲染进程已崩溃、尚未重新接入：没有可执行的回调，立即失败而不是等满超时。
    return 0;
  }

  {
    std::lock_guard<std::mutex> lock(dispatcher_mutex_);
    if (!main_thread_poster_) {
      if (error) *error = "sync events require a main thread poster";
      return -1;
    }
  }

  if (state->execute_level.load() != nest_level_) {
    if (error) *error = "level misaligned, sync nesting rejected";
    return 1;
  }
  if (state->execute_level.load() >= kMaxSyncDepth) {
    if (error) *error = "nested event depth exceeded";
    return 1;
  }

  const int invoke_id = next_invoke_.fetch_add(1);
  auto invoke = std::make_shared<InvokeState>();
  {
    std::lock_guard<std::mutex> lock(invoke_mutex_);
    invokes_[invoke_id] = invoke;
  }

  state->execute_level.fetch_add(1);
  const std::string payload =
      EncodeEventPayload(event, callback_id, invoke_id, args,
                         state->env.frame_id);
  if (!state->events.Write(invoke_id, payload)) {
    state->execute_level.fetch_sub(1);
    {
      std::lock_guard<std::mutex> lock(invoke_mutex_);
      invokes_.erase(invoke_id);
    }
    if (error) *error = "event channel overflow or event too large";
    return 1;
  }

  const DWORD budget = timeout_ms > 0 ? static_cast<DWORD>(timeout_ms)
                                      : static_cast<DWORD>(kDefaultTimeoutMs);
  const ULONGLONG deadline = ::GetTickCount64() + budget;
  std::string failure;
  HANDLE handles[2] = {state->nest_event, state->abandon_event};
  for (;;) {
    // 只观察 sent message（QS_SENDMESSAGE），不进入 Qt 事件循环。
    const DWORD wait = ::MsgWaitForMultipleObjects(2, handles, FALSE, 200,
                                                   QS_SENDMESSAGE);
    if (wait == WAIT_OBJECT_0) {
      DrainNestedQueue(state.get(), invoke_id, invoke.get());
    } else if (wait == WAIT_OBJECT_0 + 1) {
      failure = "render process abandoned";
      break;
    } else if (wait == WAIT_OBJECT_0 + 2) {
      MSG msg;
      while (::PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE | PM_QS_SENDMESSAGE)) {
        ::TranslateMessage(&msg);
        ::DispatchMessageW(&msg);
      }
    }

    if (invoke->done) {
      break;
    }
    if (state->abandoned.load()) {
      failure = "render process abandoned";
      break;
    }
    if (!state->renderer_attached.load()) {
      failure = "render process terminated";
      break;
    }
    if (::GetTickCount64() >= deadline) {
      failure = "sync event timed out";
      break;
    }
  }

  state->execute_level.fetch_sub(1);
  {
    std::lock_guard<std::mutex> lock(invoke_mutex_);
    invokes_.erase(invoke_id);
  }

  if (invoke->done) {
    if (result) *result = invoke->value;
    if (!invoke->ok && error) {
      *error = invoke->error.empty() ? "event callback failed" : invoke->error;
    }
    return 1;
  }
  if (error) *error = failure;
  return 1;
}

void JsApiBrowserBridge::OnEvent(
    const std::string& event,
    int callback_id,
    const jsapi::RpcEnv& target,
    const std::vector<jsapi::JsValue>& args) {
  if (stopping_.load()) {
    return;
  }

  const std::string payload =
      EncodeEventPayload(event, callback_id, 0, args, target.frame_id);
  auto state = FindStateByEnv(target);
  if (state && state->abandoned.load()) {
    return;
  }
  if (state && !state->renderer_attached.load()) {
    // 渲染进程已崩溃：没有页面能接收回调，直接丢弃，避免残留进环形缓冲。
    return;
  }
  if (state && state->events.Write(0, payload)) {
    return;
  }
  // 单条超过事件槽容量或目标通道未建立：回退进程消息。
  CefPostTask(TID_UI, new EventDispatchTask(target, payload));
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

  // 异步 RPC 也计入执行栈：它执行期间允许插件发同步事件（页面主线程空闲，
  // 渲染侧 watcher 会按第四路 PostTask 派发）。若已经在同步执行栈里
  // （回调内发起的异步调用），不要覆盖层级，交给宿主事件循环延后执行。
  std::shared_ptr<ChannelState> state = FindStateByEnv(request.env);
  const bool entered_level = state && state->execute_level.load() == 0;
  if (entered_level) {
    state->execute_level.store(1);
  }

  // 异步调用不占会话线程（它来自渲染进程主线程的空闲时机），仍在宿主 UI 线程
  // 执行；函数体若声明 UI 亲和则原地执行，无需切换线程。
  // 注意：异步请求可能在渲染主线程阻塞进同步调用前发出，因此它可能与同页的
  // 同步执行栈重叠（execute_level 非 0）。此时异步函数体内再派发同步事件会
  // 因层级失配被拒绝，异步函数不要使用 DispatchEventSync。
  jsapi::RpcResponse response;
  RunOnMainThread([this, &request, &response, entered_level] {
    const int saved_level = nest_level_;
    if (entered_level) {
      nest_level_ = 1;
    }
    response = rpc_.Process(request);
    nest_level_ = saved_level;
  });

  if (entered_level) {
    state->execute_level.store(0);
    // 唤醒会话线程接管滞留的嵌套请求（异步执行栈里收到的请求）。
    ::SetEvent(state->session_event);
  }

  auto reply = CefProcessMessage::Create(kAsyncResponseMessage);
  reply->GetArgumentList()->SetString(0, jsapi::RpcHost::EncodeResponse(response));
  if (frame && frame->IsValid()) {
    frame->SendProcessMessage(PID_RENDERER, reply);
  }
  return true;
}

}  // namespace jsapi_bridge
