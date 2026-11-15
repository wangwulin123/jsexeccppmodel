#pragma once

#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "jsapi/js_object.h"

namespace jsapi {

// 浏览器进程侧的事件出口，由 bridge 实现：把事件转发到注册回调的渲染进程。
class JsEventSink {
 public:
  virtual ~JsEventSink() = default;
  virtual void OnEvent(const std::string& event,
                       int callback_id,
                       const RpcEnv& target,
                       const std::vector<JsValue>& args) = 0;

  // 同步事件（单播）：阻塞到目标页面回调返回、超时或失败。
  // 返回值语义：
  //   > 0  成功派发（命中的监听者数量）
  //   = 0  目标不可达（没有可派发的监听者），不阻塞
  //   < 0  sink 不支持同步事件，error 里给出原因
  // 默认实现立即返回 -1，存量 sink 无需改动即可编译通过。
  virtual int OnEventSync(const std::string& /*event*/,
                          int /*callback_id*/,
                          const RpcEnv& /*target*/,
                          const std::vector<JsValue>& /*args*/,
                          int /*timeout_ms*/,
                          JsValue* result,
                          std::string* error) {
    if (result) *result = JsValue::Null();
    if (error) *error = "sync events not supported by this sink";
    return -1;
  }
};

// 句柄注册表 + 命名节点工厂。
//
// handle 0 是根节点 window.Extention。业务插件通过 RegisterNode 注册命名节点，
// 前端用 Extention.demo 直接访问节点代理；所有对象返回给前端时都分配一个新的
// 自增句柄，跨进程只传句柄数字。
class JsRegistry {
 public:
  using Factory = std::function<std::shared_ptr<JsObjectBase>()>;

  // 句柄表快照，供前端/自测观察生命周期（Extention.objectStats / 调试用）。
  struct ObjectStats {
    int handles = 0;         // 已分配的句柄槽位总数（含已释放的空槽）
    int transient_live = 0;  // 仍存活的临时对象数量
    int release_refs = 0;    // 临时对象当前的代理引用计数总和
  };

  JsRegistry();
  ~JsRegistry();

  void SetEventSink(JsEventSink* sink);
  JsEventSink* event_sink() const { return event_sink_; }

  int AddObject(std::shared_ptr<JsObjectBase> object, const RpcEnv& env);
  std::shared_ptr<JsObjectBase> GetObjectByHandle(int handle);

  ObjectStats GetObjectStats() const;

  // 释放临时对象（LifeType::Transient）。渲染进程的 JS 代理被回收时调用；
  // 多个代理共享同一句柄时按引用计数，最后一个代理释放后对象才真正销毁。
  bool ReleaseObject(int handle);

  std::shared_ptr<JsObjectBase> Root(const RpcEnv& env);

  // 注册命名节点（工厂延迟执行）。静态注册与 DLL 插件加载都走这条路径。
  // 生命周期由节点类内的 JSAPI_NODE_LIFE_TYPE 决定，这里不接受覆盖。
  // 重复注册同名节点会保留已实例化对象的句柄，不会丢弃页面上已有的代理。
  void RegisterNode(const std::string& name, Factory factory);

  // 直接注册已经构造好的节点实例（不再延迟创建），仍会在首次访问时挂到句柄表。
  void RegisterNode(const std::string& name, std::shared_ptr<JsObjectBase> node);

  std::shared_ptr<JsObjectBase> GetNode(const std::string& name,
                                        const RpcEnv& env);
  std::vector<std::string> NodeNames() const;

  // 清理某个浏览器的失效事件监听器（见 JsObjectBase::DiscardListeners）。
  // keep_frame_id 为空表示清空该浏览器的全部监听（渲染进程已崩溃）。
  int DiscardListeners(int browser_id, const std::string& keep_frame_id);

  // 供 bridge 的 describe 使用。
  JsValue Describe(int handle);

 private:
  struct NodeRecord {
    Factory factory;
    int handle = -1;
  };

  // 调用方已持有 mutex_。
  int AddObjectLocked(std::shared_ptr<JsObjectBase> object, const RpcEnv& env);

  mutable std::mutex mutex_;
  std::vector<std::shared_ptr<JsObjectBase>> objects_;
  std::map<std::string, NodeRecord> nodes_;
  // 注册顺序（append-only），根节点属性按它排布，保证已下发到渲染进程的 index 不漂移。
  std::vector<std::string> node_order_;
  // 临时对象的代理引用计数：describe 出一层代理 +1，代理回收时 release -1。
  std::map<int, int> release_refs_;
  JsEventSink* event_sink_ = nullptr;
};

namespace plugins {

// 加载实现了 GetExtensionJsApiObj() 的动态库，返回延迟创建节点的工厂。
// 生命周期由插件节点类内的 JSAPI_NODE_LIFE_TYPE 决定，加载时不需要额外协商。
JsRegistry::Factory LoadDll(const std::string& path, std::string* error);

}  // namespace plugins

}  // namespace jsapi

// 动态插件入口：导出节点工厂。生命周期写在节点类里的 JSAPI_NODE_LIFE_TYPE。
//
//   JSAPI_EXPORT_EXTENSION(DemoNode);
#define JSAPI_EXPORT_EXTENSION(CLASSNAME)                  \
  extern "C" __declspec(dllexport) ::jsapi::JsObjectBase*  \
  GetExtensionJsApiObj() {                                 \
    return new CLASSNAME();                                \
  }
