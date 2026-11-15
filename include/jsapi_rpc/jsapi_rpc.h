#pragma once

#include <functional>
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

  // UI 亲和成员的执行器：把函数体搬到宿主 UI 主线程执行并同步等待返回。
  // 实现方不应在闭包里再加锁——调用线程已经持有所需上下文，闭包只跑函数体。
  using UiExecutor = std::function<void(const std::function<void()>&)>;

  // ui_executor 为空时所有成员都在当前线程执行（单测/无 UI 宿主场景）。
  RpcResponse Process(const RpcRequest& request,
                      const UiExecutor& ui_executor = nullptr);

 private:
  JsRegistry* registry_ = nullptr;
};

// 按声明的参数类型做简单强转；失败时写 error。
bool CoerceValue(const JsValue& input,
                 JsType type,
                 JsValue* output,
                 std::string* error);

}  // namespace jsapi
