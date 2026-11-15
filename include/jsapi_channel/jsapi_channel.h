#pragma once

#include <windows.h>

#include <mutex>
#include <string>
#include <vector>

#include "jsapi/js_value.h"

namespace jsapi_bridge {

// 嵌套深度与等待超时（方案 §9）。
inline constexpr int kMaxSyncDepth = 8;
inline constexpr int kDefaultTimeoutMs = 10000;

// 请求/回包槽与事件槽：统一 1MB，异步事件单条超限回退进程消息。
inline constexpr int kChannelBufferBytes = 1 << 20;
inline constexpr int kEventSlots = 4;
inline constexpr int kEventSlotBytes = 1 << 20;

// 请求通道上承载的两类消息：普通 RPC 与同步事件的回执。
enum MessageKind : long {
  kMessageRpc = 0,
  kMessageEventResult = 1,
};

// 所有通道名都按 channel_key 隔离（Local 命名空间）。
// key 取浏览器标识（browser_id）：浏览器进程与渲染进程都能在建立通道时直接得到
// 同一个值，因而无需任何进程消息握手就能对齐（渲染主线程在 OnContextCreated
// 里阻塞等待首个同步 RPC 时，渲染->浏览器的进程消息是发不出去的）。
struct ChannelNames {
  std::wstring request_map;
  std::wstring request_event;
  std::wstring request_mutex;
  std::wstring reply_map;
  std::wstring reply_event;
  std::wstring reply_mutex;
  std::wstring event_map;
  std::wstring event_mutex;
  std::wstring event_event;
};

ChannelNames MakeChannelNames(unsigned long pid);

// 请求槽：pending 是电平标志（写方置 1、读方清 0），命名事件只作加速唤醒。
struct SharedRequestHeader {
  volatile LONG pending;
  LONG seq;
  LONG kind;
  LONG invoke_id;
  LONG length;
};
inline constexpr int kRequestCapacity =
    kChannelBufferBytes - static_cast<int>(sizeof(SharedRequestHeader));

// 回包槽：与请求通道对称，reply_pending 电平 + seq 双校验。
struct SharedReplyHeader {
  volatile LONG pending;
  LONG seq;
  LONG status;  // 0 = ok，1 = 序列化后的错误响应
  LONG length;
};
inline constexpr int kReplyCapacity =
    kChannelBufferBytes - static_cast<int>(sizeof(SharedReplyHeader));

// 事件环形缓冲：槽满即失败（同步事件不丢），异步事件由调用方回退进程消息。
struct SharedEventSlot {
  volatile LONG length;  // -1 表示空槽
  LONG invoke_id;        // >0 同步事件，=0 异步事件
};
inline constexpr int kEventCapacity =
    kEventSlotBytes - static_cast<int>(sizeof(SharedEventSlot));

struct SharedEventHeader {
  LONG head;
  LONG count;
  LONG dropped;
};
inline constexpr int kEventMapBytes =
    static_cast<int>(sizeof(SharedEventHeader)) + kEventSlots * kEventSlotBytes;

// 渲染进程 -> 浏览器进程的请求写端。
class RequestWriter {
 public:
  explicit RequestWriter(unsigned long pid);
  ~RequestWriter();

  RequestWriter(const RequestWriter&) = delete;
  RequestWriter& operator=(const RequestWriter&) = delete;

  bool Ensure(std::string* error);
  bool Write(MessageKind kind,
             long seq,
             long invoke_id,
             const std::string& payload,
             std::string* error);

 private:
  ChannelNames names_;
  HANDLE map_ = nullptr;
  HANDLE mutex_ = nullptr;
  HANDLE event_ = nullptr;
  void* view_ = nullptr;
  std::mutex write_mutex_;
};

// 浏览器进程侧的请求读端。
class RequestReader {
 public:
  explicit RequestReader(unsigned long pid);
  ~RequestReader();

  RequestReader(const RequestReader&) = delete;
  RequestReader& operator=(const RequestReader&) = delete;

  bool Ensure(std::string* error);
  HANDLE event() const { return event_; }
  // 电平读取：pending 为 1 才读，读后清零。
  bool TryRead(MessageKind* kind,
               long* seq,
               long* invoke_id,
               std::string* payload);

 private:
  ChannelNames names_;
  HANDLE map_ = nullptr;
  HANDLE mutex_ = nullptr;
  HANDLE event_ = nullptr;
  void* view_ = nullptr;
};

// 浏览器进程 -> 渲染进程的回包写端。
class ReplyWriter {
 public:
  explicit ReplyWriter(unsigned long pid);
  ~ReplyWriter();

  ReplyWriter(const ReplyWriter&) = delete;
  ReplyWriter& operator=(const ReplyWriter&) = delete;

  bool Ensure(std::string* error);
  bool Write(long seq, const std::string& payload);
  // 作废槽内尚未被读取的回包（渲染进程崩溃/重启时调用）。
  void Reset();

 private:
  ChannelNames names_;
  HANDLE map_ = nullptr;
  HANDLE mutex_ = nullptr;
  HANDLE event_ = nullptr;
  void* view_ = nullptr;
};

// 渲染进程侧的回包读端。
class ReplyReader {
 public:
  explicit ReplyReader(unsigned long pid);
  ~ReplyReader();

  ReplyReader(const ReplyReader&) = delete;
  ReplyReader& operator=(const ReplyReader&) = delete;

  bool Ensure(std::string* error);
  HANDLE event() const { return event_; }
  // seq 不匹配时不清 pending、不消费，交给剩余超时继续等（迟到回包语义）。
  bool TryTake(long expected_seq, long* status, std::string* payload);
  // 调用方进入新的等待层级前作废旧回包。
  void Reset();

 private:
  ChannelNames names_;
  HANDLE map_ = nullptr;
  HANDLE mutex_ = nullptr;
  HANDLE event_ = nullptr;
  void* view_ = nullptr;
};

// 事件环形缓冲写端（浏览器进程）。
class EventRingWriter {
 public:
  explicit EventRingWriter(unsigned long pid);
  ~EventRingWriter();

  EventRingWriter(const EventRingWriter&) = delete;
  EventRingWriter& operator=(const EventRingWriter&) = delete;

  bool Ensure(std::string* error);
  bool Write(long invoke_id, const std::string& payload);
  // 丢弃环形缓冲里的全部滞留事件（渲染进程崩溃/重启时调用）。
  void Clear();
  // 单条事件能否放进一个槽；超过时调用方回退进程消息。
  static bool Fits(const std::string& payload);

 private:
  ChannelNames names_;
  HANDLE map_ = nullptr;
  HANDLE mutex_ = nullptr;
  HANDLE event_ = nullptr;
  void* view_ = nullptr;
};

// 事件环形缓冲读端（渲染进程 watcher 线程，唯一消费者）。
class EventRingReader {
 public:
  explicit EventRingReader(unsigned long pid);
  ~EventRingReader();

  EventRingReader(const EventRingReader&) = delete;
  EventRingReader& operator=(const EventRingReader&) = delete;

  bool Ensure(std::string* error);
  HANDLE event() const { return event_; }
  bool TryRead(long* invoke_id, std::string* payload);
  // 丢弃尚未读取的滞留事件（新渲染进程接入时调用）。
  void DiscardPending();

 private:
  ChannelNames names_;
  HANDLE map_ = nullptr;
  HANDLE mutex_ = nullptr;
  HANDLE event_ = nullptr;
  void* view_ = nullptr;
};

// 事件 payload 编解码（渲染进程与浏览器进程共用，单测可直接覆盖）。
std::string EncodeEventPayload(const std::string& name,
                               int callback_id,
                               int invoke_id,
                               const std::vector<jsapi::JsValue>& args,
                               const std::string& frame = std::string());
bool DecodeEventPayload(const std::string& payload,
                        std::string* name,
                        int* callback_id,
                        int* invoke_id,
                        std::vector<jsapi::JsValue>* args,
                        std::string* frame);

std::string EncodeEventResult(int invoke_id,
                              bool ok,
                              const jsapi::JsValue& value,
                              const std::string& error);
bool DecodeEventResult(const std::string& payload,
                       int* invoke_id,
                       bool* ok,
                       jsapi::JsValue* value,
                       std::string* error);

}  // namespace jsapi_bridge
