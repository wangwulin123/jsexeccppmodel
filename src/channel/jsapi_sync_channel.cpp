#include "jsapi_channel/jsapi_sync_channel.h"

#include <utility>

namespace jsapi_bridge {

SyncChannel::~SyncChannel() { Stop(); }

bool SyncChannel::Start(unsigned long channel_key, std::string* error) {
  if (started_.load() && key_ == channel_key) {
    return true;
  }
  if (started_.load()) {
    Stop();
  }
  // 换 key（同一渲染进程被另一个窗口复用）时重建全部通道对象。
  key_ = channel_key;
  request_writer_ = std::make_unique<RequestWriter>(channel_key);
  reply_reader_ = std::make_unique<ReplyReader>(channel_key);
  evt_reader_ = std::make_unique<EventRingReader>(channel_key);
  if (!request_writer_->Ensure(error) || !reply_reader_->Ensure(error) ||
      !evt_reader_->Ensure(error)) {
    request_writer_.reset();
    reply_reader_.reset();
    evt_reader_.reset();
    return false;
  }
  // 上一个渲染进程崩溃时可能留下 pending 的回包（seq 与本次进程无关），
  // 不清掉会被本次首个同步调用误当成自己的回包。通道对象按 browser_id 复用，
  // 因此必须在每次 Start（新渲染进程接入）时作废。
  reply_reader_->Reset();
  // 事件环形缓冲同理：旧页面残留的异步事件会在新渲染进程按旧 callback_id 派发，
  // 造成"回调串页"。浏览器侧在渲染进程终止时已 Clear，这里再兜一次底。
  evt_reader_->DiscardPending();

  handoff_event_ = ::CreateEventW(nullptr, FALSE, FALSE, nullptr);
  stop_event_ = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
  if (!handoff_event_ || !stop_event_) {
    if (error) {
      *error = "sync channel event init failed, error=" +
               std::to_string(::GetLastError());
    }
    return false;
  }
  started_.store(true);
  stopping_.store(false);
  watcher_ = std::thread(&SyncChannel::WatcherLoop, this);
  return true;
}

void SyncChannel::Stop() {
  if (!started_.exchange(false)) {
    return;
  }
  stopping_.store(true);
  if (stop_event_) {
    ::SetEvent(stop_event_);
  }
  if (watcher_.joinable()) {
    watcher_.join();
  }
  if (handoff_event_) {
    ::CloseHandle(handoff_event_);
    handoff_event_ = nullptr;
  }
  if (stop_event_) {
    ::CloseHandle(stop_event_);
    stop_event_ = nullptr;
  }
  request_writer_.reset();
  reply_reader_.reset();
  evt_reader_.reset();
}

bool SyncChannel::Send(const std::string& payload,
                       int seq,
                       std::string* response,
                       std::string* error,
                       int timeout_ms) {
  if (!started_.load()) {
    if (error) *error = "sync channel not started";
    return false;
  }

  // 必须在写请求之前 +1：watcher 据此判断同步事件该走 handoff 还是 PostTask。
  const int depth = sync_depth_.fetch_add(1) + 1;
  if (depth > kMaxSyncDepth) {
    sync_depth_.fetch_sub(1);
    if (error) *error = "nested call depth exceeded";
    return false;
  }

  // 不再每次清空回包槽：TryTake 用 seq 精确匹配，超时残留的旧回包会被忽略，
  // 下一次 Write 直接覆盖。省掉一次命名互斥量往返，是同步调用最热的路径之一。
  {
    std::lock_guard<std::mutex> lock(send_mutex_);
    if (!request_writer_->Write(kMessageRpc, seq, 0, payload, error)) {
      sync_depth_.fetch_sub(1);
      return false;
    }
  }

  const ULONGLONG deadline =
      ::GetTickCount64() +
      static_cast<ULONGLONG>(timeout_ms > 0 ? timeout_ms : kDefaultTimeoutMs);
  std::string reply;
  bool ok = false;
  for (;;) {
    const ULONGLONG now = ::GetTickCount64();
    if (now >= deadline) {
      if (error) *error = "sync jsapi call timed out";
      break;
    }
    HANDLE handles[2] = {reply_reader_->event(), handoff_event_};
    // 不派发窗口消息（避免 CEF 内部 SendMessage 重入）：WaitForMultipleObjects
    // 与 wake mask = 0 的 MsgWait 语义一致，但少一次消息队列检查。
    const DWORD wait = ::WaitForMultipleObjects(
        2, handles, FALSE, static_cast<DWORD>(deadline - now));
    if (wait == WAIT_OBJECT_0) {
      if (reply_reader_->TryTake(seq, nullptr, &reply)) {
        ok = true;
        break;
      }
      // 迟到回包（上一轮超时调用残留）：不消费，用剩余超时继续等。
      continue;
    }
    if (wait == WAIT_OBJECT_0 + 1) {
      DrainHandoffInline();
      continue;
    }
    if (wait == WAIT_TIMEOUT) {
      if (error) *error = "sync jsapi call timed out";
      break;
    }
    if (error) *error = "sync jsapi wait failed";
    break;
  }

  if (sync_depth_.fetch_sub(1) == 1) {
    // 最外层退出：迟到的同步事件按异步补发，暂存的异步事件按序派发。
    DrainToPost(true);
    DrainToPost(false);
  }
  if (!ok) {
    return false;
  }
  *response = std::move(reply);
  return true;
}

void SyncChannel::WatcherLoop() {
  HANDLE handles[2] = {evt_reader_->event(), stop_event_};
  while (!stopping_.load()) {
    const DWORD wait = ::WaitForMultipleObjects(2, handles, FALSE, 200);
    if (wait == WAIT_OBJECT_0 + 1) {
      return;
    }
    if (wait != WAIT_OBJECT_0) {
      continue;
    }
    long invoke_id = 0;
    std::string payload;
    while (evt_reader_->TryRead(&invoke_id, &payload)) {
      const int depth = sync_depth_.load();
      if (invoke_id > 0) {
        if (depth > 0) {
          {
            std::lock_guard<std::mutex> lock(queue_mutex_);
            handoff_.push_back(EventItem{payload});
          }
          ::SetEvent(handoff_event_);
        } else {
          Post(payload);
        }
      } else if (depth > 0) {
        std::lock_guard<std::mutex> lock(queue_mutex_);
        stash_.push_back(EventItem{payload});
      } else {
        Post(payload);
      }
    }
  }
}

void SyncChannel::ExecutePayload(const std::string& payload) {
  std::string name;
  int callback_id = -1;
  int invoke_id = 0;
  std::vector<jsapi::JsValue> args;
  if (!DecodeEventPayload(payload, &name, &callback_id, &invoke_id, &args,
                          nullptr)) {
    return;
  }

  jsapi::JsValue result = jsapi::JsValue::Null();
  std::string error;
  bool ok = true;
  if (inline_dispatch_) {
    ok = inline_dispatch_(name, callback_id, args, &result, &error);
  }
  if (invoke_id > 0) {
    std::string write_error;
    request_writer_->Write(kMessageEventResult, 0, invoke_id,
                           EncodeEventResult(invoke_id, ok, result, error),
                           &write_error);
  }
}

void SyncChannel::DrainHandoffInline() {
  for (;;) {
    EventItem item;
    {
      std::lock_guard<std::mutex> lock(queue_mutex_);
      if (handoff_.empty()) {
        break;
      }
      item = std::move(handoff_.front());
      handoff_.pop_front();
    }
    ExecutePayload(item.payload);
  }
}

void SyncChannel::DrainToPost(bool handoff) {
  for (;;) {
    EventItem item;
    {
      std::lock_guard<std::mutex> lock(queue_mutex_);
      std::deque<EventItem>& queue = handoff ? handoff_ : stash_;
      if (queue.empty()) {
        break;
      }
      item = std::move(queue.front());
      queue.pop_front();
    }
    Post(item.payload);
  }
}

}  // namespace jsapi_bridge
