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

    // 同步事件往返：C++ 阻塞等前端回调返回，是同步嵌套调用的最基础形态。
    // 前端先 addEventListener("sync-echo", fn)，回调返回值原样带回 JS。
    table.Function(
        "syncEcho", jsapi::JsType::String, {{"text", jsapi::JsType::String}},
        [](jsapi::JsObjectBase* self, jsapi::JsCallContext& ctx) {
          std::string error;
          const jsapi::JsValue result = self->DispatchEventSync(
              "sync-echo", ctx.env(),
              {jsapi::JsValue::String(ctx.args()[0].AsString())}, 5000, &error);
          if (!error.empty()) {
            ctx.Fail(error);
            return;
          }
          ctx.Return(result);
        });

    // 多层同步嵌套：前端回调里再次调用 syncNest(depth-1)，形成
    // C++ -> JS -> C++ -> ... 的同步调用链，每一层都在同一调用栈上等待。
    table.Function(
        "syncNest", jsapi::JsType::String,
        {{"depth", jsapi::JsType::Int}, {"token", jsapi::JsType::String}},
        [](jsapi::JsObjectBase* self, jsapi::JsCallContext& ctx) {
          std::string error;
          const jsapi::JsValue result = self->DispatchEventSync(
              "sync-nest", ctx.env(),
              {jsapi::JsValue::Int(ctx.args()[0].AsInt()),
               jsapi::JsValue::String(ctx.args()[1].AsString())},
              5000, &error);
          if (!error.empty()) {
            ctx.Fail(error);
            return;
          }
          ctx.Return(result);
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
