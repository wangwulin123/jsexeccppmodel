#include <map>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

#include "jsapi/js_registry.h"
#include "jsapi_rpc/jsapi_rpc.h"
#include "support/test_fixtures.h"
#include "support/test_harness.h"

namespace {

test::CoreFixture& Fixture() {
  static test::CoreFixture fixture;
  return fixture;
}

// 在派发中途抛异常的宿主 sink，用于验证监听器之间的隔离。
class ThrowingEventSink final : public jsapi::JsEventSink {
 public:
  void OnEvent(const std::string&,
               int callback_id,
               const jsapi::RpcEnv&,
               const std::vector<jsapi::JsValue>&) override {
    delivered.push_back(callback_id);
    if (callback_id == 9101) {
      throw std::runtime_error("listener exploded");
    }
  }

  std::vector<int> delivered;
};

}  // namespace

TEST(reliability, event_register_broadcast_remove) {
  test::CoreFixture& f = Fixture();
  const jsapi::RpcResponse desc = test::Describe(&f.rpc, f.host_handle);
  const int add = test::FindMemberIndex(desc.value, "addEventListener");
  const int dispatch = test::FindMemberIndex(desc.value, "dispatchEvent");
  const int remove = test::FindMemberIndex(desc.value, "removeEventListener");
  test::Check(add >= 0 && dispatch >= 0 && remove >= 0,
              "base event function table present");

  f.sink.events.clear();
  const jsapi::RpcResponse add_res =
      test::Execute(&f.rpc, f.host_handle, add,
                    {jsapi::JsValue::String("hello"),
                     jsapi::JsValue::CallbackHandle(77)});
  test::Check(add_res.ok && add_res.value.AsBool(), "addEventListener",
              add_res.error);

  const jsapi::RpcResponse dispatch_res =
      test::Execute(&f.rpc, f.host_handle, dispatch,
                    {jsapi::JsValue::String("hello"),
                     jsapi::JsValue::Array({jsapi::JsValue::String("a"),
                                            jsapi::JsValue::Int(5)})});
  test::Check(dispatch_res.ok && dispatch_res.value.AsInt() == 1,
              "dispatchEvent reports 1 listener", dispatch_res.error);
  test::Check(f.sink.events.size() == 1 &&
                  f.sink.events[0].callback == 77 &&
                  f.sink.events[0].env.browser_id == 7 &&
                  f.sink.events[0].env.frame_id == "test-frame" &&
                  f.sink.events[0].args.size() == 2,
              "event sink receives callback target and args");

  const jsapi::RpcResponse remove_res =
      test::Execute(&f.rpc, f.host_handle, remove,
                    {jsapi::JsValue::String("hello"),
                     jsapi::JsValue::CallbackHandle(77)});
  test::Check(remove_res.ok && remove_res.value.AsBool(), "removeEventListener",
              remove_res.error);

  f.sink.events.clear();
  const jsapi::RpcResponse dispatch_again = test::Execute(
      &f.rpc, f.host_handle, dispatch, {jsapi::JsValue::String("hello")});
  test::Check(dispatch_again.ok && dispatch_again.value.AsInt() == 0 &&
                  f.sink.events.empty(),
              "no event delivered after removeEventListener",
              dispatch_again.error);
}

TEST(reliability, targeted_dispatch) {
  test::CoreFixture& f = Fixture();
  const jsapi::RpcResponse desc = test::Describe(&f.rpc, f.host_handle);
  const int dispatch = test::FindMemberIndex(desc.value, "dispatchEvent");
  test::Check(dispatch >= 0, "dispatchEvent present for targeted dispatch");

  auto host_object = f.registry.GetObjectByHandle(f.host_handle);
  test::Check(host_object != nullptr, "resolve host node object");
  if (!host_object) {
    return;
  }

  jsapi::RpcEnv frame_a;
  frame_a.browser_id = 7;
  frame_a.frame_id = "frame-a";
  jsapi::RpcEnv frame_b_other_browser;
  frame_b_other_browser.browser_id = 8;
  frame_b_other_browser.frame_id = "frame-b";
  jsapi::RpcEnv frame_b_same_browser;
  frame_b_same_browser.browser_id = 7;
  frame_b_same_browser.frame_id = "frame-b";
  const jsapi::RpcEnv caller_env = test::MakeEnv();  // 7 / test-frame

  host_object->AddEventListener("targeted", 101, frame_a);
  host_object->AddEventListener("targeted", 102, frame_b_other_browser);
  host_object->AddEventListener("targeted", 103, frame_b_same_browser);
  host_object->AddEventListener("targeted", 104, caller_env);

  f.sink.events.clear();
  const int exact = host_object->DispatchEvent(
      "targeted", frame_a, {jsapi::JsValue::String("only-a")});
  test::Check(exact == 1 && f.sink.events.size() == 1 &&
                  f.sink.events[0].callback == 101 &&
                  f.sink.events[0].env.frame_id == "frame-a",
              "DispatchEvent filters by frame");

  f.sink.events.clear();
  jsapi::RpcEnv browser_only;
  browser_only.browser_id = 7;
  const int by_browser =
      host_object->DispatchEvent("targeted", browser_only, {});
  test::Check(by_browser == 3 && f.sink.events.size() == 3,
              "DispatchEvent with empty frame matches all frames of browser");

  f.sink.events.clear();
  jsapi::RpcEnv frame_only;
  frame_only.frame_id = "frame-b";
  const int by_frame = host_object->DispatchEvent("targeted", frame_only, {});
  test::Check(by_frame == 2 && f.sink.events.size() == 2,
              "DispatchEvent with browser_id -1 matches every browser");

  f.sink.events.clear();
  jsapi::RpcEnv missing;
  missing.browser_id = 99;
  const int none = host_object->DispatchEvent("targeted", missing, {});
  test::Check(none == 0 && f.sink.events.empty(),
              "DispatchEvent to unknown browser delivers nothing");

  f.sink.events.clear();
  test::Check(host_object->BroadcastEvent("targeted", {}) == 4,
              "BroadcastEvent still reaches every listener");

  // JS 侧：dispatchEvent(event, args, { browser, frame, excludeSelf })
  f.sink.events.clear();
  std::map<std::string, jsapi::JsValue> target_options;
  target_options["browser"] = jsapi::JsValue::Int(8);
  target_options["frame"] = jsapi::JsValue::String("frame-b");
  const jsapi::RpcResponse js_target = test::Execute(
      &f.rpc, f.host_handle, dispatch,
      {jsapi::JsValue::String("targeted"),
       jsapi::JsValue::Array({jsapi::JsValue::String("payload")}),
       jsapi::JsValue::Object(target_options)});
  test::Check(js_target.ok && js_target.value.AsInt() == 1 &&
                  f.sink.events.size() == 1 && f.sink.events[0].callback == 102,
              "js dispatchEvent honors browser/frame target", js_target.error);

  f.sink.events.clear();
  std::map<std::string, jsapi::JsValue> exclude_options;
  exclude_options["browser"] = jsapi::JsValue::Int(7);
  exclude_options["excludeSelf"] = jsapi::JsValue::Bool(true);
  const jsapi::RpcResponse js_exclude = test::Execute(
      &f.rpc, f.host_handle, dispatch,
      {jsapi::JsValue::String("targeted"), jsapi::JsValue::Array({}),
       jsapi::JsValue::Object(exclude_options)});
  test::Check(js_exclude.ok && js_exclude.value.AsInt() == 2,
              "js dispatchEvent excludeSelf skips caller frame",
              js_exclude.error);
  for (const auto& record : f.sink.events) {
    test::Check(record.env.frame_id != caller_env.frame_id,
                "excludeSelf never delivers back to caller frame");
  }

  host_object->RemoveEventListener("targeted", 101);
  host_object->RemoveEventListener("targeted", 102);
  host_object->RemoveEventListener("targeted", 103);
  host_object->RemoveEventListener("targeted", 104);
  f.sink.events.clear();
  test::Check(host_object->BroadcastEvent("targeted", {}) == 0,
              "targeted listeners removed");
}

TEST(reliability, sync_events) {
  test::CoreFixture& f = Fixture();
  auto host_object = f.registry.GetObjectByHandle(f.host_handle);
  test::Check(host_object != nullptr, "resolve host node for sync events");
  if (!host_object) {
    return;
  }

  const jsapi::RpcEnv env = test::MakeEnv();
  const jsapi::RpcResponse desc = test::Describe(&f.rpc, f.host_handle);
  const int echo = test::FindMemberIndex(desc.value, "echo");

  f.sink.sync_handler = nullptr;
  f.sink.sync_calls.clear();
  host_object->AddEventListener("sync-evt", 201, env);

  std::string error;
  jsapi::JsValue result = host_object->DispatchEventSync(
      "sync-evt", env, {jsapi::JsValue::Int(7)}, 1000, &error);
  test::Check(error.empty() && result.type() == jsapi::JsType::String &&
                  result.AsString() == "sync:sync-evt",
              "DispatchEventSync returns callback value", error);
  test::Check(f.sink.sync_calls.size() == 1 &&
                  f.sink.sync_calls[0].callback == 201 &&
                  f.sink.sync_calls[0].args.size() == 1 &&
                  f.sink.sync_calls[0].args[0].AsInt() == 7,
              "DispatchEventSync forwards target/args");

  // 单播：同 env 注册两个监听者，只执行注册顺序上的第一个。
  host_object->AddEventListener("sync-evt", 202, env);
  f.sink.sync_calls.clear();
  error.clear();
  result = host_object->DispatchEventSync("sync-evt", env, {}, 1000, &error);
  test::Check(error.empty() && f.sink.sync_calls.size() == 1 &&
                  f.sink.sync_calls[0].callback == 201,
              "DispatchEventSync is unicast (first listener only)", error);
  host_object->RemoveEventListener("sync-evt", 202);

  // 无监听者：立即失败，不进入等待。
  error.clear();
  result = host_object->DispatchEventSync("sync-missing", env, {}, 100, &error);
  test::Check(result.is_null() && error.find("no listener") != std::string::npos,
              "DispatchEventSync without listener fails fast", error);

  // 多层嵌套：外层同步事件回调里先重入一次同步 RPC，再发起下一层同步事件。
  f.sink.sync_handler = [&](int /*callback_id*/,
                            const std::vector<jsapi::JsValue>& args,
                            jsapi::JsValue* out,
                            std::string* handler_error) -> int {
    const int depth = static_cast<int>(args.empty() ? 0 : args[0].AsInt());
    if (depth <= 1) {
      if (out) *out = jsapi::JsValue::Int(0);
      return 1;
    }
    // 重入 RpcHost：证明同步事件栈里可以再次执行插件函数。
    const jsapi::RpcResponse nested = test::Execute(
        &f.rpc, f.host_handle, echo, {jsapi::JsValue::String("nested")});
    if (!nested.ok) {
      if (handler_error) *handler_error = nested.error;
      return 1;
    }
    const jsapi::JsValue inner = host_object->DispatchEventSync(
        "chain", env, {jsapi::JsValue::Int(depth - 1)}, 1000, handler_error);
    if (handler_error && !handler_error->empty()) {
      return 1;
    }
    if (out) *out = jsapi::JsValue::Int(inner.AsInt() + 1);
    return 1;
  };

  host_object->AddEventListener("chain", 301, env);
  f.sink.sync_calls.clear();
  error.clear();
  result = host_object->DispatchEventSync("chain", env,
                                          {jsapi::JsValue::Int(4)}, 2000,
                                          &error);
  test::Check(error.empty() && result.AsInt() == 3,
              "nested sync events return the unwound value", error);
  test::Check(f.sink.sync_calls.size() == 4,
              "nested sync events re-enter one frame per level");

  host_object->RemoveEventListener("chain", 301);
  host_object->RemoveEventListener("sync-evt", 201);
  f.sink.sync_handler = nullptr;

  // 存量 sink 未实现 OnEventSync：默认实现给出明确错误，不影响编译与其它用例。
  test::PlainEventSink plain;
  f.registry.SetEventSink(&plain);
  host_object->AddEventListener("plain-evt", 401, env);
  error.clear();
  result = host_object->DispatchEventSync("plain-evt", env, {}, 100, &error);
  test::Check(result.is_null() &&
                  error.find("not supported by this sink") !=
                      std::string::npos,
              "sink without OnEventSync reports unsupported", error);
  host_object->RemoveEventListener("plain-evt", 401);
  f.registry.SetEventSink(&f.sink);
}

TEST(reliability, event_edge_cases) {
  test::CoreFixture& f = Fixture();
  auto host_object = f.registry.GetObjectByHandle(f.host_handle);
  test::Check(host_object != nullptr, "resolve host node for event edge cases");
  if (!host_object) {
    return;
  }
  const jsapi::RpcEnv env = test::MakeEnv();
  const jsapi::RpcResponse desc = test::Describe(&f.rpc, f.host_handle);

  // 同一监听重复添加：两次添加、两次投递；移除一次只去掉一个。
  f.sink.events.clear();
  test::Check(host_object->AddEventListener("dup", 1001, env) &&
                  host_object->AddEventListener("dup", 1001, env),
              "event: duplicate listener is accepted");
  test::Check(host_object->BroadcastEvent("dup", {}) == 2 &&
                  f.sink.events.size() == 2,
              "event: duplicate listener receives twice");
  test::Check(host_object->RemoveEventListener("dup", 1001) &&
                  host_object->BroadcastEvent("dup", {}) == 1,
              "event: removing one duplicate keeps the other");
  host_object->RemoveEventListener("dup", 1001);

  // 移除未注册监听 / 无效 callback handle。
  test::Check(!host_object->RemoveEventListener("never-added", 42),
              "event: removing an unknown listener returns false");
  test::Check(!host_object->RemoveEventListener("dup", -1),
              "event: invalid callback handle cannot be removed");
  test::Check(!host_object->AddEventListener("dup", -1, env),
              "event: negative callback id is rejected");
  test::Check(!host_object->AddEventListener("", 1, env),
              "event: empty event name is rejected");

  // 无监听时广播。
  f.sink.events.clear();
  test::Check(host_object->BroadcastEvent("nobody", {}) == 0 &&
                  f.sink.events.empty(),
              "event: broadcast without listeners returns 0");

  // JS 侧传入 {"$cb":-1}：AddEventListener 拒绝后必须把 false 返回给前端。
  const int add = test::FindMemberIndex(desc.value, "addEventListener");
  const jsapi::RpcResponse neg_cb = test::Execute(
      &f.rpc, f.host_handle, add,
      {jsapi::JsValue::String("neg-cb"), jsapi::JsValue::CallbackHandle(-1)});
  test::Check(neg_cb.ok && !neg_cb.value.AsBool(),
              "event: addEventListener rejects callback handle -1");

  // 监听器抛异常：其余监听器仍应收到事件，且异常不应逃逸到调用方。
  ThrowingEventSink throwing;
  f.registry.SetEventSink(&throwing);
  host_object->AddEventListener("throwing-evt", 9101, env);
  host_object->AddEventListener("throwing-evt", 9102, env);
  bool threw = false;
  try {
    host_object->BroadcastEvent("throwing-evt", {});
  } catch (...) {
    threw = true;
  }
  test::Check(
      !threw && throwing.delivered.size() == 2 &&
          throwing.delivered[1] == 9102,
      "event: one throwing listener does not block the rest");
  host_object->RemoveEventListener("throwing-evt", 9101);
  host_object->RemoveEventListener("throwing-evt", 9102);
  f.registry.SetEventSink(&f.sink);

  // 同步 emit 的嵌套深度：对象层没有人为上限，12 层必须完整回卷。
  f.sink.sync_handler = [&](int /*callback_id*/,
                            const std::vector<jsapi::JsValue>& args,
                            jsapi::JsValue* out,
                            std::string* handler_error) -> int {
    const int depth = static_cast<int>(args.empty() ? 0 : args[0].AsInt());
    if (depth <= 1) {
      if (out) *out = jsapi::JsValue::Int(0);
      return 1;
    }
    const jsapi::JsValue inner = host_object->DispatchEventSync(
        "deep", env, {jsapi::JsValue::Int(depth - 1)}, 5000, handler_error);
    if (handler_error && !handler_error->empty()) {
      return 1;
    }
    if (out) *out = jsapi::JsValue::Int(inner.AsInt() + 1);
    return 1;
  };
  host_object->AddEventListener("deep", 901, env);
  f.sink.sync_calls.clear();
  std::string error;
  const jsapi::JsValue deep_result = host_object->DispatchEventSync(
      "deep", env, {jsapi::JsValue::Int(12)}, 5000, &error);
  test::Check(error.empty() && deep_result.AsInt() == 11 &&
                  f.sink.sync_calls.size() == 12,
              "sync event: 12 nested emits unwind without an object-level cap",
              error);
  host_object->RemoveEventListener("deep", 901);
  f.sink.sync_handler = nullptr;
}
