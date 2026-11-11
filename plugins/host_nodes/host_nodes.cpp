#include "host_nodes.h"

#include <windows.h>

#include <chrono>
#include <ctime>

namespace jsapi_demo {
namespace {

class HostNode final : public jsapi::JsObjectBase {
 protected:
  void DeclareMembers(jsapi::JsTableBuilder& table) const override {
    table.Function(
        "echo", jsapi::JsType::String, {{"text", jsapi::JsType::String}},
        [](jsapi::JsObjectBase*, jsapi::JsCallContext& ctx) {
          ctx.Return(jsapi::JsValue::String(ctx.args()[0].AsString()));
        });

    table.Function(
        "mul", jsapi::JsType::Int,
        {{"a", jsapi::JsType::Int}, {"b", jsapi::JsType::Int}},
        [](jsapi::JsObjectBase*, jsapi::JsCallContext& ctx) {
          ctx.Return(jsapi::JsValue::Int(ctx.args()[0].AsInt() *
                                         ctx.args()[1].AsInt()));
        });

    table.Function("now", jsapi::JsType::String, {},
                   [](jsapi::JsObjectBase*, jsapi::JsCallContext& ctx) {
                     const auto now = std::chrono::system_clock::now();
                     const std::time_t t = std::chrono::system_clock::to_time_t(now);
                     std::tm tm{};
                     localtime_s(&tm, &t);
                     char buf[64] = {};
                     std::strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &tm);
                     ctx.Return(jsapi::JsValue::String(buf));
                   });

    table.Property("app", jsapi::JsType::String,
                   [](jsapi::JsObjectBase*) {
                     return jsapi::JsValue::String("jsapi_demo_qt_host");
                   });

    table.Property("pid", jsapi::JsType::Int,
                   [](jsapi::JsObjectBase*) {
                     return jsapi::JsValue::Int(::GetCurrentProcessId());
                   });

    table.Property(
        "counter", jsapi::JsType::Int,
        [](jsapi::JsObjectBase* self) {
          return jsapi::JsValue::Int(static_cast<HostNode*>(self)->counter_);
        },
        [](jsapi::JsObjectBase* self, const jsapi::JsValue& value,
           std::string* error) {
          const std::int64_t next = value.AsInt();
          if (next < 0) {
            if (error) *error = "counter must be >= 0";
            return false;
          }
          static_cast<HostNode*>(self)->counter_ = next;
          return true;
        });
  }

 private:
  std::int64_t counter_ = 0;
};

}  // namespace

std::shared_ptr<jsapi::JsObjectBase> CreateHostNode() {
  return std::make_shared<HostNode>();
}

}  // namespace jsapi_demo
