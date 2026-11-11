#pragma once

#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "jsapi/js_value.h"

namespace jsapi {

class JsObjectBase;
class JsCallContext;

// 函数参数声明：驱动渲染进程侧的参数个数检查和浏览器进程侧的类型强转。
struct Param {
  std::string name;
  JsType type = JsType::Any;
  bool optional = false;
};

// 函数统一入口。插件只需要填写函数表，运行时由 RpcHost 按 (handle, index) 分发。
using JsFunction = std::function<void(JsObjectBase* self, JsCallContext& ctx)>;

// 属性访问器。
using JsGetter = std::function<JsValue(JsObjectBase* self)>;
using JsSetter =
    std::function<bool(JsObjectBase* self, const JsValue& value, std::string* error)>;

struct JsMember {
  std::string name;
  bool is_property = false;
  // 声明顺序即索引；渲染进程只传 index，不传名字
  int index = -1;
  // 函数返回值类型；属性则为属性值类型。
  JsType type = JsType::Null;
  std::vector<Param> params;
  bool async = false;
  bool writable = false;

  JsFunction function;
  JsGetter getter;
  JsSetter setter;
};

class JsTable {
 public:
  JsTable() = default;

  const JsMember* FindByName(const std::string& name) const;
  const JsMember* FindByIndex(int index) const;
  const std::vector<JsMember>& members() const { return members_; }

 private:
  friend class JsTableBuilder;
  std::vector<JsMember> members_;
  std::map<std::string, int> name_to_index_;
};

// 业务插件用它声明自己的 JSAPI 节点能力。
class JsTableBuilder {
 public:
  JsTableBuilder& Function(const std::string& name,
                           JsType return_type,
                           std::vector<Param> params,
                           JsFunction function,
                           bool async = false);

  JsTableBuilder& AsyncFunction(const std::string& name,
                                JsType return_type,
                                std::vector<Param> params,
                                JsFunction function);

  JsTableBuilder& Property(const std::string& name,
                           JsType type,
                           JsGetter getter,
                           JsSetter setter = nullptr);

  std::shared_ptr<const JsTable> Build();

 private:
  JsMember& AddMember(const std::string& name);
  JsTable table_;
};

}  // namespace jsapi
