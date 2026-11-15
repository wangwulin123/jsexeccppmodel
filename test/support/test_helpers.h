#pragma once

#include <string>
#include <vector>

#include "jsapi/js_value.h"
#include "jsapi_rpc/jsapi_rpc.h"

namespace test {

std::string WideToUtf8(const std::wstring& text);
std::string GetExecutableDir();

// 统一的测试页面身份：browser=7 / frame=test-frame。
jsapi::RpcEnv MakeEnv();

int FindMemberIndex(const jsapi::JsValue& describe, const std::string& name);

// 走完整协议：编码请求 -> 解析请求 -> 执行 -> 编码响应 -> 解析响应。
jsapi::RpcResponse Roundtrip(jsapi::RpcHost* host,
                             jsapi::RpcRequest request,
                             bool* protocol_ok = nullptr);

jsapi::RpcResponse Execute(jsapi::RpcHost* host,
                           int handle,
                           int index,
                           std::vector<jsapi::JsValue> args,
                           bool async = false);

jsapi::RpcResponse Describe(jsapi::RpcHost* host, int handle);

jsapi::RpcResponse GetProperty(jsapi::RpcHost* host, int handle, int index);

int GetNodeHandle(jsapi::RpcHost* host, const std::string& name);

}  // namespace test
