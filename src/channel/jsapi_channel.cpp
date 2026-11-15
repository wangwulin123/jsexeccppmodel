#include "jsapi_channel/jsapi_channel.h"

#include <cstring>
#include <map>

namespace jsapi_bridge {
namespace {

// 共享内存 + 命名同步对象的通用小工具：任何一端先创建都能拿到同一个对象。
struct ChannelHandles {
  HANDLE map = nullptr;
  HANDLE mutex = nullptr;
  HANDLE event = nullptr;
  void* view = nullptr;

  ~ChannelHandles() { Close(); }

  void Close() {
    if (view) {
      ::UnmapViewOfFile(view);
      view = nullptr;
    }
    if (event) {
      ::CloseHandle(event);
      event = nullptr;
    }
    if (mutex) {
      ::CloseHandle(mutex);
      mutex = nullptr;
    }
    if (map) {
      ::CloseHandle(map);
      map = nullptr;
    }
  }
};

bool OpenMap(const std::wstring& name, int bytes, ChannelHandles* out,
             std::string* error) {
  out->map = ::CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE,
                                  0, static_cast<DWORD>(bytes), name.c_str());
  if (!out->map) {
    if (error) {
      *error = "CreateFileMapping failed, error=" +
               std::to_string(::GetLastError());
    }
    return false;
  }
  out->view = ::MapViewOfFile(out->map, FILE_MAP_WRITE, 0, 0, 0);
  if (!out->view) {
    if (error) {
      *error = "MapViewOfFile failed, error=" +
               std::to_string(::GetLastError());
    }
    return false;
  }
  return true;
}

bool OpenMutex(const std::wstring& name, ChannelHandles* out,
               std::string* error) {
  out->mutex = ::CreateMutexW(nullptr, FALSE, name.c_str());
  if (!out->mutex) {
    if (error) {
      *error = "CreateMutex failed, error=" + std::to_string(::GetLastError());
    }
    return false;
  }
  return true;
}

bool OpenAutoEvent(const std::wstring& name, ChannelHandles* out,
                   std::string* error) {
  out->event = ::CreateEventW(nullptr, FALSE, FALSE, name.c_str());
  if (!out->event) {
    if (error) {
      *error = "CreateEvent failed, error=" + std::to_string(::GetLastError());
    }
    return false;
  }
  return true;
}

void SetLengthClamped(LONG* length, const std::string& payload, int capacity) {
  LONG value = static_cast<LONG>(payload.size());
  if (value < 0) value = 0;
  if (value > capacity) value = capacity;
  *length = value;
}

// 读槽里的负载；调用方已持有互斥量。
std::string ReadSlot(const void* header, LONG length, long header_size,
                     int capacity) {
  if (length < 0) length = 0;
  if (length > capacity) length = capacity;
  return std::string(reinterpret_cast<const char*>(header) + header_size,
                     static_cast<std::size_t>(length));
}

SharedEventSlot* EventSlotAt(void* view, int index) {
  auto* base = static_cast<char*>(view) + sizeof(SharedEventHeader);
  return reinterpret_cast<SharedEventSlot*>(base + index * kEventSlotBytes);
}

const SharedEventSlot* EventSlotAt(const void* view, int index) {
  auto* base = static_cast<const char*>(view) + sizeof(SharedEventHeader);
  return reinterpret_cast<const SharedEventSlot*>(base +
                                                  index * kEventSlotBytes);
}

}  // namespace

ChannelNames MakeChannelNames(unsigned long pid) {
  const std::wstring suffix = std::to_wstring(pid);
  ChannelNames names;
  names.request_map = L"Local\\jsapi_req_" + suffix;
  names.request_event = L"Local\\jsapi_req_evt_" + suffix;
  names.request_mutex = L"Local\\jsapi_req_mutex_" + suffix;
  names.reply_map = L"Local\\jsapi_reply_" + suffix;
  names.reply_event = L"Local\\jsapi_reply_evt_" + suffix;
  names.reply_mutex = L"Local\\jsapi_reply_mutex_" + suffix;
  names.event_map = L"Local\\jsapi_evt_" + suffix;
  names.event_mutex = L"Local\\jsapi_evt_mutex_" + suffix;
  names.event_event = L"Local\\jsapi_evt_event_" + suffix;
  return names;
}

// ---------------------------------------------------------------------------
// RequestWriter
// ---------------------------------------------------------------------------
RequestWriter::RequestWriter(unsigned long pid)
    : names_(MakeChannelNames(pid)) {}

RequestWriter::~RequestWriter() {
  if (view_) ::UnmapViewOfFile(view_);
  if (event_) ::CloseHandle(event_);
  if (mutex_) ::CloseHandle(mutex_);
  if (map_) ::CloseHandle(map_);
}

bool RequestWriter::Ensure(std::string* error) {
  if (view_ && mutex_ && event_) {
    return true;
  }
  ChannelHandles handles;
  if (!OpenMap(names_.request_map, kChannelBufferBytes, &handles, error) ||
      !OpenMutex(names_.request_mutex, &handles, error) ||
      !OpenAutoEvent(names_.request_event, &handles, error)) {
    return false;
  }
  map_ = handles.map;
  mutex_ = handles.mutex;
  event_ = handles.event;
  view_ = handles.view;
  handles.map = handles.mutex = handles.event = nullptr;
  handles.view = nullptr;
  return true;
}

bool RequestWriter::Write(MessageKind kind,
                          long seq,
                          long invoke_id,
                          const std::string& payload,
                          std::string* error) {
  std::lock_guard<std::mutex> local_lock(write_mutex_);
  if (!Ensure(error)) {
    return false;
  }
  if (payload.size() > static_cast<std::size_t>(kRequestCapacity)) {
    if (error) *error = "sync jsapi request exceeds shared-memory capacity";
    return false;
  }

  if (::WaitForSingleObject(mutex_, 5000) != WAIT_OBJECT_0) {
    if (error) *error = "sync jsapi request mutex timeout";
    return false;
  }
  auto* header = static_cast<SharedRequestHeader*>(view_);
  header->seq = static_cast<LONG>(seq);
  header->kind = static_cast<LONG>(kind);
  header->invoke_id = static_cast<LONG>(invoke_id);
  SetLengthClamped(&header->length, payload, kRequestCapacity);
  std::memcpy(header + 1, payload.data(), static_cast<std::size_t>(header->length));
  // 电平置位必须放在数据写完之后，读方以该标志为准。
  ::InterlockedExchange(&header->pending, 1);
  ::ReleaseMutex(mutex_);
  ::SetEvent(event_);
  return true;
}

// ---------------------------------------------------------------------------
// RequestReader
// ---------------------------------------------------------------------------
RequestReader::RequestReader(unsigned long pid) : names_(MakeChannelNames(pid)) {}

RequestReader::~RequestReader() {
  if (view_) ::UnmapViewOfFile(view_);
  if (event_) ::CloseHandle(event_);
  if (mutex_) ::CloseHandle(mutex_);
  if (map_) ::CloseHandle(map_);
}

bool RequestReader::Ensure(std::string* error) {
  if (view_ && mutex_ && event_) {
    return true;
  }
  ChannelHandles handles;
  if (!OpenMap(names_.request_map, kChannelBufferBytes, &handles, error) ||
      !OpenMutex(names_.request_mutex, &handles, error) ||
      !OpenAutoEvent(names_.request_event, &handles, error)) {
    return false;
  }
  map_ = handles.map;
  mutex_ = handles.mutex;
  event_ = handles.event;
  view_ = handles.view;
  handles.map = handles.mutex = handles.event = nullptr;
  handles.view = nullptr;
  return true;
}

bool RequestReader::TryRead(MessageKind* kind,
                            long* seq,
                            long* invoke_id,
                            std::string* payload) {
  if (!view_ || !mutex_) {
    return false;
  }
  if (::WaitForSingleObject(mutex_, 5000) != WAIT_OBJECT_0) {
    return false;
  }
  auto* header = static_cast<SharedRequestHeader*>(view_);
  if (::InterlockedCompareExchange(&header->pending, 1, 1) != 1) {
    ::ReleaseMutex(mutex_);
    return false;
  }
  if (kind) *kind = static_cast<MessageKind>(header->kind);
  if (seq) *seq = static_cast<long>(header->seq);
  if (invoke_id) *invoke_id = static_cast<long>(header->invoke_id);
  if (payload) {
    *payload = ReadSlot(header, header->length, sizeof(SharedRequestHeader),
                        kRequestCapacity);
  }
  ::InterlockedExchange(&header->pending, 0);
  ::ReleaseMutex(mutex_);
  return true;
}

// ---------------------------------------------------------------------------
// ReplyWriter
// ---------------------------------------------------------------------------
ReplyWriter::ReplyWriter(unsigned long pid) : names_(MakeChannelNames(pid)) {}

ReplyWriter::~ReplyWriter() {
  if (view_) ::UnmapViewOfFile(view_);
  if (event_) ::CloseHandle(event_);
  if (mutex_) ::CloseHandle(mutex_);
  if (map_) ::CloseHandle(map_);
}

bool ReplyWriter::Ensure(std::string* error) {
  if (view_ && mutex_ && event_) {
    return true;
  }
  ChannelHandles handles;
  if (!OpenMap(names_.reply_map, kChannelBufferBytes, &handles, error) ||
      !OpenMutex(names_.reply_mutex, &handles, error) ||
      !OpenAutoEvent(names_.reply_event, &handles, error)) {
    return false;
  }
  map_ = handles.map;
  mutex_ = handles.mutex;
  event_ = handles.event;
  view_ = handles.view;
  handles.map = handles.mutex = handles.event = nullptr;
  handles.view = nullptr;
  return true;
}

bool ReplyWriter::Write(long seq, const std::string& payload) {
  if (!Ensure(nullptr)) {
    return false;
  }
  // 与请求侧一致：超容量显式失败，绝不截断后当成完整回包投递。
  // 上层（WriteReply）会预先检查容量并改发错误回包，这里是防御性兜底。
  if (payload.size() > static_cast<std::size_t>(kReplyCapacity)) {
    return false;
  }
  if (::WaitForSingleObject(mutex_, 5000) != WAIT_OBJECT_0) {
    return false;
  }
  auto* header = static_cast<SharedReplyHeader*>(view_);
  header->seq = static_cast<LONG>(seq);
  header->status = 0;
  SetLengthClamped(&header->length, payload, kReplyCapacity);
  std::memcpy(header + 1, payload.data(), static_cast<std::size_t>(header->length));
  ::InterlockedExchange(&header->pending, 1);
  ::ReleaseMutex(mutex_);
  ::SetEvent(event_);
  return true;
}

void ReplyWriter::Reset() {
  if (!Ensure(nullptr)) {
    return;
  }
  if (::WaitForSingleObject(mutex_, 5000) != WAIT_OBJECT_0) {
    return;
  }
  auto* header = static_cast<SharedReplyHeader*>(view_);
  header->length = 0;
  header->seq = 0;
  ::InterlockedExchange(&header->pending, 0);
  ::ReleaseMutex(mutex_);
}

// ---------------------------------------------------------------------------
// ReplyReader
// ---------------------------------------------------------------------------
ReplyReader::ReplyReader(unsigned long pid) : names_(MakeChannelNames(pid)) {}

ReplyReader::~ReplyReader() {
  if (view_) ::UnmapViewOfFile(view_);
  if (event_) ::CloseHandle(event_);
  if (mutex_) ::CloseHandle(mutex_);
  if (map_) ::CloseHandle(map_);
}

bool ReplyReader::Ensure(std::string* error) {
  if (view_ && mutex_ && event_) {
    return true;
  }
  ChannelHandles handles;
  if (!OpenMap(names_.reply_map, kChannelBufferBytes, &handles, error) ||
      !OpenMutex(names_.reply_mutex, &handles, error) ||
      !OpenAutoEvent(names_.reply_event, &handles, error)) {
    return false;
  }
  map_ = handles.map;
  mutex_ = handles.mutex;
  event_ = handles.event;
  view_ = handles.view;
  handles.map = handles.mutex = handles.event = nullptr;
  handles.view = nullptr;
  return true;
}

bool ReplyReader::TryTake(long expected_seq, long* status, std::string* payload) {
  if (!view_ || !mutex_) {
    return false;
  }
  if (::WaitForSingleObject(mutex_, 5000) != WAIT_OBJECT_0) {
    return false;
  }
  auto* header = static_cast<SharedReplyHeader*>(view_);
  if (::InterlockedCompareExchange(&header->pending, 1, 1) != 1 ||
      header->seq != static_cast<LONG>(expected_seq)) {
    // 迟到回包：保留 pending，等正确的回包覆盖它。
    ::ReleaseMutex(mutex_);
    return false;
  }
  if (status) *status = static_cast<long>(header->status);
  if (payload) {
    *payload = ReadSlot(header, header->length, sizeof(SharedReplyHeader),
                        kReplyCapacity);
  }
  ::InterlockedExchange(&header->pending, 0);
  ::ReleaseMutex(mutex_);
  return true;
}

void ReplyReader::Reset() {
  if (!view_ || !mutex_) {
    return;
  }
  if (::WaitForSingleObject(mutex_, 5000) != WAIT_OBJECT_0) {
    return;
  }
  auto* header = static_cast<SharedReplyHeader*>(view_);
  header->length = 0;
  header->seq = 0;
  ::InterlockedExchange(&header->pending, 0);
  ::ReleaseMutex(mutex_);
}

// ---------------------------------------------------------------------------
// EventRingWriter / EventRingReader
// ---------------------------------------------------------------------------
EventRingWriter::EventRingWriter(unsigned long pid) : names_(MakeChannelNames(pid)) {}

EventRingWriter::~EventRingWriter() {
  if (view_) ::UnmapViewOfFile(view_);
  if (event_) ::CloseHandle(event_);
  if (mutex_) ::CloseHandle(mutex_);
  if (map_) ::CloseHandle(map_);
}

bool EventRingWriter::Ensure(std::string* error) {
  if (view_ && mutex_ && event_) {
    return true;
  }
  ChannelHandles handles;
  if (!OpenMap(names_.event_map, kEventMapBytes, &handles, error) ||
      !OpenMutex(names_.event_mutex, &handles, error) ||
      !OpenAutoEvent(names_.event_event, &handles, error)) {
    return false;
  }
  map_ = handles.map;
  mutex_ = handles.mutex;
  event_ = handles.event;
  view_ = handles.view;
  handles.map = handles.mutex = handles.event = nullptr;
  handles.view = nullptr;
  auto* header = static_cast<SharedEventHeader*>(view_);
  for (int i = 0; i < kEventSlots; ++i) {
    ::InterlockedExchange(&EventSlotAt(view_, i)->length, -1);
  }
  header->head = 0;
  header->count = 0;
  header->dropped = 0;
  return true;
}

bool EventRingWriter::Fits(const std::string& payload) {
  return payload.size() <= static_cast<std::size_t>(kEventCapacity);
}

void EventRingWriter::Clear() {
  if (!Ensure(nullptr)) {
    return;
  }
  if (::WaitForSingleObject(mutex_, 5000) != WAIT_OBJECT_0) {
    return;
  }
  auto* header = static_cast<SharedEventHeader*>(view_);
  for (int i = 0; i < kEventSlots; ++i) {
    ::InterlockedExchange(&EventSlotAt(view_, i)->length, -1);
  }
  header->head = 0;
  header->count = 0;
  ::ReleaseMutex(mutex_);
}

bool EventRingWriter::Write(long invoke_id, const std::string& payload) {
  if (!Ensure(nullptr)) {
    return false;
  }
  if (!Fits(payload)) {
    // 单条超限：同步事件直接失败；异步事件由调用方回退进程消息。
    return false;
  }
  if (::WaitForSingleObject(mutex_, 5000) != WAIT_OBJECT_0) {
    return false;
  }
  auto* header = static_cast<SharedEventHeader*>(view_);
  bool ok = false;
  if (header->count >= 0 && header->count < kEventSlots) {
    const int index = (header->head + header->count) % kEventSlots;
    SharedEventSlot* slot = EventSlotAt(view_, index);
    slot->invoke_id = static_cast<LONG>(invoke_id);
    ::InterlockedExchange(&slot->length, static_cast<LONG>(payload.size()));
    std::memcpy(slot + 1, payload.data(), payload.size());
    // length 是槽的有效标志：先写数据再置长度，读方看不到半截数据。
    header->count += 1;
    ok = true;
  } else {
    header->dropped += 1;
  }
  ::ReleaseMutex(mutex_);
  if (ok) {
    ::SetEvent(event_);
  }
  return ok;
}

EventRingReader::EventRingReader(unsigned long pid) : names_(MakeChannelNames(pid)) {}

EventRingReader::~EventRingReader() {
  if (view_) ::UnmapViewOfFile(view_);
  if (event_) ::CloseHandle(event_);
  if (mutex_) ::CloseHandle(mutex_);
  if (map_) ::CloseHandle(map_);
}

bool EventRingReader::Ensure(std::string* error) {
  if (view_ && mutex_ && event_) {
    return true;
  }
  ChannelHandles handles;
  if (!OpenMap(names_.event_map, kEventMapBytes, &handles, error) ||
      !OpenMutex(names_.event_mutex, &handles, error) ||
      !OpenAutoEvent(names_.event_event, &handles, error)) {
    return false;
  }
  map_ = handles.map;
  mutex_ = handles.mutex;
  event_ = handles.event;
  view_ = handles.view;
  handles.map = handles.mutex = handles.event = nullptr;
  handles.view = nullptr;
  return true;
}

bool EventRingReader::TryRead(long* invoke_id, std::string* payload) {
  if (!view_ || !mutex_) {
    return false;
  }
  if (::WaitForSingleObject(mutex_, 5000) != WAIT_OBJECT_0) {
    return false;
  }
  auto* header = static_cast<SharedEventHeader*>(view_);
  bool ok = false;
  if (header->count > 0) {
    SharedEventSlot* slot = EventSlotAt(view_, header->head);
    const LONG length = ::InterlockedCompareExchange(&slot->length, 0, 0);
    if (length >= 0) {
      if (invoke_id) *invoke_id = static_cast<long>(slot->invoke_id);
      if (payload) {
        LONG clamped = length;
        if (clamped > kEventCapacity) clamped = kEventCapacity;
        *payload = std::string(reinterpret_cast<const char*>(slot) +
                                   sizeof(SharedEventSlot),
                               static_cast<std::size_t>(clamped));
      }
      ::InterlockedExchange(&slot->length, -1);
      header->head = (header->head + 1) % kEventSlots;
      header->count -= 1;
      ok = true;
    }
  }
  ::ReleaseMutex(mutex_);
  return ok;
}

void EventRingReader::DiscardPending() {
  if (!view_ || !mutex_) {
    return;
  }
  if (::WaitForSingleObject(mutex_, 5000) != WAIT_OBJECT_0) {
    return;
  }
  auto* header = static_cast<SharedEventHeader*>(view_);
  for (int i = 0; i < kEventSlots; ++i) {
    ::InterlockedExchange(&EventSlotAt(view_, i)->length, -1);
  }
  header->head = 0;
  header->count = 0;
  ::ReleaseMutex(mutex_);
}

// ---------------------------------------------------------------------------
// 事件 payload / 回执编解码
// ---------------------------------------------------------------------------
std::string EncodeEventPayload(const std::string& name,
                               int callback_id,
                               int invoke_id,
                               const std::vector<jsapi::JsValue>& args,
                               const std::string& frame) {
  jsapi::JsValue root = jsapi::JsValue::Object();
  root.Set("name", jsapi::JsValue::String(name));
  root.Set("cb", jsapi::JsValue::Int(callback_id));
  root.Set("invoke_id", jsapi::JsValue::Int(invoke_id));
  root.Set("args", jsapi::JsValue::Array(args));
  if (!frame.empty()) {
    root.Set("frame", jsapi::JsValue::String(frame));
  }
  return root.ToJson();
}

bool DecodeEventPayload(const std::string& payload,
                        std::string* name,
                        int* callback_id,
                        int* invoke_id,
                        std::vector<jsapi::JsValue>* args,
                        std::string* frame) {
  jsapi::JsValue root;
  if (!jsapi::ParseJson(payload, &root, nullptr)) {
    return false;
  }
  const jsapi::JsValue* name_value = root.Find("name");
  if (!name_value || name_value->type() != jsapi::JsType::String) {
    return false;
  }
  if (name) *name = name_value->AsString();
  if (callback_id) {
    const jsapi::JsValue* cb = root.Find("cb");
    *callback_id = cb ? static_cast<int>(cb->AsInt()) : -1;
  }
  if (invoke_id) {
    const jsapi::JsValue* id = root.Find("invoke_id");
    *invoke_id = id ? static_cast<int>(id->AsInt()) : 0;
  }
  if (args) {
    const jsapi::JsValue* list = root.Find("args");
    *args = (list && list->type() == jsapi::JsType::Array) ? list->AsArray()
                                                           : std::vector<jsapi::JsValue>{};
  }
  if (frame) {
    const jsapi::JsValue* value = root.Find("frame");
    *frame = value ? value->AsString() : std::string();
  }
  return true;
}

std::string EncodeEventResult(int invoke_id,
                              bool ok,
                              const jsapi::JsValue& value,
                              const std::string& error) {
  jsapi::JsValue root = jsapi::JsValue::Object();
  root.Set("op", jsapi::JsValue::String("event_result"));
  root.Set("invoke_id", jsapi::JsValue::Int(invoke_id));
  root.Set("ok", jsapi::JsValue::Bool(ok));
  if (ok) {
    root.Set("value", value);
  } else {
    root.Set("error", jsapi::JsValue::String(error));
  }
  return root.ToJson();
}

bool DecodeEventResult(const std::string& payload,
                       int* invoke_id,
                       bool* ok,
                       jsapi::JsValue* value,
                       std::string* error) {
  jsapi::JsValue root;
  if (!jsapi::ParseJson(payload, &root, nullptr)) {
    return false;
  }
  const jsapi::JsValue* id = root.Find("invoke_id");
  if (!id) {
    return false;
  }
  if (invoke_id) *invoke_id = static_cast<int>(id->AsInt());
  const jsapi::JsValue* ok_value = root.Find("ok");
  const bool success = ok_value && ok_value->AsBool();
  if (ok) *ok = success;
  if (success) {
    if (value) {
      const jsapi::JsValue* result = root.Find("value");
      *value = result ? *result : jsapi::JsValue::Null();
    }
    if (error) error->clear();
  } else {
    if (value) *value = jsapi::JsValue::Null();
    if (error) {
      const jsapi::JsValue* reason = root.Find("error");
      *error = reason ? reason->AsString() : std::string("event callback failed");
    }
  }
  return true;
}

}  // namespace jsapi_bridge
