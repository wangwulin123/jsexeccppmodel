#include <memory>
#include <string>

#include "host_nodes.h"
#include "jsapi/js_registry.h"
#include "jsapi_rpc/jsapi_rpc.h"
#include "support/test_fixtures.h"
#include "support/test_harness.h"

namespace {

test::CoreFixture& Fixture() {
  static test::CoreFixture fixture;
  return fixture;
}

}  // namespace

TEST(reliability, lifecycle) {
  test::CoreFixture& f = Fixture();
  f.registry.RegisterNode("tmp", [] {
    return std::make_shared<test::TransientTestNode>();
  });
  const int tmp_first = test::GetNodeHandle(&f.rpc, "tmp");
  const int tmp_second = test::GetNodeHandle(&f.rpc, "tmp");
  test::Check(tmp_first > 0 && tmp_second > tmp_first,
              "transient node creates a new instance per access");

  // describe 相当于渲染进程建立一层代理，浏览器侧引用计数 +1。
  // 同一句柄被 describe 三次 → 三层代理，必须三次 release 才真正销毁。
  test::Check(test::Describe(&f.rpc, tmp_first).ok, "transient describe ok");
  const jsapi::JsRegistry::ObjectStats stats_one =
      f.registry.GetObjectStats();
  test::Check(test::Describe(&f.rpc, tmp_first).ok,
              "transient describe second proxy");
  test::Check(test::Describe(&f.rpc, tmp_first).ok,
              "transient describe third proxy");
  const jsapi::JsRegistry::ObjectStats stats_three =
      f.registry.GetObjectStats();
  test::Check(stats_three.release_refs == stats_one.release_refs + 2,
              "describe increments release refs per proxy");

  test::Check(f.registry.ReleaseObject(tmp_first), "release proxy #1");
  test::Check(f.registry.GetObjectByHandle(tmp_first) != nullptr,
              "object survives while proxies remain");
  test::Check(f.registry.ReleaseObject(tmp_first), "release proxy #2");
  test::Check(f.registry.GetObjectByHandle(tmp_first) != nullptr,
              "object survives until last proxy");
  test::Check(f.registry.ReleaseObject(tmp_first),
              "release last proxy destroys object");
  test::Check(f.registry.GetObjectByHandle(tmp_first) == nullptr,
              "released handle is no longer resolvable");
  test::Check(!test::Describe(&f.rpc, tmp_first).ok,
              "describe released handle fails");
  test::Check(!f.registry.ReleaseObject(tmp_first),
              "double release is ignored");
  test::Check(!f.registry.ReleaseObject(-1),
              "release invalid handle is ignored");
  test::Check(f.registry.GetObjectStats().transient_live <
                  stats_three.transient_live,
              "live transient count drops after release");

  // 单例节点不受 release 影响。
  test::Check(!f.registry.ReleaseObject(f.host_handle),
              "singleton is not releasable");
  test::Check(f.registry.GetObjectByHandle(f.host_handle) != nullptr,
              "singleton survives release");
}

TEST(reliability, registry_invalidation) {
  // 清空/销毁注册表后，旧句柄在新注册表中必须失效。
  int old_handle = -1;
  {
    jsapi::JsRegistry local;
    local.RegisterNode("scratch", &jsapi_demo::CreateHostNode);
    jsapi::RpcHost local_rpc(&local);
    old_handle = test::GetNodeHandle(&local_rpc, "scratch");
    test::Check(old_handle > 0, "registry: scratch node handle allocated");
  }

  jsapi::JsRegistry fresh;
  test::CoreFixture& f = Fixture();
  fresh.SetEventSink(&f.sink);
  jsapi::RpcHost fresh_rpc(&fresh);
  const jsapi::RpcResponse stale = test::Execute(&fresh_rpc, old_handle, 0, {});
  test::Check(!stale.ok &&
                  stale.error.find("invalid object handle") !=
                      std::string::npos,
              "registry: handle from a destroyed registry is rejected",
              stale.error);

  // 插件节点与静态节点同属注册表，销毁注册表即等于"卸载"其全部节点。
  // jsapi 不提供 FreeLibrary，DLL 常驻；这里固定住可见语义。
  test::Check(test::FindMemberIndex(test::Describe(&fresh_rpc, 0).value,
                                    "scratch") < 0,
              "registry: destroyed registry no longer exposes its nodes");
}

TEST(reliability, registry_duplicate_name) {
  jsapi::JsRegistry local;
  local.RegisterNode("dup", &jsapi_demo::CreateHostNode);
  jsapi::RpcHost local_rpc(&local);

  const int first = test::GetNodeHandle(&local_rpc, "dup");
  test::Check(first > 0, "registry: duplicate test first handle");

  // 同名重复注册：保留已实例化对象，页面上已有代理继续有效。
  local.RegisterNode("dup", [] {
    return std::make_shared<test::TransientTestNode>();
  });
  const int second = test::GetNodeHandle(&local_rpc, "dup");
  test::Check(first == second,
              "registry: duplicate registration keeps the live singleton");

  const jsapi::RpcResponse desc = test::Describe(&local_rpc, first);
  test::Check(test::FindMemberIndex(desc.value, "echo") >= 0,
              "registry: duplicate registration keeps the original members");
  test::Check(test::FindMemberIndex(desc.value, "ping") < 0,
              "registry: duplicate registration does not swap the node type");
}
