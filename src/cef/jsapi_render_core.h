#pragma once

// 渲染进程侧核心：通道管理、RPC 收发、V8 <-> JsValue 转换、回调表。
// 只给 jsapi_bridge 内部使用（V8 代理与公开入口共享）。

#include <atomic>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "include/cef_process_message.h"
#include "jsapi_render_types.h"
#include "jsapi_rpc/jsapi_rpc.h"

namespace jsapi_bridge {

class RendererCore {
 public:
  RendererCore();
  ~RendererCore();

  bool BuildRootObject(CefRefPtr<CefFrame> frame, CefRefPtr<CefV8Context> context);

  // 按浏览器标识建立同步通道（两端各自 Ensure，无需进程消息握手）。
  std::shared_ptr<SyncChannel> EnsureChannel(CefRefPtr<CefFrame> frame);

  CefRefPtr<CefV8Value> BuildProxy(const std::shared_ptr<const ObjectInfo>& info,
                                   const BindingPtr& binding,
                                   int depth);
  CefRefPtr<CefV8Value> ResolveHandle(int handle,
                                      const BindingPtr& binding,
                                      int depth);

  bool ToV8(const jsapi::JsValue& value,
            const BindingPtr& binding,
            CefRefPtr<CefV8Value>* out,
            int depth);
  bool FromV8(const CefRefPtr<CefV8Value>& value,
              const BindingPtr& binding,
              jsapi::JsValue* out,
              std::string* error);

  bool InvokeSync(const jsapi::RpcRequest& request,
                  const BindingPtr& binding,
                  jsapi::RpcResponse* response,
                  std::string* error);
  void InvokeAsync(const jsapi::RpcRequest& request,
                   const BindingPtr& binding,
                   CefRefPtr<CefV8Value> promise);

  int RegisterCallback(const BindingPtr& binding,
                       CefRefPtr<CefV8Value> function);
  int FindCallbackId(const BindingPtr& binding,
                     const CefRefPtr<CefV8Value>& function);

  // 在渲染主线程（V8 上下文内）执行事件回调，异常转成 error 不穿透到 C++。
  bool DispatchEvent(const std::string& name,
                     int callback_id,
                     const std::vector<jsapi::JsValue>& args,
                     jsapi::JsValue* result,
                     std::string* error);

  void HandleAsyncResponse(CefRefPtr<CefProcessMessage> message);
  void HandleEvent(CefRefPtr<CefProcessMessage> message);
  void ReleaseContext(const CefRefPtr<CefV8Context>& context);

 private:
  struct CallbackEntry {
    BindingPtr binding;
    CefRefPtr<CefV8Value> function;
  };

  struct PendingPromise {
    BindingPtr binding;
    CefRefPtr<CefV8Value> promise;
    int seq = 0;
  };

  bool DescribeHandle(int handle,
                      const BindingPtr& binding,
                      std::shared_ptr<ObjectInfo>* info,
                      std::string* error);

  int NextSeq();
  int NextCallbackId();
  // 当前 V8 上下文是否就是目标上下文（避免多余 Enter/Exit 触发微任务检查点）。
  static bool IsCurrentContext(const CefRefPtr<CefV8Context>& context);

  // 渲染进程可能承载多个浏览器（多窗口/多 web 页）：每个 browser_id 一条
  // 独立通道，避免同一渲染进程里不同页面的同步调用互相串台。
  std::mutex channels_mutex_;
  std::map<int, std::shared_ptr<SyncChannel>> channels_;
  // 已投递但尚未执行的渲染主线程任务在 RendererCore 析构后不得再访问 this。
  std::shared_ptr<std::atomic<bool>> alive_ =
      std::make_shared<std::atomic<bool>>(true);
  std::mutex seq_mutex_;
  // 0 表示尚未播种：首次 NextSeq() 用时间戳初始化，避免渲染进程重启后撞号。
  int next_seq_ = 0;
  int next_callback_ = 1;

  std::map<int, PendingPromise> pending_;
  std::map<int, CallbackEntry> callbacks_;
};

}  // namespace jsapi_bridge
