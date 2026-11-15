#pragma once

#include <string>

namespace test {

// 记录一条检查结果。失败会计入失败计数并影响进程退出码。
void Check(bool ok, const std::string& name, const std::string& detail = "");

// 已知缺陷（src/ 尚未修复）：失败时打印醒目 WARN，但不计入失败计数，
// 以免既有缺陷让整个套件无法作为回归基线。
void CheckKnownIssue(bool ok,
                     const std::string& name,
                     const std::string& detail = "");

using TestBody = void (*)();

// 由 TEST 宏在静态初始化阶段登记。
void RegisterTest(const char* suite, const char* name, TestBody body);

struct Registrar {
  Registrar(const char* suite, const char* name, TestBody body) {
    RegisterTest(suite, name, body);
  }
};

// suite 取 "all" 或 reliability / system / perf。
// 返回 0 表示全部通过，1 表示有失败，2 表示参数错误。
int RunSuite(const std::string& suite);

// --list：按套件列出用例数。
void PrintList();
void PrintHelp();

// 插件路径：默认为 exe 同目录的 demo_plugin.dll，可用 --plugin=... 覆盖。
void SetPluginPath(std::string path);
const std::string& PluginPath();

}  // namespace test

// 用例注册：TEST(reliability, json_codec) { ... }
#define TEST(suite, name)                                             \
  static void suite##_##name##_body();                                \
  static ::test::Registrar suite##_##name##_registrar(                \
      #suite, #name, &suite##_##name##_body);                         \
  static void suite##_##name##_body()
