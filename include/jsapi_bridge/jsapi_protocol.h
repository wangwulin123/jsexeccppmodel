#pragma once

#include <windows.h>

#include <string>

namespace jsapi_bridge {

// 渲染进程 <-> 浏览器进程的自定义进程消息名（异步请求/回包/事件/对象释放）。
inline const char kAsyncRequestMessage[] = "jsapi.async.request";
inline const char kAsyncResponseMessage[] = "jsapi.async.response";
inline const char kEventMessage[] = "jsapi.event";
// 渲染进程的 JS 代理被回收时发出，参数 0 是对象句柄。
inline const char kReleaseObjectMessage[] = "jsapi.object.release";

// 同步 RPC 的共享内存通道。
//
// 同步调用发生渲染进程主线程的 V8 回调里，调用方会立刻阻塞；CEF 的
// CefFrame::SendProcessMessage 要等主线程回到消息循环才真正发出去，所以同步请求
// 不能走进程消息，而是写共享内存 + SetEvent 唤醒浏览器进程的等待线程。
struct SharedRequestHeader {
  LONG seq;
  LONG length;
};

inline constexpr int kRequestBufferBytes = 1 << 20;  // 1 MB
inline constexpr int kRequestCapacity =
    kRequestBufferBytes - static_cast<int>(sizeof(SharedRequestHeader));

inline const std::wstring& RequestMapName() {
  static const std::wstring name = L"Local\\jsapi_req";
  return name;
}

inline const std::wstring& RequestEventName() {
  static const std::wstring name = L"Local\\jsapi_req_evt";
  return name;
}

inline const std::wstring& RequestMutexName() {
  static const std::wstring name = L"Local\\jsapi_req_mutex";
  return name;
}

// 同步 RPC 的共享内存回包通道。
// 渲染进程在 V8 回调里阻塞等待时无法处理 CEF 消息循环，所以同步请求的回复不能走
// 进程消息，必须由浏览器进程直接写共享内存并 SetEvent 唤醒渲染进程。
struct SharedReplyHeader {
  volatile LONG ready;
  LONG seq;
  LONG length;
};

inline constexpr int kReplyBufferBytes = 1 << 20;  // 1 MB
inline constexpr int kReplyCapacity =
    kReplyBufferBytes - static_cast<int>(sizeof(SharedReplyHeader));

// 渲染进程侧共享内存/事件名（Local 命名空间，进程内可见）。
inline std::wstring ReplyMapName(DWORD pid) {
  return L"Local\\jsapi_reply_" + std::to_wstring(pid);
}

inline std::wstring ReplyEventName(DWORD pid) {
  return L"Local\\jsapi_reply_evt_" + std::to_wstring(pid);
}

}  // namespace jsapi_bridge
