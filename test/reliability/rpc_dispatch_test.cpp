#include <climits>
#include <cstdint>
#include <limits>
#include <memory>
#include <string>
#include <vector>

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

TEST(reliability, root_and_nodes) {
  test::CoreFixture& f = Fixture();
  const jsapi::RpcResponse root = test::Describe(&f.rpc, 0);
  test::Check(root.ok, "root describe ok", root.error);
  test::Check(test::FindMemberIndex(root.value, "node") < 0,
              "root no longer has node()");
  test::Check(test::FindMemberIndex(root.value, "nodes") >= 0,
              "root has nodes()");
  test::Check(test::FindMemberIndex(root.value, "addEventListener") >= 0,
              "root has base event members");

  const jsapi::RpcResponse names = test::Execute(
      &f.rpc, 0, test::FindMemberIndex(root.value, "nodes"), {});
  bool has_host = false;
  for (const auto& item : names.value.AsArray()) {
    if (item.AsString() == "host") has_host = true;
  }
  test::Check(names.ok && has_host, "root.nodes contains host", names.error);
}

TEST(reliability, function_table) {
  test::CoreFixture& f = Fixture();
  const int host_handle = f.host_handle;
  test::Check(host_handle >= 0, "Extention.host returns handle");
  if (host_handle < 0) {
    return;
  }

  // 根对象属性形式：Extention.host 取到的句柄与直接调用一致。
  const jsapi::RpcResponse root_desc = test::Describe(&f.rpc, 0);
  const int host_property = test::FindMemberIndex(root_desc.value, "host");
  test::Check(host_property >= 0, "root exposes host property");
  if (host_property >= 0) {
    const jsapi::RpcResponse prop = test::GetProperty(&f.rpc, 0, host_property);
    test::Check(prop.ok &&
                    prop.value.type() == jsapi::JsType::ObjectHandle &&
                    prop.value.AsHandle(-1) == host_handle,
                "Extention.host property resolves to handle", prop.error);
  }

  // 重复注册同名节点：已实例化的句柄保持不变，页面上已有代理继续有效。
  f.registry.RegisterNode("host", &jsapi_demo::CreateHostNode);
  test::Check(test::GetNodeHandle(&f.rpc, "host") == host_handle,
              "re-register keeps existing handle");

  // 直接注册已构造好的节点实例，首次访问时才分配句柄。
  f.registry.RegisterNode("host_inst", jsapi_demo::CreateHostNode());
  const int inst_handle = test::GetNodeHandle(&f.rpc, "host_inst");
  test::Check(inst_handle >= 0, "instance registration returns handle");
  if (inst_handle >= 0) {
    const jsapi::RpcResponse inst_desc = test::Describe(&f.rpc, inst_handle);
    const int inst_echo = test::FindMemberIndex(inst_desc.value, "echo");
    const jsapi::RpcResponse inst_res = test::Execute(
        &f.rpc, inst_handle, inst_echo, {jsapi::JsValue::String("hi")});
    test::Check(inst_res.ok && inst_res.value.AsString() == "hi",
                "instance node is callable", inst_res.error);
  }

  const jsapi::RpcResponse desc = test::Describe(&f.rpc, host_handle);
  test::Check(desc.ok, "host describe ok", desc.error);

  const int echo = test::FindMemberIndex(desc.value, "echo");
  const int mul = test::FindMemberIndex(desc.value, "mul");
  test::Check(echo >= 0 && mul >= 0, "host function table has echo/mul");

  const jsapi::RpcResponse echo_res =
      test::Execute(&f.rpc, host_handle, echo, {jsapi::JsValue::String("hello")});
  test::Check(echo_res.ok && echo_res.value.AsString() == "hello",
              "host echo() via handle+index", echo_res.error);

  const jsapi::RpcResponse mul_res = test::Execute(
      &f.rpc, host_handle, mul,
      {jsapi::JsValue::Int(6), jsapi::JsValue::Int(7)});
  test::Check(mul_res.ok && mul_res.value.AsInt() == 42, "host mul(6,7) == 42",
              mul_res.error);

  const jsapi::RpcResponse bad_type =
      test::Execute(&f.rpc, host_handle, echo, {jsapi::JsValue::Int(1)});
  test::Check(!bad_type.ok &&
                  bad_type.error.find("expected string") != std::string::npos,
              "argument type check rejects int for string", bad_type.error);
}

TEST(reliability, property_access) {
  test::CoreFixture& f = Fixture();
  const jsapi::RpcResponse desc = test::Describe(&f.rpc, f.host_handle);
  const int counter = test::FindMemberIndex(desc.value, "counter");
  test::Check(counter >= 0, "host has writable property counter");

  jsapi::RpcRequest set_request;
  set_request.seq = 3;
  set_request.op = "set";
  set_request.handle = f.host_handle;
  set_request.index = counter;
  set_request.args = {jsapi::JsValue::Int(123)};
  const jsapi::RpcResponse set_res = test::Roundtrip(&f.rpc, set_request);
  test::Check(set_res.ok, "property counter set", set_res.error);

  jsapi::RpcRequest get_request;
  get_request.seq = 4;
  get_request.op = "get";
  get_request.handle = f.host_handle;
  get_request.index = counter;
  const jsapi::RpcResponse get_res = test::Roundtrip(&f.rpc, get_request);
  test::Check(get_res.ok && get_res.value.AsInt() == 123,
              "property counter get reflects set", get_res.error);

  const int app = test::FindMemberIndex(desc.value, "app");
  jsapi::RpcRequest readonly_set;
  readonly_set.seq = 5;
  readonly_set.op = "set";
  readonly_set.handle = f.host_handle;
  readonly_set.index = app;
  readonly_set.args = {jsapi::JsValue::String("x")};
  const jsapi::RpcResponse readonly_res = test::Roundtrip(&f.rpc, readonly_set);
  test::Check(!readonly_res.ok &&
                  readonly_res.error.find("read-only") != std::string::npos,
              "read-only property rejects set", readonly_res.error);
}

TEST(reliability, rpc_target_validation) {
  test::CoreFixture& f = Fixture();

  const jsapi::RpcResponse missing = test::Execute(&f.rpc, 999999, 0, {});
  test::Check(!missing.ok &&
                  missing.error.find("invalid object handle") !=
                      std::string::npos,
              "rpc: unknown target handle is rejected", missing.error);

  const jsapi::RpcResponse negative = test::Execute(&f.rpc, -1, 0, {});
  test::Check(!negative.ok &&
                  negative.error.find("invalid object handle") !=
                      std::string::npos,
              "rpc: negative target handle is rejected", negative.error);

  jsapi::RpcRequest from_json;
  std::string error;
  const bool parsed = jsapi::RpcHost::ParseRequest(
      R"({"seq":1,"op":"describe","handle":{"$obj":-1},"index":0,"args":[]})",
      &from_json, &error);
  test::Check(parsed, "rpc: $obj:-1 request parses", error);
  const jsapi::RpcResponse obj_negative = f.rpc.Process(from_json);
  test::Check(!obj_negative.ok &&
                  obj_negative.error.find("invalid object handle") !=
                      std::string::npos,
              "rpc: $obj handle -1 is rejected at dispatch",
              obj_negative.error);

  // 已释放句柄：临时节点代理全部 release 后必须拒绝调用。
  f.registry.RegisterNode("tmp_dispatch", [] {
    return std::make_shared<test::TransientTestNode>();
  });
  const int transient_handle = test::GetNodeHandle(&f.rpc, "tmp_dispatch");
  test::Check(transient_handle > 0, "rpc: transient target handle allocated");
  test::Check(f.registry.ReleaseObject(transient_handle),
              "rpc: transient target released");
  const jsapi::RpcResponse released =
      test::Execute(&f.rpc, transient_handle, 0, {});
  test::Check(!released.ok &&
                  released.error.find("invalid object handle") !=
                      std::string::npos,
              "rpc: released target handle is rejected", released.error);

  const jsapi::RpcResponse neg_index =
      test::Execute(&f.rpc, f.host_handle, -1, {});
  test::Check(!neg_index.ok &&
                  neg_index.error.find("invalid member index") !=
                      std::string::npos,
              "rpc: negative member index is rejected", neg_index.error);

  const jsapi::RpcResponse huge_index =
      test::Execute(&f.rpc, f.host_handle, INT_MAX, {});
  test::Check(!huge_index.ok &&
                  huge_index.error.find("invalid member index") !=
                      std::string::npos,
              "rpc: INT_MAX member index is rejected", huge_index.error);
}

TEST(reliability, rpc_dispatch_errors) {
  test::CoreFixture& f = Fixture();
  const jsapi::RpcResponse desc = test::Describe(&f.rpc, f.host_handle);
  const int echo = test::FindMemberIndex(desc.value, "echo");
  const int app = test::FindMemberIndex(desc.value, "app");
  const int counter = test::FindMemberIndex(desc.value, "counter");

  // 对属性直接 execute：必须报"不是函数"。
  const jsapi::RpcResponse prop_call =
      test::Execute(&f.rpc, f.host_handle, app, {});
  test::Check(!prop_call.ok &&
                  prop_call.error.find("is not a function") != std::string::npos,
              "rpc: calling a property reports not a function", prop_call.error);

  // 对函数直接 get：必须报"不是属性"。
  const jsapi::RpcResponse fn_get = test::GetProperty(&f.rpc, f.host_handle, echo);
  test::Check(!fn_get.ok &&
                  fn_get.error.find("is not a property") != std::string::npos,
              "rpc: reading a function reports not a property", fn_get.error);

  // set 缺参数 / 参数类型错误。
  jsapi::RpcRequest no_value;
  no_value.seq = 6;
  no_value.op = "set";
  no_value.handle = f.host_handle;
  no_value.index = counter;
  const jsapi::RpcResponse no_value_res = test::Roundtrip(&f.rpc, no_value);
  test::Check(!no_value_res.ok &&
                  no_value_res.error.find("set requires") != std::string::npos,
              "rpc: set without a value is rejected", no_value_res.error);

  jsapi::RpcRequest wrong_type;
  wrong_type.seq = 7;
  wrong_type.op = "set";
  wrong_type.handle = f.host_handle;
  wrong_type.index = counter;
  wrong_type.args = {jsapi::JsValue::String("x")};
  const jsapi::RpcResponse wrong_type_res = test::Roundtrip(&f.rpc, wrong_type);
  test::Check(!wrong_type_res.ok &&
                  wrong_type_res.error.find("expected int") != std::string::npos,
              "rpc: set with wrong type is rejected", wrong_type_res.error);

  // 未知 op。
  jsapi::RpcRequest bogus;
  bogus.seq = 8;
  bogus.op = "bogus";
  bogus.handle = f.host_handle;
  bogus.index = 0;
  const jsapi::RpcResponse bogus_res = test::Roundtrip(&f.rpc, bogus);
  test::Check(!bogus_res.ok &&
                  bogus_res.error.find("unknown op") != std::string::npos,
              "rpc: unknown op is rejected", bogus_res.error);

  // 未 describe 直接调用：函数表按需构建，必须可用（index 0 是 echo）。
  jsapi::JsRegistry local;
  local.RegisterNode("host", &jsapi_demo::CreateHostNode);
  jsapi::RpcHost local_rpc(&local);
  const std::shared_ptr<jsapi::JsObjectBase> node =
      local.GetNode("host", test::MakeEnv());
  test::Check(node != nullptr, "rpc: fresh node can be created");
  if (node) {
    const jsapi::RpcResponse first_call = test::Execute(
        &local_rpc, node->handle(), 0, {jsapi::JsValue::String("fresh")});
    test::Check(first_call.ok && first_call.value.AsString() == "fresh",
                "rpc: call before describe builds the table lazily",
                first_call.error);
  }

  // 节点返回非 JSON 值（NaN / Infinity / INT64 极值）不得破坏响应。
  f.registry.RegisterNode("raw_values", std::make_shared<test::RawValueTestNode>());
  const int raw_handle = test::GetNodeHandle(&f.rpc, "raw_values");
  test::Check(raw_handle > 0, "rpc: raw value node handle");
  if (raw_handle > 0) {
    const jsapi::RpcResponse raw_desc = test::Describe(&f.rpc, raw_handle);
    const int nan_index = test::FindMemberIndex(raw_desc.value, "nan");
    jsapi::RpcRequest nan_request;
    nan_request.seq = 30;
    nan_request.op = "execute";
    nan_request.handle = raw_handle;
    nan_request.index = nan_index;
    const jsapi::RpcResponse nan_raw = f.rpc.Process(nan_request);
    jsapi::JsValue nan_decoded;
    const bool nan_encoded = jsapi::ParseJson(
        jsapi::RpcHost::EncodeResponse(nan_raw), &nan_decoded, nullptr);
    test::Check(nan_raw.ok && nan_encoded &&
                    nan_decoded.Find("value") != nullptr &&
                    nan_decoded.Find("value")->type() == jsapi::JsType::Int &&
                    nan_decoded.Find("value")->AsInt() == 0,
                "rpc: NaN result serializes as 0 without breaking the response",
                jsapi::RpcHost::EncodeResponse(nan_raw));

    const int inf_index = test::FindMemberIndex(raw_desc.value, "infinite");
    jsapi::RpcRequest inf_request;
    inf_request.seq = 31;
    inf_request.op = "execute";
    inf_request.handle = raw_handle;
    inf_request.index = inf_index;
    const jsapi::RpcResponse inf_raw = f.rpc.Process(inf_request);
    jsapi::JsValue inf_decoded;
    const bool inf_encoded = jsapi::ParseJson(
        jsapi::RpcHost::EncodeResponse(inf_raw), &inf_decoded, nullptr);
    test::Check(inf_raw.ok && inf_encoded &&
                    inf_decoded.Find("value") != nullptr &&
                    inf_decoded.Find("value")->type() == jsapi::JsType::Int &&
                    inf_decoded.Find("value")->AsInt() == 0,
                "rpc: Infinity result serializes as 0",
                jsapi::RpcHost::EncodeResponse(inf_raw));

    const auto min_res = test::Execute(
        &f.rpc, raw_handle, test::FindMemberIndex(raw_desc.value, "intMin"), {});
    test::Check(min_res.ok &&
                    min_res.value.AsInt() ==
                        std::numeric_limits<std::int64_t>::min(),
                "rpc: INT64_MIN result roundtrips through the response",
                min_res.error);
  }
}
