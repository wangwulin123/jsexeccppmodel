#pragma once

#include <windows.h>

#include <atomic>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "include/cef_browser.h"
#include "include/cef_process_message.h"
#include "jsapi/js_registry.h"
#include "jsapi/js_value.h"
#include "jsapi_rpc/jsapi_rpc.h"
#include "jsapi_channel/jsapi_channel.h"

namespace jsapi_bridge {

// 浏览器进程侧的 JSAPI 宿主：
//  - 持有节点注册表和 RpcHost（真正执行 C++ 函数）；
//  - 同步 RPC 按浏览器标识隔离：通道名由 browser_id 派生，浏览器进程在
//    OnAfterCreated 就建立通道并启动读取线程，渲染进程用同一个 browser_id
//    计算通道名。两端无需握手——渲染主线程在 OnContextCreated 里阻塞等待
//    首个同步 RPC 时，渲染→浏览器的进程消息发不出去，任何依赖握手的启动
//    流程都会自死锁；
//  - 读/执行分离：每个浏览器一条读取线程 + 一条会话线程。读取线程是请求通道
//    的唯一等待者，永远不因执行而停止等待；顶层请求交给会话线程执行，嵌套
//    请求压入该通道的嵌套队列并 SetEvent，由正在等待的同步事件在同一会话线程
//    内联执行。两者合一会在插件派发同步事件时饿死嵌套请求（读取线程被等待
//    循环占住），因此必须分离；
//  - 线程亲和：函数体默认在会话线程执行；声明为 ThreadAffinity::UI 的成员
//    通过主线程 dispatcher 阻塞切到宿主 UI 线程执行再返回；
//  - 事件优先走共享内存环形缓冲；单条超过槽容量时回退 kEventMessage 进程消息。
class JsApiBrowserBridge final : public jsapi::JsEventSink {
 public:
  JsApiBrowserBridge();
  ~JsApiBrowserBridge() override;

  jsapi::JsRegistry& registry() { return registry_; }

  // 宿主 UI 主线程投递器。约定：dispatcher 返回时 task 必须已经执行完毕
  // （阻塞式投递，例如 Qt 的 Qt::BlockingQueuedConnection）。
  // 未设置时插件函数在收取请求的线程上直接执行。
  void SetMainThreadDispatcher(
      std::function<void(std::function<void()>)> dispatcher);

  // 能力开关：声明宿主支持同步嵌套事件（DispatchEventSync）。未设置时同步事件
  // 直接返回 "sync events require a main thread poster"，不阻塞。
  // 事件等待循环与嵌套执行都发生在会话线程，投递器本身不参与执行。
  void SetMainThreadPoster(std::function<void(std::function<void()>)> poster);

  bool OnProcessMessageReceived(CefRefPtr<CefBrowser> browser,
                                CefRefPtr<CefFrame> frame,
                                CefProcessId source_process,
                                CefRefPtr<CefProcessMessage> message);

  // 浏览器创建完成即建立该浏览器的同步通道（必须先于渲染进程的首次同步调用）。
  void OnAfterCreated(CefRefPtr<CefBrowser> browser);
  void OnBeforeClose(CefRefPtr<CefBrowser> browser);
  void OnRenderProcessTerminated(CefRefPtr<CefBrowser> browser);

 private:
  struct QueuedItem {
    long seq = 0;
    long kind = 0;
    std::string payload;
  };

  struct InvokeState {
    bool done = false;
    bool ok = false;
    jsapi::JsValue value;
    std::string error;
  };

  // 每个浏览器一条独立通道 + 一条读取线程 + 一条会话线程。
  struct ChannelState {
    ChannelState(JsApiBrowserBridge* bridge, unsigned long channel_key);
    ~ChannelState();

    void StartReader();
    void StopReader();
    void ReaderLoop();

    void StartSession();
    void StopSession();
    void SessionLoop();

    JsApiBrowserBridge* owner = nullptr;
    unsigned long key = 0;
    jsapi::RpcEnv env;

    RequestReader reader;
    ReplyWriter reply;
    EventRingWriter events;

    HANDLE nest_event = nullptr;     // 进程内：嵌套请求/回执到达
    HANDLE wake_event = nullptr;     // 进程内：唤醒读取线程重新收请求
    HANDLE session_event = nullptr;  // 进程内：唤醒会话线程执行顶层请求
    HANDLE abandon_event = nullptr;  // 渲染进程终止
    HANDLE stop_event = nullptr;     // 关闭读取/会话线程

    // > 0 表示该通道当前正处于某个同步执行栈里（顶层任务或同步事件等待中）。
    // 读取线程据此区分"顶层请求"与"嵌套请求"。
    std::atomic<int> execute_level{0};
    std::atomic<bool> abandoned{false};
    // 渲染进程是否存活。崩溃后置 false：同步事件与异步事件立即失败，
    // 不再白等 10s；新渲染进程的首个请求到达时恢复 true。
    std::atomic<bool> renderer_attached{true};
    std::atomic<bool> logged_first_request{false};

    std::mutex queue_mutex;
    std::deque<QueuedItem> nested;    // 执行栈内的嵌套请求（等待循环内联执行）
    std::deque<QueuedItem> top_level;  // 空闲时到达的顶层请求（会话线程执行）

    std::thread reader_thread;
    std::thread session_thread;
  };

  void OnEvent(const std::string& event,
               int callback_id,
               const jsapi::RpcEnv& target,
               const std::vector<jsapi::JsValue>& args) override;

  int OnEventSync(const std::string& event,
                  int callback_id,
                  const jsapi::RpcEnv& target,
                  const std::vector<jsapi::JsValue>& args,
                  int timeout_ms,
                  jsapi::JsValue* result,
                  std::string* error) override;

  bool HandleAsyncRequest(CefRefPtr<CefFrame> frame,
                          CefRefPtr<CefProcessMessage> message);

  // 阻塞切到宿主 UI 线程执行 task，返回时已执行完；未设置 dispatcher 时就地执行。
  void RunOnMainThread(std::function<void()> task);

  // 读取线程：把请求路由到会话线程执行或嵌套队列，处理同步事件回执。
  void OnRequestReady(ChannelState* state);
  void CompleteInvoke(ChannelState* state,
                      int invoke_id,
                      const std::string& payload);

  // 会话线程：执行一次顶层请求并写回包。
  void RunTopLevel(ChannelState* state, long seq, std::string payload);
  // 同步事件等待循环里内联执行嵌套请求 / 收取回执。
  void DrainNestedQueue(ChannelState* state, int current_invoke,
                        InvokeState* current);
  void DrainNestedAsTopLevel(ChannelState* state);

  jsapi::RpcResponse ProcessPayload(ChannelState* state,
                                    const std::string& payload);
  void WriteReply(ChannelState* state,
                  long seq,
                  const jsapi::RpcResponse& response);

  std::shared_ptr<ChannelState> FindStateByEnv(const jsapi::RpcEnv& env);
  void RemoveState(unsigned long channel_key);

  jsapi::JsRegistry registry_;
  jsapi::RpcHost rpc_;

  std::mutex dispatcher_mutex_;
  std::function<void(std::function<void()>)> main_thread_dispatcher_;
  std::function<void(std::function<void()>)> main_thread_poster_;

  std::mutex states_mutex_;
  std::map<unsigned long, std::shared_ptr<ChannelState>> states_;

  std::mutex invoke_mutex_;
  std::map<int, std::shared_ptr<InvokeState>> invokes_;
  std::atomic<int> next_invoke_{1};

  // 当前执行栈深度（顶层任务 = 1）。按线程独立：每个浏览器一条会话线程，
  // 异步请求则跑在宿主 UI 线程，两条线的层级互不影响。
  static thread_local int nest_level_;
  std::atomic<bool> stopping_{false};
};

}  // namespace jsapi_bridge
