#pragma once

// 渲染进程侧内部共享类型（不对外导出）。
// SyncChannel / RendererCore / V8 代理分别编译在不同 TU 里，但都围绕同一份
// "句柄描述 + frame 绑定" 工作，因此这些描述结构单独放一个内部头文件。

#include <memory>
#include <string>
#include <vector>

#include "include/cef_frame.h"
#include "include/cef_v8.h"
#include "jsapi_rpc/jsapi_rpc.h"

namespace jsapi_bridge {

class SyncChannel;

struct MemberInfo {
  std::string name;
  bool is_property = false;
  int index = -1;
  jsapi::JsType type = jsapi::JsType::Null;
  bool async = false;
  bool writable = false;
};

struct ObjectInfo {
  int handle = 0;
  std::vector<MemberInfo> members;

  const MemberInfo* FindByName(const std::string& name) const {
    for (const auto& member : members) {
      if (member.name == name) return &member;
    }
    return nullptr;
  }
};

struct Binding {
  CefRefPtr<CefV8Context> context;
  CefRefPtr<CefFrame> frame;
  jsapi::RpcEnv env;
  // 该 frame 所属浏览器（browser_id）的同步通道。多个浏览器可能共享同一个
  // 渲染进程，每个浏览器必须各自一条通道，绑定把通道和代理绑定在一起。
  std::shared_ptr<SyncChannel> channel;
};
using BindingPtr = std::shared_ptr<Binding>;

}  // namespace jsapi_bridge
