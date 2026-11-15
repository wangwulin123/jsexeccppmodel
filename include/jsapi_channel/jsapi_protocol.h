#pragma once

#include <windows.h>

#include <string>

#include "jsapi_channel/jsapi_channel.h"

namespace jsapi_bridge {

// 渲染进程 <-> 浏览器进程的自定义进程消息名（异步请求/回包/对象释放）。
inline const char kAsyncRequestMessage[] = "jsapi.async.request";
inline const char kAsyncResponseMessage[] = "jsapi.async.response";
// 仅作异步事件单条超过事件槽容量时的回退通道（主路径已切到共享内存环形缓冲）。
inline const char kEventMessage[] = "jsapi.event";
// 渲染进程的 JS 代理被回收时发出，参数 0 是对象句柄。
inline const char kReleaseObjectMessage[] = "jsapi.object.release";

}  // namespace jsapi_bridge
