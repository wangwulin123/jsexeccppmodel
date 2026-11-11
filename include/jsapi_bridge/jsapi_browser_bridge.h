#pragma once

#include <windows.h>

#include <atomic>
#include <functional>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "include/cef_browser.h"
#include "include/cef_process_message.h"
#include "jsapi/js_registry.h"
#include "jsapi/js_value.h"
#include "jsapi_rpc/jsapi_rpc.h"

namespace jsapi_bridge {

// 浏览器进程侧的 JSAPI 宿主：
//  - 持有节点注册表和 RpcHost（真正执行 C++ 函数）；
//  - 同步 RPC 由独立工作线程从共享内存读取请求、写共享内存回包。渲染进程在主线程
//    上同步阻塞时无法处理 CEF 消息循环，CefFrame::SendProcessMessage 要等回到消息
//    循环才真正送达，所以同步链路不能走进程消息；
//  - 异步 RPC 仍走进程消息；事件统一投递回 CEF UI 线程后再转发给渲染进程；
//  - 插件函数默认在收取请求的线程上执行（同步=工作线程，异步=CEF UI 线程）。
//    宿主可通过 SetMainThreadDispatcher 把执行点统一切到自己的 UI 主线程（例如
//    Qt GUI 线程），这样插件里可以直接操作窗口控件。
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

  bool OnProcessMessageReceived(CefRefPtr<CefBrowser> browser,
                                CefRefPtr<CefFrame> frame,
                                CefProcessId source_process,
                                CefRefPtr<CefProcessMessage> message);

  void OnBeforeClose(CefRefPtr<CefBrowser> browser);

 private:
  void OnEvent(const std::string& event,
               int callback_id,
               const jsapi::RpcEnv& target,
               const std::vector<jsapi::JsValue>& args) override;

  bool HandleAsyncRequest(CefRefPtr<CefFrame> frame,
                          CefRefPtr<CefProcessMessage> message);

  // 在宿主主线程执行插件函数；未设置 dispatcher 时退化为当前线程直接执行。
  void RunOnMainThread(std::function<void()> task);

  // 同步请求工作线程。
  void SyncLoop();
  bool ReadSyncRequest(std::string* payload);
  void HandleSyncPayload(const std::string& payload);
  void WriteSyncReply(const std::string& map_name,
                      const std::string& event_name,
                      const jsapi::RpcResponse& response);

  // 回包通道按 map 名缓存：渲染进程的 map/event 名字在其进程存活期内固定，
  // 每次回包重新 OpenFileMapping + MapViewOfFile 是纯内核开销。
  struct ReplyChannel {
    HANDLE mapping = nullptr;
    void* view = nullptr;
    HANDLE event = nullptr;
  };
  ReplyChannel* AcquireReplyChannelLocked(const std::string& map_name,
                                          const std::string& event_name);
  void CloseReplyChannels();

  jsapi::JsRegistry registry_;
  jsapi::RpcHost rpc_;
  // 同步请求（工作线程）与异步请求（UI 线程）都通过它串行执行插件函数。
  std::mutex process_mutex_;
  // 执行点投递：默认空，表示在收取请求的线程上直接执行。
  std::mutex dispatcher_mutex_;
  std::function<void(std::function<void()>)> main_thread_dispatcher_;

  HANDLE request_map_ = nullptr;
  void* request_view_ = nullptr;
  HANDLE request_event_ = nullptr;
  HANDLE request_mutex_ = nullptr;
  HANDLE stop_event_ = nullptr;
  // 回包通道缓存（工作线程读写，OnBeforeClose/析构可能并发清理）。
  std::mutex reply_mutex_;
  std::map<std::string, ReplyChannel> reply_channels_;
  std::thread worker_;
  std::atomic<bool> stopping_{false};
};

}  // namespace jsapi_bridge
