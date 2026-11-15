#include <cstdint>
#include <limits>
#include <string>
#include <vector>

#include "jsapi/js_value.h"
#include "support/test_harness.h"

TEST(reliability, json_codec) {
  jsapi::JsValue value;
  std::string error;
  const char* json =
      R"({"a":1,"b":[true,null,"x"],"c":{"$obj":5},"d":{"$cb":9}})";
  const bool parsed = jsapi::ParseJson(json, &value, &error);
  test::Check(parsed, "json parse", error);
  if (parsed) {
    const jsapi::JsValue* c = value.Find("c");
    const jsapi::JsValue* d = value.Find("d");
    test::Check(c && c->type() == jsapi::JsType::ObjectHandle &&
                    c->AsHandle() == 5,
                "json object handle decode");
    test::Check(d && d->type() == jsapi::JsType::CallbackHandle &&
                    d->AsHandle() == 9,
                "json callback handle decode");

    jsapi::JsValue roundtrip;
    const bool again = jsapi::ParseJson(value.ToJson(), &roundtrip, &error);
    test::Check(again && roundtrip.Find("c")->AsHandle() == 5 &&
                    roundtrip.Find("d")->AsHandle() == 9,
                "json handle roundtrip", error);
  }
}

TEST(reliability, value_model) {
  using jsapi::JsType;
  using jsapi::JsValue;

  // 每个工厂落到唯一分支，type() 准确回读。
  test::Check(JsValue::Null().type() == JsType::Null && JsValue().is_null(),
              "variant: default/null is Null");
  test::Check(JsValue::Bool(true).type() == JsType::Bool, "variant: bool tag");
  test::Check(JsValue::Int(-7).type() == JsType::Int, "variant: int tag");
  test::Check(JsValue::Double(1.5).type() == JsType::Double,
              "variant: double tag");
  test::Check(JsValue::String("x").type() == JsType::String,
              "variant: string tag");
  test::Check(JsValue::Array().type() == JsType::Array, "variant: array tag");
  test::Check(JsValue::Object().type() == JsType::Object, "variant: object tag");
  test::Check(JsValue::ObjectHandle(3).type() == JsType::ObjectHandle,
              "variant: object handle tag");
  test::Check(JsValue::CallbackHandle(4).type() == JsType::CallbackHandle,
              "variant: callback handle tag");

  // 两种句柄底层都是 int，必须仍能区分类型且都能取到 id。
  const JsValue object_handle = JsValue::ObjectHandle(11);
  const JsValue callback_handle = JsValue::CallbackHandle(11);
  test::Check(object_handle.AsHandle() == 11 && callback_handle.AsHandle() == 11,
              "variant: both handle kinds expose id");
  test::Check(object_handle.ToJson() == "{\"$obj\":11}" &&
                  callback_handle.ToJson() == "{\"$cb\":11}",
              "variant: handle json encoding stays distinct",
              object_handle.ToJson() + " / " + callback_handle.ToJson());
  test::Check(JsValue::Int(11).AsHandle(-1) == -1,
              "variant: non-handle AsHandle falls back");

  // 数值互转语义与原实现一致。
  test::Check(JsValue::Bool(true).AsInt() == 1 &&
                  JsValue::Bool(false).AsInt() == 0,
              "variant: bool -> int");
  test::Check(JsValue::Int(5).AsBool() && !JsValue::Int(0).AsBool(),
              "variant: int -> bool");
  test::Check(JsValue::Int(5).AsDouble() == 5.0, "variant: int -> double");
  test::Check(JsValue::Double(2.9).AsInt() == 2,
              "variant: double -> int truncates");
  test::Check(JsValue::Double(0.5).AsBool(), "variant: double -> bool");
  test::Check(JsValue::String("42").AsInt(-1) == -1 &&
                  !JsValue::String("1").AsBool(false) &&
                  JsValue::String("1").AsDouble(-1.0) == -1.0,
              "variant: string is not coerced to number");

  // 类型不符时容器访问返回静态空容器，不产生悬空引用。
  test::Check(JsValue::Int(1).AsArray().empty() &&
                  JsValue::Int(1).AsObject().empty(),
              "variant: container accessors fall back to empty");
  test::Check(JsValue::Int(1).AsString("d") == "d",
              "variant: string accessor falls back");
  test::Check(JsValue::Int(1).Find("k") == nullptr,
              "variant: Find on non-object is null");

  // Set / Push 的隐式类型切换与清空行为不变。
  JsValue obj;
  obj.Set("a", JsValue::Int(1));
  obj.Set("b", JsValue::Int(2));
  test::Check(obj.type() == JsType::Object && obj.AsObject().size() == 2,
              "variant: Set promotes Null to object");
  obj.Set("a", JsValue::Int(9));
  test::Check(obj.AsObject().size() == 2 && obj.Find("a")->AsInt() == 9,
              "variant: Set overwrites existing key");

  JsValue mixed = JsValue::Object();
  mixed.Set("k", JsValue::Int(1));
  mixed.Push(JsValue::Int(2));
  test::Check(mixed.type() == JsType::Array && mixed.AsArray().size() == 1 &&
                  mixed.AsObject().empty(),
              "variant: Push replaces object with cleared array");

  JsValue array;
  array.Push(JsValue::String("a"));
  array.Push(JsValue::String("b"));
  test::Check(array.type() == JsType::Array && array.AsArray().size() == 2,
              "variant: Push appends");
  array.Set("k", JsValue::Bool(true));
  test::Check(array.type() == JsType::Object && array.AsObject().size() == 1 &&
                  array.AsArray().empty(),
              "variant: Set replaces array with cleared object");

  // 值语义：拷贝后互不影响（variant 深拷贝，不是共享存储）。
  JsValue original = JsValue::Object();
  original.Set("n", JsValue::Int(1));
  JsValue copied = original;
  copied.Set("n", JsValue::Int(2));
  copied.Set("extra", JsValue::Bool(true));
  test::Check(original.AsObject().size() == 1 &&
                  original.Find("n")->AsInt() == 1,
              "variant: object copy is deep");

  JsValue array_copy = JsValue::Array();
  array_copy.Push(JsValue::Int(1));
  JsValue array_copy2 = array_copy;
  array_copy2.Push(JsValue::Int(2));
  test::Check(array_copy.AsArray().size() == 1 &&
                  array_copy2.AsArray().size() == 2,
              "variant: array copy is deep");

  // JSON 往返覆盖全部分支（含嵌套、转义、int64 边界与 -0.0）。
  JsValue all = JsValue::Array();
  all.Push(JsValue::Null());
  all.Push(JsValue::Bool(false));
  all.Push(JsValue::Int(std::numeric_limits<std::int64_t>::max()));
  all.Push(JsValue::Double(-0.0));
  all.Push(JsValue::String("a\"b\\c\n\tz"));
  all.Push(JsValue::ObjectHandle(7));
  all.Push(JsValue::CallbackHandle(8));
  JsValue nested = JsValue::Object();
  nested.Set("arr", JsValue::Array());
  nested.Set("obj", JsValue::Object());
  all.Push(nested);

  std::string error;
  JsValue decoded;
  const bool roundtrip = jsapi::ParseJson(all.ToJson(), &decoded, &error);
  test::Check(roundtrip, "variant: all-branch json parse", error);
  if (roundtrip) {
    const std::vector<JsValue>& items = decoded.AsArray();
    test::Check(items.size() == 8, "variant: all-branch json keeps arity");
    test::Check(items.size() == 8 && items[0].type() == JsType::Null &&
                    items[1].type() == JsType::Bool &&
                    items[2].type() == JsType::Int &&
                    items[2].AsInt() ==
                        std::numeric_limits<std::int64_t>::max() &&
                    items[3].type() == JsType::Double &&
                    items[4].AsString() == "a\"b\\c\n\tz" &&
                    items[5].type() == JsType::ObjectHandle &&
                    items[6].type() == JsType::CallbackHandle &&
                    items[7].Find("arr") != nullptr &&
                    items[7].Find("arr")->type() == JsType::Array &&
                    items[7].Find("obj") != nullptr &&
                    items[7].Find("obj")->type() == JsType::Object,
                "variant: all-branch json preserves types");
  }

  // 体积约束：整型不再携带 string/map/vector 等冗余成员。
  test::Check(sizeof(JsValue) <= 48, "variant: JsValue stays compact",
              std::to_string(sizeof(JsValue)) + " bytes");
}

TEST(reliability, json_edge_cases) {
  using jsapi::JsType;
  using jsapi::JsValue;

  std::string error;
  JsValue value;

  // JSON 没有 NaN/Infinity：序列化为 0，解析字面量则失败。
  test::Check(
      JsValue::Double(std::numeric_limits<double>::quiet_NaN()).ToJson() == "0",
      "json: NaN serializes as 0");
  test::Check(
      JsValue::Double(std::numeric_limits<double>::infinity()).ToJson() == "0",
      "json: Infinity serializes as 0");
  test::Check(
      JsValue::Double(-std::numeric_limits<double>::infinity()).ToJson() == "0",
      "json: -Infinity serializes as 0");
  error.clear();
  test::Check(!jsapi::ParseJson("NaN", &value, &error) && !error.empty(),
              "json: NaN literal is rejected", error);
  error.clear();
  test::Check(!jsapi::ParseJson("Infinity", &value, &error) && !error.empty(),
              "json: Infinity literal is rejected", error);

  // INT64 极值往返必须逐位精确。
  test::Check(JsValue::Int(std::numeric_limits<std::int64_t>::min()).ToJson() ==
                  "-9223372036854775808",
              "json: INT64_MIN encoding is exact");
  error.clear();
  const bool min_ok = jsapi::ParseJson("-9223372036854775808", &value, &error);
  test::Check(min_ok && value.type() == JsType::Int &&
                  value.AsInt() == std::numeric_limits<std::int64_t>::min(),
              "json: INT64_MIN roundtrip", error);
  error.clear();
  const bool max_ok = jsapi::ParseJson("9223372036854775807", &value, &error);
  test::Check(max_ok && value.type() == JsType::Int &&
                  value.AsInt() == std::numeric_limits<std::int64_t>::max(),
              "json: INT64_MAX roundtrip", error);

  // 浮点边界：1e308 / 1e-308 编解码保持同一个 double。
  error.clear();
  const bool big_ok =
      jsapi::ParseJson(JsValue::Double(1e308).ToJson(), &value, &error);
  test::Check(big_ok && value.type() == JsType::Double &&
                  value.AsDouble() == 1e308,
              "json: 1e308 roundtrip", error);
  error.clear();
  const bool small_ok =
      jsapi::ParseJson(JsValue::Double(1e-308).ToJson(), &value, &error);
  test::Check(small_ok && value.type() == JsType::Double &&
                  value.AsDouble() == 1e-308,
              "json: 1e-308 roundtrip", error);

  // 超深嵌套必须显式失败，而不是把测试进程的栈打爆。
  std::string deep;
  deep.reserve(1002);
  for (int i = 0; i < 500; ++i) deep.push_back('[');
  for (int i = 0; i < 500; ++i) deep.push_back(']');
  error.clear();
  test::Check(!jsapi::ParseJson(deep, &value, &error) && !error.empty(),
              "json: 500-level nesting fails instead of overflowing the stack",
              error);

  // 非法 JSON：尾随垃圾、未闭合对象、单引号、空字符串。
  error.clear();
  test::Check(!jsapi::ParseJson("{} garbage", &value, &error) && !error.empty(),
              "json: trailing garbage is rejected", error);
  error.clear();
  test::Check(!jsapi::ParseJson("{\"a\":1", &value, &error) && !error.empty(),
              "json: unterminated object is rejected", error);
  error.clear();
  test::Check(!jsapi::ParseJson("{'a':1}", &value, &error) && !error.empty(),
              "json: single quotes are rejected", error);
  error.clear();
  test::Check(!jsapi::ParseJson("", &value, &error) && !error.empty(),
              "json: empty input is rejected", error);

  // 重复 key：保留最后出现的值。
  error.clear();
  const bool dup_ok = jsapi::ParseJson(R"({"a":1,"a":2})", &value, &error);
  test::Check(dup_ok && value.Find("a") && value.Find("a")->AsInt() == 2,
              "json: duplicate key keeps the last value", error);

  // \uXXXX（BMP）解码为 UTF-8。
  const std::string chinese = "\xE4\xB8\xAD";
  error.clear();
  const bool bmp_ok = jsapi::ParseJson(R"("\u4e2d")", &value, &error);
  test::Check(bmp_ok && value.AsString() == chinese,
              "json: \\uXXXX decodes BMP characters", error);

  // 代理对：当前实现按 BMP 逐个编码（不合并为 UCS-4），至少不能崩溃/截断。
  error.clear();
  const bool pair_ok = jsapi::ParseJson(R"("\uD83D\uDE00")", &value, &error);
  test::Check(pair_ok && value.AsString().size() == 6,
              "json: surrogate pair decodes as two BMP code units",
              error.empty() ? std::to_string(value.AsString().size()) : error);

  // 字符串转义：引号 / 反斜杠 / 换行 / 控制字符往返。
  const std::string escaped = std::string("a") + static_cast<char>(0x01) + "b";
  const JsValue raw = JsValue::String(escaped);
  test::Check(raw.ToJson() == "\"a\\u0001b\"",
              "json: control characters are escaped", raw.ToJson());
  error.clear();
  const bool escape_ok = jsapi::ParseJson(raw.ToJson(), &value, &error);
  test::Check(escape_ok && value.AsString() == escaped,
              "json: escapable characters roundtrip", error);

  test::Check(JsValue::String("a\"b\\c\nd").ToJson() ==
                  "\"a\\\"b\\\\c\\nd\"",
              "json: quote/backslash/newline encoding");
  error.clear();
  const bool mix_ok = jsapi::ParseJson(
      JsValue::String("a\"b\\c\nd").ToJson(), &value, &error);
  test::Check(mix_ok && value.AsString() == "a\"b\\c\nd",
              "json: quote/backslash/newline roundtrip", error);

  // UTF-8 中文往返。
  const std::string chinese_text = "\xE4\xB8\xAD\xE6\x96\x87";
  error.clear();
  const bool utf8_ok = jsapi::ParseJson(JsValue::String(chinese_text).ToJson(),
                                        &value, &error);
  test::Check(utf8_ok && value.AsString() == chinese_text,
              "json: UTF-8 Chinese roundtrip", error);

  // 内嵌 \u0000 不能被 std::string 截断。
  error.clear();
  const bool nul_ok = jsapi::ParseJson(R"("a\u0000b")", &value, &error);
  test::Check(nul_ok && value.AsString().size() == 3 &&
                  value.AsString()[0] == 'a' && value.AsString()[1] == '\0' &&
                  value.AsString()[2] == 'b',
              "json: embedded \\u0000 is not truncated", error);
  error.clear();
  const bool nul_roundtrip =
      jsapi::ParseJson(value.ToJson(), &value, &error);
  test::Check(nul_roundtrip && value.AsString().size() == 3,
              "json: embedded \\u0000 roundtrip", error);
}
