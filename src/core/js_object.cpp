#include "jsapi/js_object.h"

#include <algorithm>

#include "jsapi/js_registry.h"

namespace jsapi {
namespace {

bool EnvMatches(const RpcEnv& listener, const RpcEnv& target) {
  if (target.browser_id >= 0 && listener.browser_id != target.browser_id) {
    return false;
  }
  if (!target.frame_id.empty() && listener.frame_id != target.frame_id) {
    return false;
  }
  return true;
}

bool EnvEquals(const RpcEnv& a, const RpcEnv& b) {
  return a.browser_id == b.browser_id && a.frame_id == b.frame_id;
}

}  // namespace

JsCallContext::JsCallContext(JsObjectBase* self,
                             std::vector<JsValue> args,
                             JsRegistry* registry,
                             const RpcEnv& env)
    : self_(self),
      args_(std::move(args)),
      registry_(registry),
      env_(env) {}

void JsCallContext::Return(JsValue value) {
  has_value_ = true;
  value_ = std::move(value);
  error_.clear();
}

void JsCallContext::Fail(std::string error) {
  has_value_ = false;
  error_ = std::move(error);
}

const JsTable& JsObjectBase::GetTable() const {
  return EnsureTable();
}

void JsObjectBase::Attach(JsRegistry* registry, int handle, const RpcEnv& env) {
  registry_ = registry;
  handle_ = handle;
  env_ = env;
}

void JsObjectBase::InvalidateTable() const {
  std::lock_guard<std::mutex> lock(table_mutex_);
  table_.reset();
}

const JsTable& JsObjectBase::EnsureTable() const {
  std::lock_guard<std::mutex> lock(table_mutex_);
  if (table_) {
    return *table_;
  }

  JsTableBuilder builder;
  DeclareMembers(builder);

  // 所有节点都自动获得事件能力。
  builder.Function(
      "addEventListener", JsType::Bool,
      {{"event", JsType::String}, {"callback", JsType::CallbackHandle}},
      [](JsObjectBase* self, JsCallContext& ctx) {
        const std::string event = ctx.args()[0].AsString();
        const int callback = ctx.args()[1].AsHandle();
        // 非法 callback（如 {"$cb":-1}）或空事件名必须如实返回 false，
        // 否则页面无法区分"注册成功"与"静默丢弃"。
        ctx.Return(JsValue::Bool(
            self->AddEventListener(event, callback, ctx.env())));
      });

  builder.Function(
      "removeEventListener", JsType::Bool,
      {{"event", JsType::String}, {"callback", JsType::CallbackHandle}},
      [](JsObjectBase* self, JsCallContext& ctx) {
        ctx.Return(JsValue::Bool(
            self->RemoveEventListener(ctx.args()[0].AsString(),
                                      ctx.args()[1].AsHandle())));
      });

  builder.Function(
      "dispatchEvent", JsType::Int,
      {{"event", JsType::String},
       {"args", JsType::Array, true},
       {"target", JsType::Any, true}},
      [](JsObjectBase* self, JsCallContext& ctx) {
        std::vector<JsValue> args;
        if (ctx.args().size() > 1 && ctx.args()[1].type() == JsType::Array) {
          args = ctx.args()[1].AsArray();
        }
        // target 缺省时广播；browser/frame 可单独指定，excludeSelf 跳过调用方。
        RpcEnv target;
        bool exclude_self = false;
        if (ctx.args().size() > 2 && ctx.args()[2].type() == JsType::Object) {
          const JsValue& options = ctx.args()[2];
          if (const JsValue* browser = options.Find("browser")) {
            target.browser_id = static_cast<int>(browser->AsInt(-1));
          }
          if (const JsValue* frame = options.Find("frame")) {
            target.frame_id = frame->AsString();
          }
          if (const JsValue* exclude = options.Find("excludeSelf")) {
            exclude_self = exclude->AsBool();
          }
        }
        const RpcEnv* exclude = exclude_self ? &ctx.env() : nullptr;
        ctx.Return(JsValue::Int(self->EmitTo(ctx.args()[0].AsString(), args,
                                             &target, exclude)));
      });

  builder.Function("getAllEventInfo", JsType::Array, {},
                   [](JsObjectBase* self, JsCallContext& ctx) {
                     ctx.Return(self->GetAllEventInfo());
                   });

  table_ = builder.Build();
  return *table_;
}

bool JsObjectBase::AddEventListener(const std::string& event,
                                    int callback_id,
                                    const RpcEnv& env) {
  if (callback_id < 0 || event.empty()) {
    return false;
  }
  std::lock_guard<std::mutex> lock(listener_mutex_);
  listeners_.push_back(Listener{event, callback_id, env});
  return true;
}

bool JsObjectBase::RemoveEventListener(const std::string& event,
                                       int callback_id) {
  std::lock_guard<std::mutex> lock(listener_mutex_);
  for (auto it = listeners_.begin(); it != listeners_.end(); ++it) {
    if (it->event == event && it->callback_id == callback_id) {
      listeners_.erase(it);
      return true;
    }
  }
  return false;
}

int JsObjectBase::BroadcastEvent(const std::string& event,
                                 const std::vector<JsValue>& args) {
  return EmitTo(event, args, nullptr, nullptr);
}

int JsObjectBase::DispatchEvent(const std::string& event,
                                const RpcEnv& target,
                                const std::vector<JsValue>& args) {
  return EmitTo(event, args, &target, nullptr);
}

JsValue JsObjectBase::DispatchEventSync(const std::string& event,
                                        const RpcEnv& target,
                                        const std::vector<JsValue>& args,
                                        int timeout_ms,
                                        std::string* error) {
  if (error) error->clear();

  JsEventSink* sink = registry_ ? registry_->event_sink() : nullptr;
  if (!sink) {
    if (error) *error = "no listener for event '" + event + "'";
    return JsValue::Null();
  }

  // 同步事件只支持单播：listeners_ 是注册顺序的 vector，取首个匹配项。
  Listener target_listener;
  bool found = false;
  {
    std::lock_guard<std::mutex> lock(listener_mutex_);
    for (const auto& listener : listeners_) {
      if (listener.event != event) {
        continue;
      }
      if (!EnvMatches(listener.env, target)) {
        continue;
      }
      target_listener = listener;
      found = true;
      break;
    }
  }
  if (!found) {
    if (error) *error = "no listener for event '" + event + "'";
    return JsValue::Null();
  }

  JsValue result = JsValue::Null();
  std::string reason;
  const int hits = sink->OnEventSync(event, target_listener.callback_id,
                                     target_listener.env, args, timeout_ms,
                                     &result, &reason);
  if (hits < 0) {
    if (error) *error = reason.empty() ? "sync events not supported by this sink"
                                       : reason;
    return JsValue::Null();
  }
  if (hits == 0) {
    if (error) *error = "no listener for event '" + event + "'";
    return JsValue::Null();
  }
  if (!reason.empty()) {
    if (error) *error = reason;
    return JsValue::Null();
  }
  return result;
}

int JsObjectBase::DiscardListeners(int browser_id,
                                   const std::string& keep_frame_id) {
  std::lock_guard<std::mutex> lock(listener_mutex_);
  const std::size_t before = listeners_.size();
  listeners_.erase(
      std::remove_if(listeners_.begin(), listeners_.end(),
                     [&](const Listener& listener) {
                       if (listener.env.browser_id != browser_id) {
                         return false;
                       }
                       return keep_frame_id.empty() ||
                              listener.env.frame_id != keep_frame_id;
                     }),
      listeners_.end());
  return static_cast<int>(before - listeners_.size());
}

int JsObjectBase::EmitTo(const std::string& event,
                         const std::vector<JsValue>& args,
                         const RpcEnv* target,
                         const RpcEnv* exclude) {
  JsEventSink* sink = registry_ ? registry_->event_sink() : nullptr;
  if (!sink) {
    return 0;
  }

  std::vector<Listener> targets;
  {
    std::lock_guard<std::mutex> lock(listener_mutex_);
    for (const auto& listener : listeners_) {
      if (listener.event != event) {
        continue;
      }
      if (target && !EnvMatches(listener.env, *target)) {
        continue;
      }
      if (exclude && EnvEquals(listener.env, *exclude)) {
        continue;
      }
      targets.push_back(listener);
    }
  }

  for (const auto& item : targets) {
    // 单个监听器抛异常不能中断后续投递：事件是广播语义，各监听互不影响。
    try {
      sink->OnEvent(event, item.callback_id, item.env, args);
    } catch (...) {
    }
  }
  return static_cast<int>(targets.size());
}

JsValue JsObjectBase::GetAllEventInfo() const {
  std::vector<JsValue> items;
  {
    std::lock_guard<std::mutex> lock(listener_mutex_);
    for (const auto& listener : listeners_) {
      std::map<std::string, JsValue> info;
      info["event"] = JsValue::String(listener.event);
      info["callback"] = JsValue::CallbackHandle(listener.callback_id);
      info["browser"] = JsValue::Int(listener.env.browser_id);
      info["frame"] = JsValue::String(listener.env.frame_id);
      items.push_back(JsValue::Object(std::move(info)));
    }
  }
  return JsValue::Array(std::move(items));
}

}  // namespace jsapi
