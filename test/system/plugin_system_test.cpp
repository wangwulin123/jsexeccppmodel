#include <string>
#include <utility>

#include "host_nodes.h"
#include "jsapi/js_registry.h"
#include "jsapi/jsapi_version.h"
#include "jsapi_rpc/jsapi_rpc.h"
#include "support/test_fixtures.h"
#include "support/test_harness.h"

namespace {

test::CoreFixture& Fixture() {
  static test::CoreFixture fixture;
  return fixture;
}

jsapi::JsRegistry::Factory LoadPlugin(std::string* error) {
  return jsapi::plugins::LoadDll(test::PluginPath(), error);
}

}  // namespace

TEST(system, plugin_full_chain) {
  test::CoreFixture& f = Fixture();
  std::string load_error;
  jsapi::JsRegistry::Factory factory = LoadPlugin(&load_error);
  test::Check(factory != nullptr,
              "load demo_plugin.dll (" + test::PluginPath() + ")", load_error);
  if (!factory) {
    return;
  }

  f.registry.RegisterNode("demo", std::move(factory));

  const int demo_handle = test::GetNodeHandle(&f.rpc, "demo");
  test::Check(demo_handle >= 0, "Extention.demo returns handle");
  if (demo_handle < 0) {
    return;
  }

  const jsapi::RpcResponse desc = test::Describe(&f.rpc, demo_handle);
  test::Check(desc.ok, "demo describe ok", desc.error);

  const int add = test::FindMemberIndex(desc.value, "add");
  const int sum_async = test::FindMemberIndex(desc.value, "sumAsync");
  const int fail = test::FindMemberIndex(desc.value, "fail");
  const int child = test::FindMemberIndex(desc.value, "child");
  const int version = test::FindMemberIndex(desc.value, "version");
  const int count = test::FindMemberIndex(desc.value, "count");
  test::Check(add >= 0 && sum_async >= 0 && fail >= 0 && child >= 0,
              "plugin function table indexes resolved");

  const jsapi::RpcResponse add_res =
      test::Execute(&f.rpc, demo_handle, add,
                    {jsapi::JsValue::Int(2), jsapi::JsValue::Int(3)});
  test::Check(add_res.ok && add_res.value.AsInt() == 5,
              "plugin add(2,3) == 5", add_res.error);

  const jsapi::RpcResponse async_res = test::Execute(
      &f.rpc, demo_handle, sum_async,
      {jsapi::JsValue::Int(20), jsapi::JsValue::Int(22)}, /*async=*/true);
  test::Check(async_res.ok && async_res.value.AsInt() == 42,
              "async declared sumAsync returns 42", async_res.error);

  const jsapi::RpcResponse fail_res =
      test::Execute(&f.rpc, demo_handle, fail, {});
  test::Check(!fail_res.ok &&
                  fail_res.error.find("intentional") != std::string::npos,
              "plugin failure becomes RPC error", fail_res.error);

  jsapi::RpcRequest version_get;
  version_get.seq = 6;
  version_get.op = "get";
  version_get.handle = demo_handle;
  version_get.index = version;
  const jsapi::RpcResponse version_res = test::Roundtrip(&f.rpc, version_get);
  test::Check(version_res.ok && version_res.value.AsString() == "demo-1.2.0",
              "plugin read-only property version", version_res.error);

  jsapi::RpcRequest count_get;
  count_get.seq = 7;
  count_get.op = "get";
  count_get.handle = demo_handle;
  count_get.index = count;
  const jsapi::RpcResponse count_res = test::Roundtrip(&f.rpc, count_get);
  test::Check(count_res.ok && count_res.value.AsInt() == 10,
              "plugin default count == 10", count_res.error);

  const jsapi::RpcResponse child_res =
      test::Execute(&f.rpc, demo_handle, child, {});
  test::Check(child_res.ok && child_res.value.AsHandle() > demo_handle,
              "plugin child() allocates a new handle", child_res.error);
  if (child_res.ok) {
    const int child_handle = child_res.value.AsHandle();
    const jsapi::RpcResponse child_desc = test::Describe(&f.rpc, child_handle);
    const int ping = test::FindMemberIndex(child_desc.value, "ping");
    const jsapi::RpcResponse ping_res =
        test::Execute(&f.rpc, child_handle, ping,
                      {jsapi::JsValue::String("hi")});
    test::Check(ping_res.ok && ping_res.value.AsString() == "pong: hi",
                "child node ping works through its own table", ping_res.error);
  }

  // Qt 实现的新接口：注册表读写 + 异步读文件。
  const int set_reg = test::FindMemberIndex(desc.value, "setRegValue");
  const int get_reg = test::FindMemberIndex(desc.value, "getRegValue");
  const int read_file = test::FindMemberIndex(desc.value, "readFile");
  test::Check(set_reg >= 0 && get_reg >= 0 && read_file >= 0,
              "plugin declares setRegValue/getRegValue/readFile");

  const jsapi::RpcResponse set_res = test::Execute(
      &f.rpc, demo_handle, set_reg,
      {jsapi::JsValue::String("self_test_value"),
       jsapi::JsValue::String("hello-reg")});
  test::Check(set_res.ok && set_res.value.AsBool(),
              "setRegValue writes registry", set_res.error);

  const jsapi::RpcResponse get_res = test::Execute(
      &f.rpc, demo_handle, get_reg,
      {jsapi::JsValue::String("self_test_value"),
       jsapi::JsValue::String("fallback")});
  test::Check(get_res.ok && get_res.value.AsString() == "hello-reg",
              "getRegValue reads back value", get_res.error);

  const jsapi::RpcResponse default_res = test::Execute(
      &f.rpc, demo_handle, get_reg,
      {jsapi::JsValue::String("self_test_missing_key"),
       jsapi::JsValue::String("fallback")});
  test::Check(default_res.ok && default_res.value.AsString() == "fallback",
              "getRegValue returns default for missing key", default_res.error);

  const std::string file_content = "jsapi async file content";
  const std::string temp_file = test::WriteTempFile(file_content);
  const jsapi::RpcResponse read_res = test::Execute(
      &f.rpc, demo_handle, read_file,
      {jsapi::JsValue::String(temp_file)}, /*async=*/true);
  test::Check(read_res.ok && read_res.value.AsString() == file_content,
              "readFile returns file content via async response",
              read_res.error);

  // 事件回环：注册监听 -> 节点主动 emit -> sink 收到 -> 移除。
  const int add_event = test::FindMemberIndex(desc.value, "addEventListener");
  const int remove_event = test::FindMemberIndex(desc.value, "removeEventListener");
  const int emit = test::FindMemberIndex(desc.value, "emit");
  test::Check(add_event >= 0 && remove_event >= 0 && emit >= 0,
              "plugin exposes base event members");

  f.sink.events.clear();
  const jsapi::RpcResponse subscribe =
      test::Execute(&f.rpc, demo_handle, add_event,
                    {jsapi::JsValue::String("plugin-evt"),
                     jsapi::JsValue::CallbackHandle(700)});
  test::Check(subscribe.ok && subscribe.value.AsBool(),
              "plugin event listener registered", subscribe.error);

  const jsapi::RpcResponse emitted = test::Execute(
      &f.rpc, demo_handle, emit,
      {jsapi::JsValue::String("plugin-evt"),
       jsapi::JsValue::Array({jsapi::JsValue::String("payload"),
                              jsapi::JsValue::Int(2)})});
  test::Check(emitted.ok && emitted.value.AsInt() == 1,
              "plugin emit reaches one listener", emitted.error);
  test::Check(f.sink.events.size() == 1 &&
                  f.sink.events[0].callback == 700 &&
                  f.sink.events[0].args.size() == 2 &&
                  f.sink.events[0].args[0].AsString() == "payload",
              "plugin event loopback preserves callback and args");

  const jsapi::RpcResponse unsubscribe =
      test::Execute(&f.rpc, demo_handle, remove_event,
                    {jsapi::JsValue::String("plugin-evt"),
                     jsapi::JsValue::CallbackHandle(700)});
  test::Check(unsubscribe.ok && unsubscribe.value.AsBool(),
              "plugin event listener removed", unsubscribe.error);
  f.sink.events.clear();
  test::Check(test::Execute(&f.rpc, demo_handle, emit,
                            {jsapi::JsValue::String("plugin-evt")})
                      .value.AsInt() == 0 &&
                  f.sink.events.empty(),
              "plugin event stops after unsubscribe");

  // jsapi 不提供 FreeLibrary：DLL 常驻进程。可观察的"卸载"语义是注册表销毁后
  // 旧句柄失效，这里用一个局部注册表固定该行为。
  std::string scoped_error;
  int scoped_handle = -1;
  {
    jsapi::JsRegistry scoped;
    scoped.SetEventSink(&f.sink);
    jsapi::JsRegistry::Factory scoped_factory = LoadPlugin(&scoped_error);
    test::Check(scoped_factory != nullptr,
                "plugin reloads for unload semantics", scoped_error);
    if (scoped_factory) {
      scoped.RegisterNode("demo", std::move(scoped_factory));
      jsapi::RpcHost scoped_rpc(&scoped);
      scoped_handle = test::GetNodeHandle(&scoped_rpc, "demo");
      test::Check(scoped_handle > 0, "scoped plugin node handle");
    }
  }
  jsapi::JsRegistry after_unload;
  jsapi::RpcHost after_rpc(&after_unload);
  test::Check(!test::Execute(&after_rpc, scoped_handle, 0, {}).ok,
              "plugin node handle is invalid after its registry is destroyed");
}

TEST(system, plugin_second_load) {
  std::string error;
  jsapi::JsRegistry registry;
  test::TestEventSink sink;
  registry.SetEventSink(&sink);

  jsapi::JsRegistry::Factory first = LoadPlugin(&error);
  jsapi::JsRegistry::Factory second = LoadPlugin(&error);
  test::Check(first != nullptr && second != nullptr,
              "same plugin dll loads twice in one process", error);
  if (!first || !second) {
    return;
  }

  registry.RegisterNode("demo_a", std::move(first));
  registry.RegisterNode("demo_b", std::move(second));
  jsapi::RpcHost rpc(&registry);
  const int handle_a = test::GetNodeHandle(&rpc, "demo_a");
  const int handle_b = test::GetNodeHandle(&rpc, "demo_b");
  test::Check(handle_a > 0 && handle_b > 0 && handle_a != handle_b,
              "two plugin loads expose distinct node handles",
              std::to_string(handle_a) + "/" + std::to_string(handle_b));

  const jsapi::RpcResponse desc_a = test::Describe(&rpc, handle_a);
  const int count_a = test::FindMemberIndex(desc_a.value, "count");
  jsapi::RpcRequest set_a;
  set_a.seq = 20;
  set_a.op = "set";
  set_a.handle = handle_a;
  set_a.index = count_a;
  set_a.args = {jsapi::JsValue::Int(88)};
  test::Check(test::Roundtrip(&rpc, set_a).ok,
              "one plugin instance accepts state");

  const jsapi::RpcResponse desc_b = test::Describe(&rpc, handle_b);
  const int count_b = test::FindMemberIndex(desc_b.value, "count");
  jsapi::RpcRequest get_b;
  get_b.seq = 21;
  get_b.op = "get";
  get_b.handle = handle_b;
  get_b.index = count_b;
  const jsapi::RpcResponse get_b_res = test::Roundtrip(&rpc, get_b);
  test::Check(get_b_res.ok && get_b_res.value.AsInt() == 10,
              "plugin instances do not share node state", get_b_res.error);
}

TEST(system, plugin_name_collision) {
  std::string error;
  jsapi::JsRegistry registry;
  test::TestEventSink sink;
  registry.SetEventSink(&sink);

  // 静态 host 节点先注册并实例化，随后同名注册插件工厂。
  registry.RegisterNode("demo", &jsapi_demo::CreateHostNode);
  jsapi::RpcHost rpc(&registry);
  const int first_handle = test::GetNodeHandle(&rpc, "demo");
  test::Check(first_handle > 0, "collision: static node handle");

  jsapi::JsRegistry::Factory factory = LoadPlugin(&error);
  test::Check(factory != nullptr, "collision: plugin loads", error);
  if (!factory) {
    return;
  }
  registry.RegisterNode("demo", std::move(factory));

  const int second_handle = test::GetNodeHandle(&rpc, "demo");
  test::Check(second_handle == first_handle,
              "collision: the already-instantiated static node wins");
  const jsapi::RpcResponse desc = test::Describe(&rpc, second_handle);
  test::Check(test::FindMemberIndex(desc.value, "echo") >= 0,
              "collision: static node members remain visible");
  test::Check(test::FindMemberIndex(desc.value, "add") < 0,
              "collision: plugin members are not merged into the static node");
}

TEST(system, plugin_load_failures) {
  std::string error;
  const jsapi::JsRegistry::Factory missing = jsapi::plugins::LoadDll(
      test::GetExecutableDir() + "\\no_such_plugin_12345.dll", &error);
  test::Check(missing == nullptr && !error.empty(),
              "missing plugin dll fails to load", error);

  error.clear();
  const jsapi::JsRegistry::Factory wrong_entry =
      jsapi::plugins::LoadDll("kernel32.dll", &error);
  test::Check(wrong_entry == nullptr &&
                  error.find("GetExtensionJsApiObj") != std::string::npos,
              "dll without the plugin entry point is rejected", error);

  test::Check(JSAPI_ABI_VERSION == 1,
              "plugin ABI contract version is 1; LoadDll performs no runtime "
              "ABI negotiation");
}
