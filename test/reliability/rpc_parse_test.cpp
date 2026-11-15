#include <climits>
#include <map>
#include <string>

#include "jsapi_channel/jsapi_channel.h"
#include "jsapi_rpc/jsapi_rpc.h"
#include "support/test_harness.h"

namespace {

// 构造一个字段齐全的请求，供两种解析入口对比。
jsapi::RpcRequest MakeFullRequest() {
  jsapi::RpcRequest request;
  request.seq = 42;
  request.op = "execute";
  request.handle = 3;
  request.index = 2;
  request.async = true;
  request.args.push_back(jsapi::JsValue::Int(5));
  request.env.browser_id = 7;
  request.env.frame_id = "frame-x";
  return request;
}

}  // namespace

TEST(reliability, rpc_parse) {
  const jsapi::RpcRequest request = MakeFullRequest();

  std::map<std::string, jsapi::JsValue> extras;
  extras["reply_map"] = jsapi::JsValue::String("map-a");
  extras["reply_event"] = jsapi::JsValue::String("event-a");
  const std::string json = jsapi::RpcHost::EncodeRequest(request, extras);

  jsapi::JsValue root;
  std::string error;
  const bool tree_ok = jsapi::ParseJson(json, &root, &error);
  test::Check(tree_ok, "rpc: extras json parses once", error);

  jsapi::RpcRequest from_tree;
  jsapi::RpcRequest from_text;
  const bool tree_parse = jsapi::RpcHost::ParseRequest(root, &from_tree, &error);
  const bool text_parse = jsapi::RpcHost::ParseRequest(json, &from_text, &error);
  test::Check(tree_parse && text_parse, "rpc: both parse entrances succeed",
              error);
  test::Check(from_tree.seq == 42 && from_tree.op == "execute" &&
                  from_tree.handle == 3 && from_tree.index == 2 &&
                  from_tree.async && from_tree.args.size() == 1 &&
                  from_tree.args[0].AsInt() == 5 &&
                  from_tree.env.browser_id == 7 &&
                  from_tree.env.frame_id == "frame-x",
              "rpc: parsed tree keeps all request fields");
  test::Check(from_tree.seq == from_text.seq &&
                  from_tree.index == from_text.index &&
                  from_tree.args.size() == from_text.args.size(),
              "rpc: tree and text entrances agree");
  test::Check(root.Find("reply_map") != nullptr &&
                  root.Find("reply_map")->AsString() == "map-a",
              "rpc: extras stay readable from the same tree");

  jsapi::RpcRequest bad;
  test::Check(!jsapi::RpcHost::ParseRequest(jsapi::JsValue::Int(1), &bad, &error),
              "rpc: non-object root is rejected");
}

TEST(reliability, rpc_parse_edge_cases) {
  jsapi::RpcRequest request;
  std::string error;

  // 空请求 / 纯空白 / 缺少 op。
  error.clear();
  test::Check(!jsapi::RpcHost::ParseRequest(std::string(), &request, &error) &&
                  !error.empty(),
              "rpc parse: empty request is rejected", error);
  error.clear();
  test::Check(
      !jsapi::RpcHost::ParseRequest(std::string("  \n\t "), &request, &error) &&
          !error.empty(),
      "rpc parse: whitespace-only request is rejected", error);
  error.clear();
  test::Check(!jsapi::RpcHost::ParseRequest(R"({"seq":1,"handle":0})",
                                            &request, &error) &&
                  error.find("missing op") != std::string::npos,
              "rpc parse: request without op is rejected", error);
  error.clear();
  test::Check(!jsapi::RpcHost::ParseRequest(R"({"op":5,"handle":0})", &request,
                                            &error) &&
                  error.find("missing op") != std::string::npos,
              "rpc parse: non-string op is rejected", error);
  error.clear();
  test::Check(!jsapi::RpcHost::ParseRequest(R"({"op":"describe"}{})", &request,
                                            &error) &&
                  !error.empty(),
              "rpc parse: trailing garbage is rejected", error);

  // args 不是数组：当前实现按空数组容错（记录实际语义，不视为崩溃）。
  error.clear();
  const bool args_ok = jsapi::RpcHost::ParseRequest(
      R"({"op":"describe","handle":0,"args":5})", &request, &error);
  test::Check(args_ok && request.args.empty(),
              "rpc parse: non-array args is tolerated as empty", error);

  // index 越界值原样保留，由分发层拒绝。
  error.clear();
  const bool negative_ok = jsapi::RpcHost::ParseRequest(
      R"({"op":"get","handle":0,"index":-1})", &request, &error);
  test::Check(negative_ok && request.index == -1,
              "rpc parse: negative index survives parsing", error);
  error.clear();
  const bool max_ok = jsapi::RpcHost::ParseRequest(
      R"({"op":"get","handle":0,"index":2147483647})", &request, &error);
  test::Check(max_ok && request.index == INT_MAX,
              "rpc parse: INT_MAX index survives parsing", error);

  // 非法句柄对象：{"$obj":-1} / {"$cb":-1}。
  error.clear();
  const bool obj_ok = jsapi::RpcHost::ParseRequest(
      R"({"op":"execute","handle":{"$obj":-1},"index":0,"args":[]})",
      &request, &error);
  test::Check(obj_ok && request.handle == -1,
              "rpc parse: $obj:-1 survives parsing", error);

  error.clear();
  const bool cb_ok = jsapi::RpcHost::ParseRequest(
      R"({"op":"execute","handle":0,"index":0,"args":[{"$cb":-1}]})",
      &request, &error);
  test::Check(cb_ok && request.args.size() == 1 &&
                  request.args[0].type() == jsapi::JsType::CallbackHandle &&
                  request.args[0].AsHandle() == -1,
              "rpc parse: $cb:-1 survives parsing", error);

  // 缺少 handle / index 时沿用结构体默认值（handle=0，index=-1）。
  error.clear();
  const bool defaults_ok =
      jsapi::RpcHost::ParseRequest(R"({"op":"describe"})", &request, &error);
  test::Check(defaults_ok && request.handle == 0 && request.index == -1,
              "rpc parse: missing handle/index keep protocol defaults", error);
}
