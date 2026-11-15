#pragma once

#include <windows.h>

#include <cstdio>
#include <functional>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "host_nodes.h"
#include "jsapi/js_object.h"
#include "support/test_helpers.h"

namespace test {

// 用类内宏声明生命周期，验证临时节点行为。
class TransientTestNode final : public jsapi::JsObjectBase {
 public:
  JSAPI_NODE_LIFE_TYPE(jsapi::LifeType::Transient);
};

// 线程亲和用例：函数/属性都返回函数体实际所在的线程 id。
class AffinityTestNode final : public jsapi::JsObjectBase {
 public:
  JSAPI_NODE_LIFE_TYPE(jsapi::LifeType::Singleton);

  std::uint32_t constructor_thread() const { return ctor_thread_; }

 protected:
  void DeclareMembers(jsapi::JsTableBuilder& table) const override {
    // 默认亲和 Worker：在收取请求的线程上就地执行。
    table.Function("workerFn", jsapi::JsType::Int, {},
                   [](jsapi::JsObjectBase*, jsapi::JsCallContext& ctx) {
                     ctx.Return(jsapi::JsValue::Int(::GetCurrentThreadId()));
                   });
    table.Function("uiFn", jsapi::JsType::Int, {},
                   [](jsapi::JsObjectBase*, jsapi::JsCallContext& ctx) {
                     ctx.Return(jsapi::JsValue::Int(::GetCurrentThreadId()));
                   },
                   /*async=*/false, jsapi::ThreadAffinity::UI);
    table.Property("workerProp", jsapi::JsType::Int,
                   [](jsapi::JsObjectBase*) {
                     return jsapi::JsValue::Int(::GetCurrentThreadId());
                   });
    table.Property("uiProp", jsapi::JsType::Int,
                   [](jsapi::JsObjectBase*) {
                     return jsapi::JsValue::Int(::GetCurrentThreadId());
                   },
                   nullptr, jsapi::ThreadAffinity::UI);
  }

 private:
  std::uint32_t ctor_thread_ = ::GetCurrentThreadId();
};

// 异常兜底用例：插件函数/属性抛出 C++ 异常时必须转成 RPC 错误而不是终止进程，
// 且异常之后该节点仍可正常调用。
class ThrowingTestNode final : public jsapi::JsObjectBase {
 public:
  JSAPI_NODE_LIFE_TYPE(jsapi::LifeType::Singleton);

 protected:
  void DeclareMembers(jsapi::JsTableBuilder& table) const override {
    table.Function("throwStd", jsapi::JsType::Int, {},
                   [](jsapi::JsObjectBase*, jsapi::JsCallContext&) {
                     throw std::runtime_error("std failure");
                   });
    table.Function("throwNonStd", jsapi::JsType::Int, {},
                   [](jsapi::JsObjectBase*, jsapi::JsCallContext&) {
                     throw 42;
                   });
    table.Function("okFn", jsapi::JsType::Int, {},
                   [](jsapi::JsObjectBase*, jsapi::JsCallContext& ctx) {
                     ctx.Return(jsapi::JsValue::Int(7));
                   });
    table.Property("throwProp", jsapi::JsType::Int,
                   [](jsapi::JsObjectBase*) -> jsapi::JsValue {
                     throw std::runtime_error("getter failure");
                   });
    table.Property("okProp", jsapi::JsType::Int,
                   [](jsapi::JsObjectBase*) {
                     return jsapi::JsValue::Int(9);
                   });
  }
};

// 返回无法直接表示为 JSON 的值（NaN / Infinity / INT64 极值）。
class RawValueTestNode final : public jsapi::JsObjectBase {
 public:
  JSAPI_NODE_LIFE_TYPE(jsapi::LifeType::Singleton);

 protected:
  void DeclareMembers(jsapi::JsTableBuilder& table) const override {
    table.Function(
        "nan", jsapi::JsType::Double, {},
        [](jsapi::JsObjectBase*, jsapi::JsCallContext& ctx) {
          ctx.Return(jsapi::JsValue::Double(
              std::numeric_limits<double>::quiet_NaN()));
        });
    table.Function(
        "infinite", jsapi::JsType::Double, {},
        [](jsapi::JsObjectBase*, jsapi::JsCallContext& ctx) {
          ctx.Return(jsapi::JsValue::Double(
              std::numeric_limits<double>::infinity()));
        });
    table.Function(
        "intMin", jsapi::JsType::Int, {},
        [](jsapi::JsObjectBase*, jsapi::JsCallContext& ctx) {
          ctx.Return(jsapi::JsValue::Int(
              std::numeric_limits<std::int64_t>::min()));
        });
  }
};

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

  int OnEventSync(const std::string& event,
                  int callback_id,
                  const jsapi::RpcEnv& target,
                  const std::vector<jsapi::JsValue>& args,
                  int /*timeout_ms*/,
                  jsapi::JsValue* result,
                  std::string* error) override {
    sync_calls.push_back(EventRecord{event, callback_id, target, args});
    if (!sync_handler) {
      if (result) *result = jsapi::JsValue::String("sync:" + event);
      return 1;
    }
    return sync_handler(callback_id, args, result, error);
  }

  std::vector<EventRecord> events;
  std::vector<EventRecord> sync_calls;
  // 返回值语义同 JsEventSink::OnEventSync；用于模拟"回调里再发起同步 RPC"。
  std::function<int(int,
                    const std::vector<jsapi::JsValue>&,
                    jsapi::JsValue*,
                    std::string*)>
      sync_handler;
};

// 未实现同步事件的存量 sink：验证默认实现不会导致编译或行为断裂。
class PlainEventSink final : public jsapi::JsEventSink {
 public:
  void OnEvent(const std::string& /*event*/,
               int /*callback_id*/,
               const jsapi::RpcEnv& /*target*/,
               const std::vector<jsapi::JsValue>& /*args*/) override {}
};

// 写一个临时文件，供异步 readFile 用例读取。
inline std::string WriteTempFile(const std::string& content) {
  char dir[MAX_PATH] = {};
  ::GetTempPathA(MAX_PATH, dir);
  const std::string path = std::string(dir) + "jsapi_readfile_test.txt";
  FILE* fp = nullptr;
  if (::fopen_s(&fp, path.c_str(), "wb") != 0 || fp == nullptr) {
    return {};
  }
  std::fwrite(content.data(), 1, content.size(), fp);
  std::fclose(fp);
  return path;
}

// 可靠性/系统/性能套件共用的最小夹具：注册 host 节点并取得其句柄。
struct CoreFixture {
  jsapi::JsRegistry registry;
  TestEventSink sink;
  jsapi::RpcHost rpc;
  int host_handle = -1;

  CoreFixture() : rpc(&registry) {
    registry.SetEventSink(&sink);
    registry.RegisterNode("host", &jsapi_demo::CreateHostNode);
    host_handle = GetNodeHandle(&rpc, "host");
  }
};

}  // namespace test
