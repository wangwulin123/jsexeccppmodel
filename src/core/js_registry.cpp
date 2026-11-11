#include "jsapi/js_registry.h"

#include <windows.h>

namespace jsapi {
namespace {

// 根节点：window.Extention。
// 它按名字把业务插件注册的命名节点直接挂成属性：前端用 Extention.demo 访问。
class JsRootObject : public JsObjectBase {
 protected:
  void DeclareMembers(JsTableBuilder& table) const override {
    table.Function("nodes", JsType::Array, {},
                   [](JsObjectBase* /*self*/, JsCallContext& ctx) {
                     std::vector<JsValue> names;
                     for (const auto& name : ctx.registry()->NodeNames()) {
                       names.push_back(JsValue::String(name));
                     }
                     ctx.Return(JsValue::Array(std::move(names)));
                   });

    table.Function("version", JsType::String, {},
                   [](JsObjectBase* /*self*/, JsCallContext& ctx) {
                     ctx.Return(JsValue::String("jsapi-lite/1.0"));
                   });

    // 调试/测试用：观察句柄表与临时对象的存活情况。
    table.Function("objectStats", JsType::Object, {},
                   [](JsObjectBase* /*self*/, JsCallContext& ctx) {
                     const JsRegistry::ObjectStats stats =
                         ctx.registry()->GetObjectStats();
                     JsValue result = JsValue::Object();
                     result.Set("handles", JsValue::Int(stats.handles));
                     result.Set("transient_live",
                                JsValue::Int(stats.transient_live));
                     result.Set("release_refs",
                                JsValue::Int(stats.release_refs));
                     ctx.Return(std::move(result));
                   });

    // 业务节点直接挂在根对象上：Extention.demo。
    // 函数表在首次 describe 时才构建，晚于此刻的 RegisterNode 需要刷新页面才可见。
    for (const auto& name : registry()->NodeNames()) {
      if (name == "nodes" || name == "version") {
        continue;
      }
      table.Property(name, JsType::ObjectHandle,
                     [name](JsObjectBase* self) {
                       auto* registry = self ? self->registry() : nullptr;
                       if (!registry) {
                         return JsValue::Null();
                       }
                       auto node = registry->GetNode(name, RpcEnv{});
                       if (!node) {
                         return JsValue::Null();
                       }
                       return JsValue::ObjectHandle(node->handle());
                     });
    }
  }
};

std::wstring Utf8ToWide(const std::string& text) {
  if (text.empty()) {
    return std::wstring();
  }
  const int size = ::MultiByteToWideChar(CP_UTF8, 0, text.data(),
                                         static_cast<int>(text.size()), nullptr, 0);
  std::wstring wide(static_cast<std::size_t>(size), L'\0');
  ::MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()),
                        wide.data(), size);
  return wide;
}

}  // namespace

JsRegistry::JsRegistry() {
  // handle 0 固定是根节点，构造时立即建立，describe(0) 才能直接查到。
  auto root = std::make_shared<JsRootObject>();
  root->Attach(this, 0, RpcEnv{});
  objects_.push_back(std::move(root));
}
JsRegistry::~JsRegistry() = default;

void JsRegistry::SetEventSink(JsEventSink* sink) {
  std::lock_guard<std::mutex> lock(mutex_);
  event_sink_ = sink;
}

int JsRegistry::AddObject(std::shared_ptr<JsObjectBase> object,
                          const RpcEnv& env) {
  if (!object) {
    return -1;
  }
  std::lock_guard<std::mutex> lock(mutex_);
  return AddObjectLocked(std::move(object), env);
}

int JsRegistry::AddObjectLocked(std::shared_ptr<JsObjectBase> object,
                                const RpcEnv& env) {
  const int handle = static_cast<int>(objects_.size());
  object->Attach(this, handle, env);
  objects_.push_back(std::move(object));
  return handle;
}

// 渲染进程的代理被 V8 回收后调用。只处理临时对象：单例句柄不会被缓存进
// nodes_ 以外的释放路径，命名节点缓存句柄也只可能是单例，这里无需额外判断。
// 同一句柄可能被多层代理引用（例如插件返回同一个子对象句柄），按引用计数释放。
bool JsRegistry::ReleaseObject(int handle) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (handle <= 0 || static_cast<std::size_t>(handle) >= objects_.size()) {
    return false;
  }
  auto& slot = objects_[static_cast<std::size_t>(handle)];
  if (!slot || slot->life_type() != LifeType::Transient) {
    return false;
  }

  auto ref = release_refs_.find(handle);
  if (ref != release_refs_.end()) {
    if (--ref->second > 0) {
      return true;
    }
    release_refs_.erase(ref);
  }
  slot.reset();
  return true;
}

JsRegistry::ObjectStats JsRegistry::GetObjectStats() const {
  std::lock_guard<std::mutex> lock(mutex_);
  ObjectStats stats;
  stats.handles = static_cast<int>(objects_.size());
  for (const auto& object : objects_) {
    if (object && object->life_type() == LifeType::Transient) {
      ++stats.transient_live;
    }
  }
  for (const auto& ref : release_refs_) {
    stats.release_refs += ref.second;
  }
  return stats;
}

std::shared_ptr<JsObjectBase> JsRegistry::GetObjectByHandle(int handle) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (handle < 0 || static_cast<std::size_t>(handle) >= objects_.size()) {
    return nullptr;
  }
  return objects_[static_cast<std::size_t>(handle)];
}

std::shared_ptr<JsObjectBase> JsRegistry::Root(const RpcEnv& /*env*/) {
  std::lock_guard<std::mutex> lock(mutex_);
  return objects_[0];
}

void JsRegistry::RegisterNode(const std::string& name, Factory factory) {
  if (name.empty() || !factory) {
    return;
  }
  std::shared_ptr<JsObjectBase> root;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    NodeRecord record;
    record.factory = std::move(factory);
    auto it = nodes_.find(name);
    if (it == nodes_.end()) {
      node_order_.push_back(name);
    } else {
      // 已实例化过就沿用原句柄：旧代理继续指向原来的对象，不会被新工厂顶掉。
      record.handle = it->second.handle;
    }
    nodes_[name] = std::move(record);
    root = objects_.empty() ? nullptr : objects_[0];
  }
  // 根节点的属性来自节点名，新增节点后必须让下次 describe 重新声明。
  // 已加载页面的代理不会自动多出新属性，需要刷新页面。
  if (root) {
    root->InvalidateTable();
  }
}

void JsRegistry::RegisterNode(const std::string& name,
                              std::shared_ptr<JsObjectBase> node) {
  if (!node) {
    return;
  }
  RegisterNode(name, [node]() { return node; });
}

std::shared_ptr<JsObjectBase> JsRegistry::GetNode(const std::string& name,
                                                  const RpcEnv& env) {
  std::lock_guard<std::mutex> lock(mutex_);
  auto it = nodes_.find(name);
  if (it == nodes_.end() || !it->second.factory) {
    return nullptr;
  }
  // 单例：首次访问创建后缓存句柄，所有页面后续都拿到同一份。
  if (it->second.handle >= 0) {
    return objects_[static_cast<std::size_t>(it->second.handle)];
  }
  auto object = it->second.factory();
  if (!object) {
    return nullptr;
  }
  const int handle = AddObjectLocked(object, env);
  // 类型完全由类内声明决定；只有单例才缓存句柄，
  // 临时对象每次访问都新建，句柄随渲染进程代理回收释放。
  if (object->life_type() == LifeType::Singleton) {
    it->second.handle = handle;
  }
  return object;
}

std::vector<std::string> JsRegistry::NodeNames() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return node_order_;
}

JsValue JsRegistry::Describe(int handle) {
  auto object = GetObjectByHandle(handle);
  if (!object) {
    return JsValue::Null();
  }

  // 渲染进程每次 describe 都会建一层代理，临时对象的引用计数在这里 +1，
  // 对应代理被 V8 回收时发起的 release。
  if (handle > 0 && object->life_type() == LifeType::Transient) {
    std::lock_guard<std::mutex> lock(mutex_);
    ++release_refs_[handle];
  }

  JsValue result = JsValue::Object();
  result.Set("handle", JsValue::Int(handle));

  std::vector<JsValue> members;
  for (const auto& member : object->GetTable().members()) {
    JsValue item = JsValue::Object();
    item.Set("name", JsValue::String(member.name));
    item.Set("kind", JsValue::String(member.is_property ? "property" : "function"));
    item.Set("index", JsValue::Int(member.index));
    item.Set("type", JsValue::String(TypeName(member.type)));
    item.Set("async", JsValue::Bool(member.async));
    item.Set("writable", JsValue::Bool(member.writable));

    std::vector<JsValue> params;
    for (const auto& param : member.params) {
      JsValue param_value = JsValue::Object();
      param_value.Set("name", JsValue::String(param.name));
      param_value.Set("type", JsValue::String(TypeName(param.type)));
      param_value.Set("optional", JsValue::Bool(param.optional));
      params.push_back(std::move(param_value));
    }
    item.Set("params", JsValue::Array(std::move(params)));
    members.push_back(std::move(item));
  }
  result.Set("members", JsValue::Array(std::move(members)));
  return result;
}

namespace plugins {

JsRegistry::Factory LoadDll(const std::string& path, std::string* error) {
  const std::wstring wide_path = Utf8ToWide(path);
  HMODULE module = ::LoadLibraryW(wide_path.c_str());
  if (!module) {
    if (error) {
      *error = "LoadLibrary failed for " + path +
               ", error=" + std::to_string(::GetLastError());
    }
    return nullptr;
  }

  using CreateProc = JsObjectBase* (*)();
  CreateProc create = reinterpret_cast<CreateProc>(
      ::GetProcAddress(module, "GetExtensionJsApiObj"));
  if (!create) {
    if (error) {
      *error = "GetExtensionJsApiObj not found in " + path;
    }
    return nullptr;
  }

  // 动态库常驻，不调用 FreeLibrary，避免对象析构时模块已卸载。
  return [module, create]() -> std::shared_ptr<JsObjectBase> {
    JsObjectBase* raw = create();
    return std::shared_ptr<JsObjectBase>(raw);
  };
}

}  // namespace plugins

}  // namespace jsapi
