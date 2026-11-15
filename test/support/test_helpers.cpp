#include "support/test_helpers.h"

#include <windows.h>

#include <cstdio>
#include <string>
#include <utility>

namespace test {

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

std::string GetExecutableDir() {
  wchar_t path[MAX_PATH] = {};
  ::GetModuleFileNameW(::GetModuleHandleW(nullptr), path, MAX_PATH);
  std::wstring dir(path);
  const std::size_t pos = dir.find_last_of(L"\\/");
  if (pos != std::wstring::npos) {
    dir.resize(pos);
  }
  return WideToUtf8(dir);
}

jsapi::RpcEnv MakeEnv() {
  jsapi::RpcEnv env;
  env.browser_id = 7;
  env.frame_id = "test-frame";
  return env;
}

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

jsapi::RpcResponse Roundtrip(jsapi::RpcHost* host,
                             jsapi::RpcRequest request,
                             bool* protocol_ok) {
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
                           bool async) {
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

}  // namespace test
