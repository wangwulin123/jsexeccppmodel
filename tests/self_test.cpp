#include <windows.h>

#include <cstdio>
#include <fstream>
#include <limits>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "host_nodes.h"
#include "jsapi_rpc/jsapi_rpc.h"

namespace {

int g_failures = 0;
int g_checks = 0;

void Check(bool ok, const std::string& name, const std::string& detail = "") {
  ++g_checks;
  if (ok) {
    std::printf("[PASS] %s\n", name.c_str());
  } else {
    ++g_failures;
    std::printf("[FAIL] %s%s%s\n", name.c_str(), detail.empty() ? "" : " -> ",
                detail.c_str());
  }
}

std::wstring GetExecutableDir() {
  wchar_t path[MAX_PATH] = {};
  ::GetModuleFileNameW(::GetModuleHandleW(nullptr), path, MAX_PATH);
  std::wstring dir(path);
  const std::size_t pos = dir.find_last_of(L"\\/");
  if (pos != std::wstring::npos) {
    dir.resize(pos);
  }
  return dir;
}

std::string WideToUtf8(const std::wstring& text) {
  if (text.empty()) return {};
  const int size = ::WideCharToMultiByte(CP_UTF8, 0, text.data(),
                                         static_cast<int>(text.size()), nullptr,
                                         0, nullptr, nullptr);
  std::string out(static_cast<std::size_t>(size), '\0');
  ::WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()),
                        out.data(), size, nullptr, nullptr);
  return out;
}

// 用类内宏声明生命周期，验证临时节点行为。
class TransientTestNode final : public jsapi::JsObjectBase {
 public:
  JSAPI_NODE_LIFE_TYPE(jsapi::LifeType::Transient);
};

// 写一个临时文件，供异步 readFile 用例读取。
std::string WriteTempFile(const std::string& content) {
  char dir[MAX_PATH] = {};
  ::GetTempPathA(MAX_PATH, dir);
  const std::string path = std::string(dir) + "jsapi_readfile_test.txt";
  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  out << content;
  out.close();
  return path;
}

struct EventRecord {
  std::string event;
  int callback = -1;
  jsapi::RpcEnv env;
  std::vector<jsapi::JsValue> args;
};

class TestEventSink final : public jsapi::JsEventSink {
 public:
  void OnEvent(const std::string& event,
               int callback_id,
               const jsapi::RpcEnv& target,
               const std::vector<jsapi::JsValue>& args) override {
    events.push_back(EventRecord{event, callback_id, target, args});
  }

  std::vector<EventRecord> events;
};

int FindMemberIndex(const jsapi::JsValue& describe, const std::string& name) {
  if (describe.type() != jsapi::JsType::Object) return -1;
  const jsapi::JsValue* members = describe.Find("members");
  if (!members) return -1;
  for (const auto& member : members->AsArray()) {
    const jsapi::JsValue* member_name = member.Find("name");
    if (member_name && member_name->AsString() == name) {
      const jsapi::JsValue* index = member.Find("index");
      return index ? static_cast<int>(index->AsInt(-1)) : -1;
    }
  }
  return -1;
}

jsapi::RpcEnv MakeEnv() {
  jsapi::RpcEnv env;
  env.browser_id = 7;
  env.frame_id = "test-frame";
  return env;
}

jsapi::RpcResponse Roundtrip(jsapi::RpcHost* host,
                             jsapi::RpcRequest request,
                             bool* protocol_ok = nullptr) {
  request.env = MakeEnv();
  const std::string request_json = jsapi::RpcHost::EncodeRequest(request);
  jsapi::RpcRequest parsed;
  std::string error;
  const bool parsed_ok =
      jsapi::RpcHost::ParseRequest(request_json, &parsed, &error);
  if (protocol_ok) *protocol_ok = parsed_ok;
  if (!parsed_ok) {
    std::printf("  request parse failed: %s\n", error.c_str());
    jsapi::RpcResponse failed;
    failed.seq = request.seq;
    failed.ok = false;
    failed.error = error;
    return failed;
  }

  jsapi::RpcResponse response = host->Process(parsed);
  const std::string response_json = jsapi::RpcHost::EncodeResponse(response);
  jsapi::JsValue decoded;
  if (!jsapi::ParseJson(response_json, &decoded, &error)) {
    std::printf("  response parse failed: %s\n", error.c_str());
    response.ok = false;
    response.error = error;
    return response;
  }
  return response;
}

jsapi::RpcResponse Execute(jsapi::RpcHost* host,
                           int handle,
                           int index,
                           std::vector<jsapi::JsValue> args,
                           bool async = false) {
  jsapi::RpcRequest request;
  request.seq = 1;
  request.op = "execute";
  request.handle = handle;
  request.index = index;
  request.async = async;
  request.args = std::move(args);
  return Roundtrip(host, std::move(request));
}

jsapi::RpcResponse Describe(jsapi::RpcHost* host, int handle) {
  jsapi::RpcRequest request;
  request.seq = 2;
  request.op = "describe";
  request.handle = handle;
  return Roundtrip(host, std::move(request));
}

jsapi::RpcResponse GetProperty(jsapi::RpcHost* host, int handle, int index) {
  jsapi::RpcRequest request;
  request.seq = 3;
  request.op = "get";
  request.handle = handle;
  request.index = index;
  return Roundtrip(host, std::move(request));
}

int GetNodeHandle(jsapi::RpcHost* host, const std::string& name) {
  const jsapi::RpcResponse root = Describe(host, 0);
  const int index = FindMemberIndex(root.value, name);
  if (index < 0) return -1;
  const jsapi::RpcResponse node = GetProperty(host, 0, index);
  return node.ok ? node.value.AsHandle(-1) : -1;
}

}  // namespace

int main(int argc, char** argv) {
  ::SetConsoleOutputCP(CP_UTF8);

  std::printf("jsapi self test\n");
  std::printf("==============\n");

  // 1) JSON 编解码，含对象句柄/回调句柄编码。
  {
    jsapi::JsValue value;
    std::string error;
    const char* json = R"({"a":1,"b":[true,null,"x"],"c":{"$obj":5},"d":{"$cb":9}})";
    const bool parsed = jsapi::ParseJson(json, &value, &error);
    Check(parsed, "json parse", error);
    if (parsed) {
      const jsapi::JsValue* c = value.Find("c");
      const jsapi::JsValue* d = value.Find("d");
      Check(c && c->type() == jsapi::JsType::ObjectHandle && c->AsHandle() == 5,
            "json object handle decode");
      Check(d && d->type() == jsapi::JsType::CallbackHandle && d->AsHandle() == 9,
            "json callback handle decode");

      jsapi::JsValue roundtrip;
      const bool again = jsapi::ParseJson(value.ToJson(), &roundtrip, &error);
      Check(again && roundtrip.Find("c")->AsHandle() == 5 &&
                roundtrip.Find("d")->AsHandle() == 9,
            "json handle roundtrip", error);
    }
  }

  // 1b) 值模型：JsValue 改为 std::variant 存储后的影响面。
  std::printf("-- value model --\n");
  {
    using jsapi::JsType;
    using jsapi::JsValue;

    // 1b.1 每个工厂落到唯一分支，type() 准确回读。
    Check(JsValue::Null().type() == JsType::Null && JsValue().is_null(),
          "variant: default/null is Null");
    Check(JsValue::Bool(true).type() == JsType::Bool, "variant: bool tag");
    Check(JsValue::Int(-7).type() == JsType::Int, "variant: int tag");
    Check(JsValue::Double(1.5).type() == JsType::Double, "variant: double tag");
    Check(JsValue::String("x").type() == JsType::String, "variant: string tag");
    Check(JsValue::Array().type() == JsType::Array, "variant: array tag");
    Check(JsValue::Object().type() == JsType::Object, "variant: object tag");
    Check(JsValue::ObjectHandle(3).type() == JsType::ObjectHandle,
          "variant: object handle tag");
    Check(JsValue::CallbackHandle(4).type() == JsType::CallbackHandle,
          "variant: callback handle tag");

    // 1b.2 两种句柄底层都是 int，必须仍能区分类型且都能取到 id。
    const JsValue object_handle = JsValue::ObjectHandle(11);
    const JsValue callback_handle = JsValue::CallbackHandle(11);
    Check(object_handle.AsHandle() == 11 && callback_handle.AsHandle() == 11,
          "variant: both handle kinds expose id");
    Check(object_handle.ToJson() == "{\"$obj\":11}" &&
              callback_handle.ToJson() == "{\"$cb\":11}",
          "variant: handle json encoding stays distinct",
          object_handle.ToJson() + " / " + callback_handle.ToJson());
    Check(JsValue::Int(11).AsHandle(-1) == -1,
          "variant: non-handle AsHandle falls back");

    // 1b.3 数值互转语义与原实现一致。
    Check(JsValue::Bool(true).AsInt() == 1 && JsValue::Bool(false).AsInt() == 0,
          "variant: bool -> int");
    Check(JsValue::Int(5).AsBool() && !JsValue::Int(0).AsBool(),
          "variant: int -> bool");
    Check(JsValue::Int(5).AsDouble() == 5.0, "variant: int -> double");
    Check(JsValue::Double(2.9).AsInt() == 2,
          "variant: double -> int truncates");
    Check(JsValue::Double(0.5).AsBool(), "variant: double -> bool");
    Check(JsValue::String("42").AsInt(-1) == -1 &&
              !JsValue::String("1").AsBool(false) &&
              JsValue::String("1").AsDouble(-1.0) == -1.0,
          "variant: string is not coerced to number");

    // 1b.4 类型不符时容器访问返回静态空容器，不产生悬空引用。
    Check(JsValue::Int(1).AsArray().empty() &&
              JsValue::Int(1).AsObject().empty(),
          "variant: container accessors fall back to empty");
    Check(JsValue::Int(1).AsString("d") == "d",
          "variant: string accessor falls back");
    Check(JsValue::Int(1).Find("k") == nullptr,
          "variant: Find on non-object is null");

    // 1b.5 Set / Push 的隐式类型切换与清空行为不变。
    JsValue obj;
    obj.Set("a", JsValue::Int(1));
    obj.Set("b", JsValue::Int(2));
    Check(obj.type() == JsType::Object && obj.AsObject().size() == 2,
          "variant: Set promotes Null to object");
    obj.Set("a", JsValue::Int(9));
    Check(obj.AsObject().size() == 2 && obj.Find("a")->AsInt() == 9,
          "variant: Set overwrites existing key");

    JsValue mixed = JsValue::Object();
    mixed.Set("k", JsValue::Int(1));
    mixed.Push(JsValue::Int(2));
    Check(mixed.type() == JsType::Array && mixed.AsArray().size() == 1 &&
              mixed.AsObject().empty(),
          "variant: Push replaces object with cleared array");

    JsValue array;
    array.Push(JsValue::String("a"));
    array.Push(JsValue::String("b"));
    Check(array.type() == JsType::Array && array.AsArray().size() == 2,
          "variant: Push appends");
    array.Set("k", JsValue::Bool(true));
    Check(array.type() == JsType::Object && array.AsObject().size() == 1 &&
              array.AsArray().empty(),
          "variant: Set replaces array with cleared object");

    // 1b.6 值语义：拷贝后互不影响（variant 深拷贝，不是共享存储）。
    JsValue original = JsValue::Object();
    original.Set("n", JsValue::Int(1));
    JsValue copied = original;
    copied.Set("n", JsValue::Int(2));
    copied.Set("extra", JsValue::Bool(true));
    Check(original.AsObject().size() == 1 && original.Find("n")->AsInt() == 1,
          "variant: object copy is deep");

    JsValue array_copy = JsValue::Array();
    array_copy.Push(JsValue::Int(1));
    JsValue array_copy2 = array_copy;
    array_copy2.Push(JsValue::Int(2));
    Check(array_copy.AsArray().size() == 1 &&
              array_copy2.AsArray().size() == 2,
          "variant: array copy is deep");

    // 1b.7 JSON 往返覆盖全部分支（含嵌套、转义、int64 边界与 -0.0）。
    JsValue all = JsValue::Array();
    all.Push(JsValue::Null());
    all.Push(JsValue::Bool(false));
    all.Push(JsValue::Int(std::numeric_limits<std::int64_t>::max()));
    all.Push(JsValue::Double(-0.0));
    all.Push(JsValue::String("a\"b\\c\n\tz"));
    all.Push(JsValue::ObjectHandle(7));
    all.Push(JsValue::CallbackHandle(8));
    JsValue nested = JsValue::Object();
    nested.Set("arr", JsValue::Array());
    nested.Set("obj", JsValue::Object());
    all.Push(nested);

    std::string error;
    JsValue decoded;
    const bool roundtrip = jsapi::ParseJson(all.ToJson(), &decoded, &error);
    Check(roundtrip, "variant: all-branch json parse", error);
    if (roundtrip) {
      const std::vector<JsValue>& items = decoded.AsArray();
      Check(items.size() == 8, "variant: all-branch json keeps arity");
      Check(items.size() == 8 && items[0].type() == JsType::Null &&
                items[1].type() == JsType::Bool &&
                items[2].type() == JsType::Int &&
                items[2].AsInt() ==
                    std::numeric_limits<std::int64_t>::max() &&
                items[3].type() == JsType::Double &&
                items[4].AsString() == "a\"b\\c\n\tz" &&
                items[5].type() == JsType::ObjectHandle &&
                items[6].type() == JsType::CallbackHandle &&
                items[7].Find("arr") != nullptr &&
                items[7].Find("arr")->type() == JsType::Array &&
                items[7].Find("obj") != nullptr &&
                items[7].Find("obj")->type() == JsType::Object,
            "variant: all-branch json preserves types");
    }

    // 1b.8 体积约束：整型不再携带 string/map/vector 等冗余成员。
    Check(sizeof(JsValue) <= 48, "variant: JsValue stays compact",
          std::to_string(sizeof(JsValue)) + " bytes");
  }

  // 1c) RPC 请求解析：字符串入口与「已解析树」入口必须等价。
  {
    jsapi::RpcRequest request;
    request.seq = 42;
    request.op = "execute";
    request.handle = 3;
    request.index = 2;
    request.async = true;
    request.args.push_back(jsapi::JsValue::Int(5));
    request.env.browser_id = 7;
    request.env.frame_id = "frame-x";

    std::map<std::string, jsapi::JsValue> extras;
    extras["reply_map"] = jsapi::JsValue::String("map-a");
    extras["reply_event"] = jsapi::JsValue::String("event-a");
    const std::string json = jsapi::RpcHost::EncodeRequest(request, extras);

    jsapi::JsValue root;
    std::string error;
    const bool tree_ok = jsapi::ParseJson(json, &root, &error);
    Check(tree_ok, "rpc: extras json parses once", error);

    jsapi::RpcRequest from_tree;
    jsapi::RpcRequest from_text;
    const bool tree_parse =
        jsapi::RpcHost::ParseRequest(root, &from_tree, &error);
    const bool text_parse =
        jsapi::RpcHost::ParseRequest(json, &from_text, &error);
    Check(tree_parse && text_parse, "rpc: both parse entrances succeed", error);
    Check(from_tree.seq == 42 && from_tree.op == "execute" &&
              from_tree.handle == 3 && from_tree.index == 2 &&
              from_tree.async && from_tree.args.size() == 1 &&
              from_tree.args[0].AsInt() == 5 &&
              from_tree.env.browser_id == 7 &&
              from_tree.env.frame_id == "frame-x",
          "rpc: parsed tree keeps all request fields");
    Check(from_tree.seq == from_text.seq &&
              from_tree.index == from_text.index &&
              from_tree.args.size() == from_text.args.size(),
          "rpc: tree and text entrances agree");
    Check(root.Find("reply_map") != nullptr &&
              root.Find("reply_map")->AsString() == "map-a",
          "rpc: extras stay readable from the same tree");

    jsapi::RpcRequest bad;
    Check(!jsapi::RpcHost::ParseRequest(jsapi::JsValue::Int(1), &bad, &error),
          "rpc: non-object root is rejected");
  }

  jsapi::JsRegistry registry;
  TestEventSink sink;
  registry.SetEventSink(&sink);
  registry.RegisterNode("host", &jsapi_demo::CreateHostNode);

  // 2) 根节点与命名节点：用 RpcHost 驱动同一套 RPC 协议。
  jsapi::RpcHost rpc(&registry);

  std::printf("-- core / rpc --\n");

  {
    const jsapi::RpcResponse root = Describe(&rpc, 0);
    Check(root.ok, "root describe ok", root.error);
    Check(FindMemberIndex(root.value, "node") < 0, "root no longer has node()");
    Check(FindMemberIndex(root.value, "nodes") >= 0, "root has nodes()");
    Check(FindMemberIndex(root.value, "addEventListener") >= 0,
          "root has base event members");

    const jsapi::RpcResponse names =
        Execute(&rpc, 0, FindMemberIndex(root.value, "nodes"), {});
    bool has_host = false;
    for (const auto& item : names.value.AsArray()) {
      if (item.AsString() == "host") has_host = true;
    }
    Check(names.ok && has_host, "root.nodes contains host", names.error);
  }

  // 3) 静态注册节点的函数表调用。
  int host_handle = -1;
  {
    host_handle = GetNodeHandle(&rpc, "host");
    Check(host_handle >= 0, "Extention.host returns handle");
    if (host_handle < 0) {
      std::printf("self test aborted: host node missing\n");
      return 1;
    }

    // 根对象属性形式：Extention.host 取到的句柄与直接调用一致。
    const jsapi::RpcResponse root_desc = Describe(&rpc, 0);
    const int host_property = FindMemberIndex(root_desc.value, "host");
    Check(host_property >= 0, "root exposes host property");
    if (host_property >= 0) {
      const jsapi::RpcResponse prop = GetProperty(&rpc, 0, host_property);
      Check(prop.ok && prop.value.type() == jsapi::JsType::ObjectHandle &&
                prop.value.AsHandle(-1) == host_handle,
            "Extention.host property resolves to handle", prop.error);
    }

    // 重复注册同名节点：已实例化的句柄保持不变，页面上已有代理继续有效。
    registry.RegisterNode("host", &jsapi_demo::CreateHostNode);
    Check(GetNodeHandle(&rpc, "host") == host_handle,
          "re-register keeps existing handle");

    // 直接注册已构造好的节点实例，首次访问时才分配句柄。
    registry.RegisterNode("host_inst", jsapi_demo::CreateHostNode());
    const int inst_handle = GetNodeHandle(&rpc, "host_inst");
    Check(inst_handle >= 0, "instance registration returns handle");
    if (inst_handle >= 0) {
      const jsapi::RpcResponse inst_desc = Describe(&rpc, inst_handle);
      const int inst_echo = FindMemberIndex(inst_desc.value, "echo");
      const jsapi::RpcResponse inst_res =
          Execute(&rpc, inst_handle, inst_echo, {jsapi::JsValue::String("hi")});
      Check(inst_res.ok && inst_res.value.AsString() == "hi",
            "instance node is callable", inst_res.error);
    }

    const jsapi::RpcResponse desc = Describe(&rpc, host_handle);
    Check(desc.ok, "host describe ok", desc.error);

    const int echo = FindMemberIndex(desc.value, "echo");
    const int mul = FindMemberIndex(desc.value, "mul");
    Check(echo >= 0 && mul >= 0, "host function table has echo/mul");

    const jsapi::RpcResponse echo_res =
        Execute(&rpc, host_handle, echo, {jsapi::JsValue::String("hello")});
    Check(echo_res.ok && echo_res.value.AsString() == "hello",
          "host echo() via handle+index", echo_res.error);

    const jsapi::RpcResponse mul_res =
        Execute(&rpc, host_handle, mul,
                {jsapi::JsValue::Int(6), jsapi::JsValue::Int(7)});
    Check(mul_res.ok && mul_res.value.AsInt() == 42, "host mul(6,7) == 42",
          mul_res.error);

    const jsapi::RpcResponse bad_type =
        Execute(&rpc, host_handle, echo, {jsapi::JsValue::Int(1)});
    Check(!bad_type.ok && bad_type.error.find("expected string") != std::string::npos,
          "argument type check rejects int for string", bad_type.error);
  }

  // 4) 属性 get / set。
  {
    const jsapi::RpcResponse desc = Describe(&rpc, host_handle);
    const int counter = FindMemberIndex(desc.value, "counter");
    Check(counter >= 0, "host has writable property counter");

    jsapi::RpcRequest set_request;
    set_request.seq = 3;
    set_request.op = "set";
    set_request.handle = host_handle;
    set_request.index = counter;
    set_request.args = {jsapi::JsValue::Int(123)};
    const jsapi::RpcResponse set_res = Roundtrip(&rpc, set_request);
    Check(set_res.ok, "property counter set", set_res.error);

    jsapi::RpcRequest get_request;
    get_request.seq = 4;
    get_request.op = "get";
    get_request.handle = host_handle;
    get_request.index = counter;
    const jsapi::RpcResponse get_res = Roundtrip(&rpc, get_request);
    Check(get_res.ok && get_res.value.AsInt() == 123,
          "property counter get reflects set", get_res.error);

    const int app = FindMemberIndex(desc.value, "app");
    jsapi::RpcRequest readonly_set;
    readonly_set.seq = 5;
    readonly_set.op = "set";
    readonly_set.handle = host_handle;
    readonly_set.index = app;
    readonly_set.args = {jsapi::JsValue::String("x")};
    const jsapi::RpcResponse readonly_res = Roundtrip(&rpc, readonly_set);
    Check(!readonly_res.ok &&
              readonly_res.error.find("read-only") != std::string::npos,
          "read-only property rejects set", readonly_res.error);
  }

  // 5) 事件：注册回调 -> 广播 -> 移除。
  {
    const jsapi::RpcResponse desc = Describe(&rpc, host_handle);
    const int add = FindMemberIndex(desc.value, "addEventListener");
    const int dispatch = FindMemberIndex(desc.value, "dispatchEvent");
    const int remove = FindMemberIndex(desc.value, "removeEventListener");
    Check(add >= 0 && dispatch >= 0 && remove >= 0,
          "base event function table present");

    sink.events.clear();
    const jsapi::RpcResponse add_res =
        Execute(&rpc, host_handle, add,
                {jsapi::JsValue::String("hello"),
                 jsapi::JsValue::CallbackHandle(77)});
    Check(add_res.ok && add_res.value.AsBool(), "addEventListener", add_res.error);

    const jsapi::RpcResponse dispatch_res =
        Execute(&rpc, host_handle, dispatch,
                {jsapi::JsValue::String("hello"),
                 jsapi::JsValue::Array({jsapi::JsValue::String("a"),
                                        jsapi::JsValue::Int(5)})});
    Check(dispatch_res.ok && dispatch_res.value.AsInt() == 1,
          "dispatchEvent reports 1 listener", dispatch_res.error);
    Check(sink.events.size() == 1 && sink.events[0].callback == 77 &&
              sink.events[0].env.browser_id == 7 &&
              sink.events[0].env.frame_id == "test-frame" &&
              sink.events[0].args.size() == 2,
          "event sink receives callback target and args");

    const jsapi::RpcResponse remove_res =
        Execute(&rpc, host_handle, remove,
                {jsapi::JsValue::String("hello"),
                 jsapi::JsValue::CallbackHandle(77)});
    Check(remove_res.ok && remove_res.value.AsBool(), "removeEventListener",
          remove_res.error);

    sink.events.clear();
    const jsapi::RpcResponse dispatch_again =
        Execute(&rpc, host_handle, dispatch,
                {jsapi::JsValue::String("hello")});
    Check(dispatch_again.ok && dispatch_again.value.AsInt() == 0 &&
              sink.events.empty(),
          "no event delivered after removeEventListener", dispatch_again.error);
  }

  // 5.2) 定向派发：多个页面在同一节点注册监听，按 browser/frame 只送目标页面。
  std::printf("-- targeted dispatch --\n");
  {
    const jsapi::RpcResponse desc = Describe(&rpc, host_handle);
    const int dispatch = FindMemberIndex(desc.value, "dispatchEvent");
    Check(dispatch >= 0, "dispatchEvent present for targeted dispatch");

    auto host_object = registry.GetObjectByHandle(host_handle);
    Check(host_object != nullptr, "resolve host node object");

    jsapi::RpcEnv frame_a;
    frame_a.browser_id = 7;
    frame_a.frame_id = "frame-a";
    jsapi::RpcEnv frame_b_other_browser;
    frame_b_other_browser.browser_id = 8;
    frame_b_other_browser.frame_id = "frame-b";
    jsapi::RpcEnv frame_b_same_browser;
    frame_b_same_browser.browser_id = 7;
    frame_b_same_browser.frame_id = "frame-b";
    const jsapi::RpcEnv caller_env = MakeEnv();  // 7 / test-frame

    host_object->AddEventListener("targeted", 101, frame_a);
    host_object->AddEventListener("targeted", 102, frame_b_other_browser);
    host_object->AddEventListener("targeted", 103, frame_b_same_browser);
    host_object->AddEventListener("targeted", 104, caller_env);

    sink.events.clear();
    const int exact = host_object->DispatchEvent(
        "targeted", frame_a, {jsapi::JsValue::String("only-a")});
    Check(exact == 1 && sink.events.size() == 1 &&
              sink.events[0].callback == 101 &&
              sink.events[0].env.frame_id == "frame-a",
          "DispatchEvent filters by frame");

    sink.events.clear();
    jsapi::RpcEnv browser_only;
    browser_only.browser_id = 7;
    const int by_browser =
        host_object->DispatchEvent("targeted", browser_only, {});
    Check(by_browser == 3 && sink.events.size() == 3,
          "DispatchEvent with empty frame matches all frames of browser");

    sink.events.clear();
    jsapi::RpcEnv frame_only;
    frame_only.frame_id = "frame-b";
    const int by_frame =
        host_object->DispatchEvent("targeted", frame_only, {});
    Check(by_frame == 2 && sink.events.size() == 2,
          "DispatchEvent with browser_id -1 matches every browser");

    sink.events.clear();
    jsapi::RpcEnv missing;
    missing.browser_id = 99;
    const int none = host_object->DispatchEvent("targeted", missing, {});
    Check(none == 0 && sink.events.empty(),
          "DispatchEvent to unknown browser delivers nothing");

    sink.events.clear();
    Check(host_object->BroadcastEvent("targeted", {}) == 4,
          "BroadcastEvent still reaches every listener");

    // JS 侧：dispatchEvent(event, args, { browser, frame, excludeSelf })
    sink.events.clear();
    std::map<std::string, jsapi::JsValue> target_options;
    target_options["browser"] = jsapi::JsValue::Int(8);
    target_options["frame"] = jsapi::JsValue::String("frame-b");
    const jsapi::RpcResponse js_target = Execute(
        &rpc, host_handle, dispatch,
        {jsapi::JsValue::String("targeted"),
         jsapi::JsValue::Array({jsapi::JsValue::String("payload")}),
         jsapi::JsValue::Object(target_options)});
    Check(js_target.ok && js_target.value.AsInt() == 1 &&
              sink.events.size() == 1 && sink.events[0].callback == 102,
          "js dispatchEvent honors browser/frame target", js_target.error);

    sink.events.clear();
    std::map<std::string, jsapi::JsValue> exclude_options;
    exclude_options["browser"] = jsapi::JsValue::Int(7);
    exclude_options["excludeSelf"] = jsapi::JsValue::Bool(true);
    const jsapi::RpcResponse js_exclude = Execute(
        &rpc, host_handle, dispatch,
        {jsapi::JsValue::String("targeted"),
         jsapi::JsValue::Array({}),
         jsapi::JsValue::Object(exclude_options)});
    Check(js_exclude.ok && js_exclude.value.AsInt() == 2,
          "js dispatchEvent excludeSelf skips caller frame", js_exclude.error);
    for (const auto& record : sink.events) {
      Check(record.env.frame_id != caller_env.frame_id,
            "excludeSelf never delivers back to caller frame");
    }

    host_object->RemoveEventListener("targeted", 101);
    host_object->RemoveEventListener("targeted", 102);
    host_object->RemoveEventListener("targeted", 103);
    host_object->RemoveEventListener("targeted", 104);
    sink.events.clear();
    Check(host_object->BroadcastEvent("targeted", {}) == 0,
          "targeted listeners removed");
  }

  // 5.5) 生命周期：由类内宏写死，临时节点每次访问新建，代理回收后释放。
  std::printf("-- life type --\n");
  {
    registry.RegisterNode("tmp", [] {
      return std::make_shared<TransientTestNode>();
    });
    const int tmp_first = GetNodeHandle(&rpc, "tmp");
    const int tmp_second = GetNodeHandle(&rpc, "tmp");
    Check(tmp_first > 0 && tmp_second > tmp_first,
          "transient node creates a new instance per access");

    // describe 相当于渲染进程建立一层代理，浏览器侧引用计数 +1。
    // 同一句柄被 describe 三次 → 三层代理，必须三次 release 才真正销毁。
    Check(Describe(&rpc, tmp_first).ok, "transient describe ok");
    const jsapi::JsRegistry::ObjectStats stats_one = registry.GetObjectStats();
    Check(Describe(&rpc, tmp_first).ok, "transient describe second proxy");
    Check(Describe(&rpc, tmp_first).ok, "transient describe third proxy");
    const jsapi::JsRegistry::ObjectStats stats_three = registry.GetObjectStats();
    Check(stats_three.release_refs == stats_one.release_refs + 2,
          "describe increments release refs per proxy");

    Check(registry.ReleaseObject(tmp_first), "release proxy #1");
    Check(registry.GetObjectByHandle(tmp_first) != nullptr,
          "object survives while proxies remain");
    Check(registry.ReleaseObject(tmp_first), "release proxy #2");
    Check(registry.GetObjectByHandle(tmp_first) != nullptr,
          "object survives until last proxy");
    Check(registry.ReleaseObject(tmp_first), "release last proxy destroys object");
    Check(registry.GetObjectByHandle(tmp_first) == nullptr,
          "released handle is no longer resolvable");
    Check(!Describe(&rpc, tmp_first).ok, "describe released handle fails");
    Check(!registry.ReleaseObject(tmp_first), "double release is ignored");
    Check(!registry.ReleaseObject(-1), "release invalid handle is ignored");
    Check(registry.GetObjectStats().transient_live < stats_three.transient_live,
          "live transient count drops after release");

    // 单例节点不受 release 影响。
    Check(!registry.ReleaseObject(host_handle), "singleton is not releasable");
    Check(registry.GetObjectByHandle(host_handle) != nullptr,
          "singleton survives release");
  }

  // 6) 动态插件加载。
  std::printf("-- plugin --\n");
  std::string plugin_path;
  if (argc > 1) {
    plugin_path = argv[1];
  } else {
    plugin_path = WideToUtf8(GetExecutableDir()) + "\\demo_plugin.dll";
  }

  std::string load_error;
  jsapi::JsRegistry::Factory factory =
      jsapi::plugins::LoadDll(plugin_path, &load_error);
  Check(factory != nullptr,
        "load demo_plugin.dll (" + plugin_path + ")", load_error);
  if (factory) {
    registry.RegisterNode("demo", std::move(factory));

    const int demo_handle = GetNodeHandle(&rpc, "demo");
    Check(demo_handle >= 0, "Extention.demo returns handle");
    if (demo_handle >= 0) {
      const jsapi::RpcResponse desc = Describe(&rpc, demo_handle);
      Check(desc.ok, "demo describe ok", desc.error);

      const int add = FindMemberIndex(desc.value, "add");
      const int sum_async = FindMemberIndex(desc.value, "sumAsync");
      const int fail = FindMemberIndex(desc.value, "fail");
      const int child = FindMemberIndex(desc.value, "child");
      const int version = FindMemberIndex(desc.value, "version");
      const int count = FindMemberIndex(desc.value, "count");
      Check(add >= 0 && sum_async >= 0 && fail >= 0 && child >= 0,
            "plugin function table indexes resolved");

      const jsapi::RpcResponse add_res =
          Execute(&rpc, demo_handle, add,
                  {jsapi::JsValue::Int(2), jsapi::JsValue::Int(3)});
      Check(add_res.ok && add_res.value.AsInt() == 5,
            "plugin add(2,3) == 5", add_res.error);

      const jsapi::RpcResponse async_res =
          Execute(&rpc, demo_handle, sum_async,
                  {jsapi::JsValue::Int(20), jsapi::JsValue::Int(22)},
                  /*async=*/true);
      Check(async_res.ok && async_res.value.AsInt() == 42,
            "async declared sumAsync returns 42", async_res.error);

      const jsapi::RpcResponse fail_res = Execute(&rpc, demo_handle, fail, {});
      Check(!fail_res.ok &&
                fail_res.error.find("intentional") != std::string::npos,
            "plugin failure becomes RPC error", fail_res.error);

      jsapi::RpcRequest version_get;
      version_get.seq = 6;
      version_get.op = "get";
      version_get.handle = demo_handle;
      version_get.index = version;
      const jsapi::RpcResponse version_res = Roundtrip(&rpc, version_get);
      Check(version_res.ok && version_res.value.AsString() == "demo-1.2.0",
            "plugin read-only property version", version_res.error);

      jsapi::RpcRequest count_get;
      count_get.seq = 7;
      count_get.op = "get";
      count_get.handle = demo_handle;
      count_get.index = count;
      const jsapi::RpcResponse count_res = Roundtrip(&rpc, count_get);
      Check(count_res.ok && count_res.value.AsInt() == 10,
            "plugin default count == 10", count_res.error);

      const jsapi::RpcResponse child_res =
          Execute(&rpc, demo_handle, child, {});
      Check(child_res.ok && child_res.value.AsHandle() > demo_handle,
            "plugin child() allocates a new handle", child_res.error);
      if (child_res.ok) {
        const int child_handle = child_res.value.AsHandle();
        const jsapi::RpcResponse child_desc = Describe(&rpc, child_handle);
        const int ping = FindMemberIndex(child_desc.value, "ping");
        const jsapi::RpcResponse ping_res =
            Execute(&rpc, child_handle, ping,
                    {jsapi::JsValue::String("hi")});
        Check(ping_res.ok && ping_res.value.AsString() == "pong: hi",
              "child node ping works through its own table", ping_res.error);
      }

      // Qt 实现的新接口：注册表读写 + 异步读文件。
      const int set_reg = FindMemberIndex(desc.value, "setRegValue");
      const int get_reg = FindMemberIndex(desc.value, "getRegValue");
      const int read_file = FindMemberIndex(desc.value, "readFile");
      Check(set_reg >= 0 && get_reg >= 0 && read_file >= 0,
            "plugin declares setRegValue/getRegValue/readFile");

      const jsapi::RpcResponse set_res =
          Execute(&rpc, demo_handle, set_reg,
                  {jsapi::JsValue::String("self_test_value"),
                   jsapi::JsValue::String("hello-reg")});
      Check(set_res.ok && set_res.value.AsBool(),
            "setRegValue writes registry", set_res.error);

      const jsapi::RpcResponse get_res =
          Execute(&rpc, demo_handle, get_reg,
                  {jsapi::JsValue::String("self_test_value"),
                   jsapi::JsValue::String("fallback")});
      Check(get_res.ok && get_res.value.AsString() == "hello-reg",
            "getRegValue reads back value", get_res.error);

      const jsapi::RpcResponse default_res =
          Execute(&rpc, demo_handle, get_reg,
                  {jsapi::JsValue::String("self_test_missing_key"),
                   jsapi::JsValue::String("fallback")});
      Check(default_res.ok && default_res.value.AsString() == "fallback",
            "getRegValue returns default for missing key", default_res.error);

      const std::string file_content = "jsapi async file content";
      const std::string temp_file = WriteTempFile(file_content);
      const jsapi::RpcResponse read_res =
          Execute(&rpc, demo_handle, read_file,
                  {jsapi::JsValue::String(temp_file)}, /*async=*/true);
      Check(read_res.ok && read_res.value.AsString() == file_content,
            "readFile returns file content via async response", read_res.error);
    }
  }

  std::printf("==============\n");
  std::printf("%d/%d checks passed\n", g_checks - g_failures, g_checks);
  return g_failures == 0 ? 0 : 1;
}
