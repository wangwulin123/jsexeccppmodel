#pragma once

#include <map>
#include <string>
#include <vector>

#include "jsapi/js_object.h"
#include "jsapi/js_registry.h"

namespace jsapi {

// 浏览器进程与渲染进程之间的 RPC 请求。
// 渲染进程只发 handle + index，不发明文函数名。
struct RpcRequest {
  int seq = 0;
  std::string op;  // describe / get / set / execute
  int handle = 0;
  int index = -1;
  bool async = false;
  std::vector<JsValue> args;
  RpcEnv env;
};

struct RpcResponse {
  int seq = 0;
  bool ok = true;
  JsValue value;
  std::string error;
};

// 浏览器进程侧的 JSAPI 执行器：解析请求、按函数表索引分发、做参数类型检查，
// 再把结果序列化。它不依赖 CEF，方便单测。
class RpcHost {
 public:
  explicit RpcHost(JsRegistry* registry) : registry_(registry) {}

  static bool ParseRequest(const std::string& json,
                           RpcRequest* request,
                           std::string* error);
  // 复用已解析的 JSON 树，避免调用方为取附加字段而重复解析整条请求。
  static bool ParseRequest(const JsValue& root,
                           RpcRequest* request,
                           std::string* error);
  static std::string EncodeRequest(
      const RpcRequest& request,
      const std::map<std::string, JsValue>& extras = {});

  static std::string EncodeResponse(const RpcResponse& response);

  RpcResponse Process(const RpcRequest& request);

 private:
  JsRegistry* registry_ = nullptr;
};

// 按声明的参数类型做简单强转；失败时写 error。
bool CoerceValue(const JsValue& input,
                 JsType type,
                 JsValue* output,
                 std::string* error);

}  // namespace jsapi
