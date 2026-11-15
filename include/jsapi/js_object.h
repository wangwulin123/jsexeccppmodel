#pragma once

#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "jsapi/js_table.h"
#include "jsapi/js_value.h"

namespace jsapi {

class JsRegistry;

// JS 上下文所在位置。事件需要按 (browser, frame) 送回注册回调的渲染进程。
struct RpcEnv {
  int browser_id = -1;
  std::string frame_id;

  bool valid() const { return browser_id >= 0 && !frame_id.empty(); }
};

// 一次 JSAPI 调用的上下文：插件通过它拿参数、写返回值或抛出错误。
class JsCallContext {
 public:
  JsCallContext(JsObjectBase* self,
                std::vector<JsValue> args,
                JsRegistry* registry,
                const RpcEnv& env);

  JsObjectBase* self() const { return self_; }
  const std::vector<JsValue>& args() const { return args_; }
  JsRegistry* registry() const { return registry_; }
  const RpcEnv& env() const { return env_; }

  void Return(JsValue value);
  void Fail(std::string error);

  bool has_value() const { return has_value_; }
  bool has_error() const { return !error_.empty(); }
  const JsValue& value() const { return value_; }
  const std::string& error() const { return error_; }

 private:
  JsObjectBase* self_ = nullptr;
  std::vector<JsValue> args_;
  JsRegistry* registry_ = nullptr;
  RpcEnv env_;

  bool has_value_ = false;
  JsValue value_;
  std::string error_;
};

// 节点生命周期
//  - Singleton：注册表按节点名缓存句柄，整个 browser 进程共用一个实例；
//  - Transient：每次取节点都新建实例，渲染进程的 JS 代理被回收时释放。
enum class LifeType {
  Singleton,
  Transient,
};

// 所有 JSAPI 节点的基类。
//
// 业务节点只需在子类里覆写 DeclareMembers() 填函数表：
//
//   void MyNode::DeclareMembers(JsTableBuilder& table) const {
//     table.Function("add", JsType::Int, {{"a", JsType::Int}, {"b", JsType::Int}},
//                    [](JsObjectBase*, JsCallContext& ctx) {
//                      ctx.Return(JsValue::Int(ctx.args()[0].AsInt() +
//                                             ctx.args()[1].AsInt()));
//                    });
//     table.Property("version", JsType::String,
//                    [](JsObjectBase*) { return JsValue::String("1.0"); });
//   }
//
// 基类会自动补上四个事件函数：addEventListener / removeEventListener /
// dispatchEvent / getAllEventInfo。
class JsObjectBase {
 public:
  virtual ~JsObjectBase() = default;

  const JsTable& GetTable() const;

  // 丢弃已缓存的函数表，下次 GetTable 重新 DeclareMembers。
  // 节点注册表变化（新增命名节点）时由 JsRegistry 调用。
  void InvalidateTable() const;

  // 由 JsRegistry 调用，绑定注册表、句柄与创建时的环境。
  void Attach(JsRegistry* registry, int handle, const RpcEnv& env);

  // 生命周期由类内的 JSAPI_NODE_LIFE_TYPE 写死，注册和运行时都不可修改。
  virtual LifeType life_type() const { return kLifeType; }

  // 类级生命周期声明（编译期常量），业务节点用 JSAPI_NODE_LIFE_TYPE 覆盖。
  static constexpr LifeType kLifeType = LifeType::Singleton;

  int handle() const { return handle_; }
  JsRegistry* registry() const { return registry_; }
  const RpcEnv& env() const { return env_; }

  bool AddEventListener(const std::string& event, int callback_id, const RpcEnv& env);
  bool RemoveEventListener(const std::string& event, int callback_id);
  int BroadcastEvent(const std::string& event, const std::vector<JsValue>& args);
  // 只派发给匹配 target 的监听者：browser_id < 0 表示不限 browser，
  // frame_id 为空表示不限 frame。业务/插件用它把事件定向送到某个页面。
  int DispatchEvent(const std::string& event,
                    const RpcEnv& target,
                    const std::vector<JsValue>& args);
  // 同步事件：按注册顺序取首个匹配 target 的监听者执行，阻塞等回调返回值。
  // error 为空表示成功；失败时返回 Null，并在 error 里给出可区分的原因：
  //   "no listener for event 'xxx'" / "sync event timed out ..." /
  //   "level misaligned, sync nesting rejected" / "event channel overflow" /
  //   "sync events not supported by this sink" 等。
  JsValue DispatchEventSync(const std::string& event,
                            const RpcEnv& target,
                            const std::vector<JsValue>& args,
                            int timeout_ms = 10000,
                            std::string* error = nullptr);
  JsValue GetAllEventInfo() const;

  // 丢弃某个浏览器的失效监听器：keep_frame_id 为空表示全部丢弃（渲染进程
  // 崩溃后所有回调都已不存在），否则保留该 frame（当前 document）的监听。
  // 返回被丢弃的数量。页面导航/刷新、渲染进程崩溃后由宿主调用。
  int DiscardListeners(int browser_id, const std::string& keep_frame_id);

 protected:
  // 子类重写：声明本节点的函数/属性表。
  virtual void DeclareMembers(JsTableBuilder& table) const {}

 private:
  struct Listener {
    std::string event;
    int callback_id = 0;
    RpcEnv env;
  };

  const JsTable& EnsureTable() const;

  // 事件派发的唯一实现：target 为空表示广播，exclude 命中的监听者跳过。
  int EmitTo(const std::string& event,
             const std::vector<JsValue>& args,
             const RpcEnv* target,
             const RpcEnv* exclude);

  JsRegistry* registry_ = nullptr;
  RpcEnv env_;
  int handle_ = -1;

  mutable std::mutex table_mutex_;
  mutable std::shared_ptr<const JsTable> table_;

  mutable std::mutex listener_mutex_;
  std::vector<Listener> listeners_;
};

}  // namespace jsapi

// 不写时继承基类默认值 Singleton。
#define JSAPI_NODE_LIFE_TYPE(TYPE)                        \
  static constexpr ::jsapi::LifeType kLifeType = (TYPE);  \
  ::jsapi::LifeType life_type() const override { return kLifeType; }
