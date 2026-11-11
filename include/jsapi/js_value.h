#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <variant>
#include <vector>

namespace jsapi {

// JSAPI 内部统一的值表示。跨进程 RPC 时序列化为 JSON：
//  - 普通数组/对象直接映射为 JSON 数组/对象
//  - 浏览器进程持有的对象句柄编码成 {"$obj": id}
//  - 渲染进程持有的回调句柄编码成 {"$cb": id}
enum class JsType : int {
  Null = 0,
  Bool,
  Int,
  Double,
  String,
  Array,
  Object,
  ObjectHandle,
  CallbackHandle,
  Any,
};

const char* TypeName(JsType type);
JsType TypeFromName(const std::string& name);

class JsValue {
 public:
  JsValue() = default;

  static JsValue Null();
  static JsValue Bool(bool value);
  static JsValue Int(std::int64_t value);
  static JsValue Double(double value);
  static JsValue String(std::string value);
  static JsValue Array(std::vector<JsValue> items = {});
  static JsValue Object(std::map<std::string, JsValue> fields = {});
  static JsValue ObjectHandle(int handle);
  static JsValue CallbackHandle(int id);

  JsType type() const;
  bool is_null() const { return type() == JsType::Null; }

  bool AsBool(bool fallback = false) const;
  std::int64_t AsInt(std::int64_t fallback = 0) const;
  double AsDouble(double fallback = 0.0) const;
  std::string AsString(const std::string& fallback = std::string()) const;
  const std::vector<JsValue>& AsArray() const;
  const std::map<std::string, JsValue>& AsObject() const;
  int AsHandle(int fallback = -1) const;

  // 便捷构造：只对 Object / Array 类型生效。
  void Set(const std::string& key, JsValue value);
  const JsValue* Find(const std::string& key) const;
  void Push(JsValue value);

  std::string ToJson() const;

 private:
  // 对象句柄与回调句柄底层都是 int，必须用不同的包装类型才能在同一 union 中区分。
  struct ObjectHandleTag {
    int id = -1;
  };
  struct CallbackHandleTag {
    int id = -1;
  };

  using ObjectFields = std::map<std::string, JsValue>;
  // 同一时刻只存在一个分支，整型不再携带 string/map/vector 等冗余成员。
  using Storage = std::variant<std::monostate, bool, std::int64_t, double,
                               std::string, std::vector<JsValue>, ObjectFields,
                               ObjectHandleTag, CallbackHandleTag>;

  Storage storage_;
};

bool ParseJson(const std::string& json, JsValue* out, std::string* error);

}  // namespace jsapi
