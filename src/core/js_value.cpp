#include "jsapi/js_value.h"

#include <cerrno>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <type_traits>

namespace jsapi {
namespace {

const std::vector<JsValue> kEmptyArray;
const std::map<std::string, JsValue> kEmptyObject;

void AppendEscapedString(std::string* out, const std::string& value) {
  out->push_back('"');
  for (unsigned char c : value) {
    switch (c) {
      case '"':
        out->append("\\\"");
        break;
      case '\\':
        out->append("\\\\");
        break;
      case '\b':
        out->append("\\b");
        break;
      case '\f':
        out->append("\\f");
        break;
      case '\n':
        out->append("\\n");
        break;
      case '\r':
        out->append("\\r");
        break;
      case '\t':
        out->append("\\t");
        break;
      default:
        if (c < 0x20) {
          char buf[7] = {};
          std::snprintf(buf, sizeof(buf), "\\u%04x", c);
          out->append(buf);
        } else {
          // 其余字节（含 UTF-8 多字节）原样输出，JSON 本身允许 UTF-8。
          out->push_back(static_cast<char>(c));
        }
        break;
    }
  }
  out->push_back('"');
}

void AppendNumber(std::string* out, double value) {
  if (!std::isfinite(value)) {
    out->append("0");
    return;
  }
  char buf[64] = {};
  std::snprintf(buf, sizeof(buf), "%.17g", value);
  if (std::strchr(buf, '.') == nullptr && std::strchr(buf, 'e') == nullptr &&
      std::strchr(buf, 'E') == nullptr) {
    // 保持 double 的 JSON 形态，避免读回时被当成整型（数值语义不变）。
    std::strcat(buf, ".0");
  }
  out->append(buf);
}

struct JsonParser {
  const std::string& text;
  std::size_t pos = 0;
  std::string error;

  explicit JsonParser(const std::string& value) : text(value) {}

  void SkipSpaces() {
    while (pos < text.size()) {
      char c = text[pos];
      if (c == ' ' || c == '\t' || c == '\r' || c == '\n') {
        ++pos;
      } else {
        break;
      }
    }
  }

  bool Fail(const std::string& message) {
    if (error.empty()) {
      error = message + " (offset " + std::to_string(pos) + ")";
    }
    return false;
  }

  bool ParseString(std::string* out) {
    if (pos >= text.size() || text[pos] != '"') {
      return Fail("expected string");
    }
    ++pos;
    out->clear();
    while (pos < text.size()) {
      char c = text[pos++];
      if (c == '"') {
        return true;
      }
      if (c != '\\') {
        out->push_back(c);
        continue;
      }
      if (pos >= text.size()) {
        return Fail("unterminated escape");
      }
      char esc = text[pos++];
      switch (esc) {
        case '"':
          out->push_back('"');
          break;
        case '\\':
          out->push_back('\\');
          break;
        case '/':
          out->push_back('/');
          break;
        case 'b':
          out->push_back('\b');
          break;
        case 'f':
          out->push_back('\f');
          break;
        case 'n':
          out->push_back('\n');
          break;
        case 'r':
          out->push_back('\r');
          break;
        case 't':
          out->push_back('\t');
          break;
        case 'u': {
          if (pos + 4 > text.size()) {
            return Fail("bad unicode escape");
          }
          unsigned int code = 0;
          for (int i = 0; i < 4; ++i) {
            char h = text[pos++];
            code <<= 4;
            if (h >= '0' && h <= '9') {
              code += static_cast<unsigned int>(h - '0');
            } else if (h >= 'a' && h <= 'f') {
              code += static_cast<unsigned int>(h - 'a' + 10);
            } else if (h >= 'A' && h <= 'F') {
              code += static_cast<unsigned int>(h - 'A' + 10);
            } else {
              return Fail("bad unicode escape");
            }
          }
          // 仅处理 BMP，够本示例使用；代理对暂不合并。
          if (code < 0x80) {
            out->push_back(static_cast<char>(code));
          } else if (code < 0x800) {
            out->push_back(static_cast<char>(0xC0 | (code >> 6)));
            out->push_back(static_cast<char>(0x80 | (code & 0x3F)));
          } else {
            out->push_back(static_cast<char>(0xE0 | (code >> 12)));
            out->push_back(static_cast<char>(0x80 | ((code >> 6) & 0x3F)));
            out->push_back(static_cast<char>(0x80 | (code & 0x3F)));
          }
          break;
        }
        default:
          return Fail("unknown escape");
      }
    }
    return Fail("unterminated string");
  }

  bool ParseNumber(JsValue* out) {
    const std::size_t start = pos;
    if (pos < text.size() && (text[pos] == '-' || text[pos] == '+')) {
      ++pos;
    }
    bool is_double = false;
    while (pos < text.size()) {
      char c = text[pos];
      if (c >= '0' && c <= '9') {
        ++pos;
      } else if (c == '.' || c == 'e' || c == 'E' || c == '+' || c == '-') {
        is_double = true;
        ++pos;
      } else {
        break;
      }
    }
    if (start == pos) {
      return Fail("expected number");
    }
    const std::string token = text.substr(start, pos - start);
    if (is_double) {
      errno = 0;
      char* end = nullptr;
      const double value = std::strtod(token.c_str(), &end);
      if (end == nullptr || *end != '\0') {
        return Fail("bad number");
      }
      *out = JsValue::Double(value);
      return true;
    }
    errno = 0;
    char* end = nullptr;
    const long long value = std::strtoll(token.c_str(), &end, 10);
    if (errno == ERANGE) {
      *out = JsValue::Double(std::strtod(token.c_str(), nullptr));
      return true;
    }
    if (end == nullptr || *end != '\0') {
      return Fail("bad integer");
    }
    *out = JsValue::Int(static_cast<std::int64_t>(value));
    return true;
  }

  bool ParseValue(JsValue* out, int depth) {
    if (depth > 80) {
      return Fail("json nested too deep");
    }
    SkipSpaces();
    if (pos >= text.size()) {
      return Fail("unexpected end of json");
    }
    const char c = text[pos];
    if (c == 'n') {
      if (text.compare(pos, 4, "null") != 0) {
        return Fail("bad literal");
      }
      pos += 4;
      *out = JsValue::Null();
      return true;
    }
    if (c == 't') {
      if (text.compare(pos, 4, "true") != 0) {
        return Fail("bad literal");
      }
      pos += 4;
      *out = JsValue::Bool(true);
      return true;
    }
    if (c == 'f') {
      if (text.compare(pos, 5, "false") != 0) {
        return Fail("bad literal");
      }
      pos += 5;
      *out = JsValue::Bool(false);
      return true;
    }
    if (c == '"') {
      std::string value;
      if (!ParseString(&value)) {
        return false;
      }
      *out = JsValue::String(std::move(value));
      return true;
    }
    if (c == '[') {
      ++pos;
      std::vector<JsValue> items;
      SkipSpaces();
      if (pos < text.size() && text[pos] == ']') {
        ++pos;
        *out = JsValue::Array(std::move(items));
        return true;
      }
      while (true) {
        JsValue item;
        if (!ParseValue(&item, depth + 1)) {
          return false;
        }
        items.push_back(std::move(item));
        SkipSpaces();
        if (pos < text.size() && text[pos] == ',') {
          ++pos;
          continue;
        }
        if (pos < text.size() && text[pos] == ']') {
          ++pos;
          *out = JsValue::Array(std::move(items));
          return true;
        }
        return Fail("expected ',' or ']'");
      }
    }
    if (c == '{') {
      ++pos;
      std::map<std::string, JsValue> fields;
      SkipSpaces();
      if (pos < text.size() && text[pos] == '}') {
        ++pos;
        *out = JsValue::Object(std::move(fields));
        return true;
      }
      while (true) {
        SkipSpaces();
        std::string key;
        if (!ParseString(&key)) {
          return false;
        }
        SkipSpaces();
        if (pos >= text.size() || text[pos] != ':') {
          return Fail("expected ':'");
        }
        ++pos;
        JsValue value;
        if (!ParseValue(&value, depth + 1)) {
          return false;
        }
        fields[key] = std::move(value);
        SkipSpaces();
        if (pos < text.size() && text[pos] == ',') {
          ++pos;
          continue;
        }
        if (pos < text.size() && text[pos] == '}') {
          ++pos;
          // 句柄编码约定：{"$obj": id} / {"$cb": id}
          if (fields.size() == 1) {
            auto it = fields.begin();
            if (it->first == "$obj" && it->second.type() == JsType::Int) {
              *out = JsValue::ObjectHandle(static_cast<int>(it->second.AsInt()));
              return true;
            }
            if (it->first == "$cb" && it->second.type() == JsType::Int) {
              *out = JsValue::CallbackHandle(static_cast<int>(it->second.AsInt()));
              return true;
            }
          }
          *out = JsValue::Object(std::move(fields));
          return true;
        }
        return Fail("expected ',' or '}'");
      }
    }
    return ParseNumber(out);
  }
};

}  // namespace

const char* TypeName(JsType type) {
  switch (type) {
    case JsType::Bool:
      return "bool";
    case JsType::Int:
      return "int";
    case JsType::Double:
      return "double";
    case JsType::String:
      return "string";
    case JsType::Array:
      return "array";
    case JsType::Object:
      return "object";
    case JsType::ObjectHandle:
      return "objhandle";
    case JsType::CallbackHandle:
      return "cbhandle";
    case JsType::Any:
      return "any";
    case JsType::Null:
    default:
      return "null";
  }
}

JsType TypeFromName(const std::string& name) {
  if (name == "bool") return JsType::Bool;
  if (name == "int") return JsType::Int;
  if (name == "double") return JsType::Double;
  if (name == "string") return JsType::String;
  if (name == "array") return JsType::Array;
  if (name == "object") return JsType::Object;
  if (name == "objhandle") return JsType::ObjectHandle;
  if (name == "cbhandle") return JsType::CallbackHandle;
  if (name == "any") return JsType::Any;
  return JsType::Null;
}

JsType JsValue::type() const {
  switch (storage_.index()) {
    case 1:
      return JsType::Bool;
    case 2:
      return JsType::Int;
    case 3:
      return JsType::Double;
    case 4:
      return JsType::String;
    case 5:
      return JsType::Array;
    case 6:
      return JsType::Object;
    case 7:
      return JsType::ObjectHandle;
    case 8:
      return JsType::CallbackHandle;
    case 0:
    default:
      // 0 是 monostate；index() == variant_npos（仅在 OOM 后出现）也按 Null 处理。
      return JsType::Null;
  }
}

JsValue JsValue::Null() {
  return JsValue();
}

JsValue JsValue::Bool(bool value) {
  JsValue out;
  out.storage_ = value;
  return out;
}

JsValue JsValue::Int(std::int64_t value) {
  JsValue out;
  out.storage_ = value;
  return out;
}

JsValue JsValue::Double(double value) {
  JsValue out;
  out.storage_ = value;
  return out;
}

JsValue JsValue::String(std::string value) {
  JsValue out;
  out.storage_ = std::move(value);
  return out;
}

JsValue JsValue::Array(std::vector<JsValue> items) {
  JsValue out;
  out.storage_ = std::move(items);
  return out;
}

JsValue JsValue::Object(std::map<std::string, JsValue> fields) {
  JsValue out;
  out.storage_ = std::move(fields);
  return out;
}

JsValue JsValue::ObjectHandle(int handle) {
  JsValue out;
  out.storage_ = ObjectHandleTag{handle};
  return out;
}

JsValue JsValue::CallbackHandle(int id) {
  JsValue out;
  out.storage_ = CallbackHandleTag{id};
  return out;
}

bool JsValue::AsBool(bool fallback) const {
  if (const auto* value = std::get_if<bool>(&storage_)) {
    return *value;
  }
  if (const auto* value = std::get_if<std::int64_t>(&storage_)) {
    return *value != 0;
  }
  if (const auto* value = std::get_if<double>(&storage_)) {
    return *value != 0.0;
  }
  return fallback;
}

std::int64_t JsValue::AsInt(std::int64_t fallback) const {
  if (const auto* value = std::get_if<std::int64_t>(&storage_)) {
    return *value;
  }
  if (const auto* value = std::get_if<double>(&storage_)) {
    return static_cast<std::int64_t>(*value);
  }
  if (const auto* value = std::get_if<bool>(&storage_)) {
    return *value ? 1 : 0;
  }
  return fallback;
}

double JsValue::AsDouble(double fallback) const {
  if (const auto* value = std::get_if<double>(&storage_)) {
    return *value;
  }
  if (const auto* value = std::get_if<std::int64_t>(&storage_)) {
    return static_cast<double>(*value);
  }
  return fallback;
}

std::string JsValue::AsString(const std::string& fallback) const {
  if (const auto* value = std::get_if<std::string>(&storage_)) {
    return *value;
  }
  return fallback;
}

const std::vector<JsValue>& JsValue::AsArray() const {
  if (const auto* value = std::get_if<std::vector<JsValue>>(&storage_)) {
    return *value;
  }
  return kEmptyArray;
}

const std::map<std::string, JsValue>& JsValue::AsObject() const {
  if (const auto* value = std::get_if<ObjectFields>(&storage_)) {
    return *value;
  }
  return kEmptyObject;
}

int JsValue::AsHandle(int fallback) const {
  if (const auto* value = std::get_if<ObjectHandleTag>(&storage_)) {
    return value->id;
  }
  if (const auto* value = std::get_if<CallbackHandleTag>(&storage_)) {
    return value->id;
  }
  return fallback;
}

void JsValue::Set(const std::string& key, JsValue value) {
  ObjectFields* fields = std::get_if<ObjectFields>(&storage_);
  if (!fields) {
    storage_ = ObjectFields{};
    fields = std::get_if<ObjectFields>(&storage_);
  }
  (*fields)[key] = std::move(value);
}

const JsValue* JsValue::Find(const std::string& key) const {
  const ObjectFields* fields = std::get_if<ObjectFields>(&storage_);
  if (!fields) {
    return nullptr;
  }
  auto it = fields->find(key);
  return it == fields->end() ? nullptr : &it->second;
}

void JsValue::Push(JsValue value) {
  auto* items = std::get_if<std::vector<JsValue>>(&storage_);
  if (!items) {
    storage_ = std::vector<JsValue>{};
    items = std::get_if<std::vector<JsValue>>(&storage_);
  }
  items->push_back(std::move(value));
}

std::string JsValue::ToJson() const {
  if (storage_.valueless_by_exception()) {
    return "null";
  }
  std::string out;
  std::visit(
      [&out](const auto& value) {
        using T = std::decay_t<decltype(value)>;
        if constexpr (std::is_same_v<T, std::monostate>) {
          out = "null";
        } else if constexpr (std::is_same_v<T, bool>) {
          out = value ? "true" : "false";
        } else if constexpr (std::is_same_v<T, std::int64_t>) {
          out = std::to_string(value);
        } else if constexpr (std::is_same_v<T, double>) {
          AppendNumber(&out, value);
        } else if constexpr (std::is_same_v<T, std::string>) {
          AppendEscapedString(&out, value);
        } else if constexpr (std::is_same_v<T, std::vector<JsValue>>) {
          out.push_back('[');
          for (std::size_t i = 0; i < value.size(); ++i) {
            if (i != 0) out.push_back(',');
            out += value[i].ToJson();
          }
          out.push_back(']');
        } else if constexpr (std::is_same_v<T, ObjectFields>) {
          out.push_back('{');
          bool first = true;
          for (const auto& item : value) {
            if (!first) out.push_back(',');
            first = false;
            AppendEscapedString(&out, item.first);
            out.push_back(':');
            out += item.second.ToJson();
          }
          out.push_back('}');
        } else if constexpr (std::is_same_v<T, ObjectHandleTag>) {
          out = "{\"$obj\":" + std::to_string(value.id) + "}";
        } else if constexpr (std::is_same_v<T, CallbackHandleTag>) {
          out = "{\"$cb\":" + std::to_string(value.id) + "}";
        }
      },
      storage_);
  return out;
}

bool ParseJson(const std::string& json, JsValue* out, std::string* error) {
  JsonParser parser(json);
  JsValue value;
  if (!parser.ParseValue(&value, 0)) {
    if (error) {
      *error = parser.error.empty() ? "invalid json" : parser.error;
    }
    return false;
  }
  parser.SkipSpaces();
  if (parser.pos != json.size()) {
    if (error) {
      *error = "trailing characters after json (offset " +
               std::to_string(parser.pos) + ")";
    }
    return false;
  }
  *out = std::move(value);
  return true;
}

}  // namespace jsapi
