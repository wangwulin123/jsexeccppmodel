#pragma once

#include <windows.h>

#include <atomic>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "jsapi_channel/jsapi_channel.h"

namespace jsapi_bridge {

// 同步 RPC 通道（渲染进程侧）。
//
// 请求/回包走共享内存，回包用 pending 电平 + seq 双校验；事件走环形缓冲。
// 常驻 watcher 线程是事件唯一接收者，按 sync_depth_ 四路分流：
//   同步事件 + 阻塞中 -> handoff（注入当前等待层级）
//   同步事件 + 空闲   -> PostTask（异步 RPC 期间的同步事件）
//   异步事件 + 阻塞中 -> stash（最外层返回后派发）
//   异步事件 + 空闲   -> PostTask
class SyncChannel {
 public:
  using InlineDispatch = std::function<bool(const std::string& name,
                                            int callback_id,
                                            const std::vector<jsapi::JsValue>& args,
                                            jsapi::JsValue* result,
                                            std::string* error)>;
  using PostDispatch = std::function<void(const std::string& payload)>;

  SyncChannel() = default;
  ~SyncChannel();

  SyncChannel(const SyncChannel&) = delete;
  SyncChannel& operator=(const SyncChannel&) = delete;

  void SetInlineDispatch(InlineDispatch handler) {
    inline_dispatch_ = std::move(handler);
  }
  void SetPostDispatch(PostDispatch handler) {
    post_dispatch_ = std::move(handler);
  }

  // channel_key 与浏览器进程对齐（浏览器标识）。两端用它派生同一组共享内存
  // 与事件名，因此不依赖任何进程消息握手：渲染主线程在 OnContextCreated 里
  // 阻塞等待首个同步 RPC 时，渲染->浏览器的进程消息是发不出去的。
  bool Start(unsigned long channel_key, std::string* error);
  void Stop();

  bool Send(const std::string& payload,
            int seq,
            std::string* response,
            std::string* error,
            int timeout_ms = kDefaultTimeoutMs);
  // watcher PostTask 到渲染主线程后的执行入口（含同步事件回执）。
  void DispatchPosted(const std::string& payload) { ExecutePayload(payload); }

 private:
  struct EventItem {
    std::string payload;
  };

  void WatcherLoop();
  void ExecutePayload(const std::string& payload);
  void DrainHandoffInline();
  void DrainToPost(bool handoff);
  void Post(const std::string& payload) {
    if (post_dispatch_) {
      post_dispatch_(payload);
    }
  }

  unsigned long key_ = 0;
  std::unique_ptr<RequestWriter> request_writer_;
  std::unique_ptr<ReplyReader> reply_reader_;
  std::unique_ptr<EventRingReader> evt_reader_;

  HANDLE handoff_event_ = nullptr;
  HANDLE stop_event_ = nullptr;
  std::thread watcher_;
  std::atomic<bool> started_{false};
  std::atomic<bool> stopping_{false};
  std::atomic<int> sync_depth_{0};

  std::mutex send_mutex_;
  std::mutex queue_mutex_;
  std::deque<EventItem> handoff_;
  std::deque<EventItem> stash_;

  InlineDispatch inline_dispatch_;
  PostDispatch post_dispatch_;
};

}  // namespace jsapi_bridge
