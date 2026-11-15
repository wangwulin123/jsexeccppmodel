#include "support/test_harness.h"

#include <chrono>
#include <cstdio>
#include <map>
#include <string>
#include <vector>

#include "support/test_helpers.h"

namespace test {
namespace {

struct TestCase {
  std::string suite;
  std::string name;
  TestBody body;
};

int g_checks = 0;
int g_failures = 0;

std::vector<TestCase>& Registry() {
  static std::vector<TestCase> tests;
  return tests;
}

std::string& PluginPathStorage() {
  static std::string path;
  return path;
}

double ElapsedMs(std::chrono::steady_clock::time_point begin) {
  return std::chrono::duration<double, std::milli>(
             std::chrono::steady_clock::now() - begin)
      .count();
}

struct SuiteStats {
  int cases = 0;
  int checks = 0;
  double ms = 0.0;
};

}  // namespace

void Check(bool ok, const std::string& name, const std::string& detail) {
  ++g_checks;
  if (ok) {
    std::printf("[PASS] %s\n", name.c_str());
  } else {
    ++g_failures;
    std::printf("[FAIL] %s%s%s\n", name.c_str(), detail.empty() ? "" : " -> ",
                detail.c_str());
  }
}

void CheckKnownIssue(bool ok,
                     const std::string& name,
                     const std::string& detail) {
  ++g_checks;
  if (ok) {
    std::printf("[PASS] %s\n", name.c_str());
    return;
  }
  std::printf(
      "[WARN] %s%s%s (known issue, not counted as failure)\n", name.c_str(),
      detail.empty() ? "" : " -> ", detail.c_str());
}

void RegisterTest(const char* suite, const char* name, TestBody body) {
  Registry().push_back(
      TestCase{suite ? suite : "", name ? name : "", body});
}

int RunSuite(const std::string& suite) {
  if (suite != "all" && suite != "reliability" && suite != "system" &&
      suite != "perf") {
    std::printf("unknown suite: %s\n", suite.c_str());
    PrintHelp();
    return 2;
  }

  g_checks = 0;
  g_failures = 0;

  std::printf("jsapi self test\n");
  std::printf("==============\n");

  const auto total_begin = std::chrono::steady_clock::now();
  std::map<std::string, SuiteStats> stats;
  for (const auto& test : Registry()) {
    if (suite != "all" && test.suite != suite) {
      continue;
    }
    std::printf("-- %s / %s --\n", test.suite.c_str(), test.name.c_str());
    const auto case_begin = std::chrono::steady_clock::now();
    const int before = g_checks;
    test.body();
    SuiteStats& item = stats[test.suite];
    ++item.cases;
    item.checks += g_checks - before;
    item.ms += ElapsedMs(case_begin);
  }

  const double total_ms = ElapsedMs(total_begin);
  std::printf("==============\n");
  for (const char* name : {"reliability", "system", "perf"}) {
    const auto it = stats.find(name);
    if (it == stats.end()) {
      continue;
    }
    std::printf("%s: %d cases, %d checks, %.1f ms\n", name, it->second.cases,
                it->second.checks, it->second.ms);
  }
  std::printf("total: %.1f ms\n", total_ms);
  std::printf("passed %d / %d checks\n", g_checks - g_failures, g_checks);
  return g_failures == 0 ? 0 : 1;
}

void PrintList() {
  std::printf("jsapi self test suites\n");
  std::printf("======================\n");
  for (const char* name : {"reliability", "system", "perf"}) {
    int count = 0;
    for (const auto& test : Registry()) {
      if (test.suite == name) {
        ++count;
      }
    }
    std::printf("%-12s %d cases\n", name, count);
  }
}

void PrintHelp() {
  std::printf(
      "usage: jsapi_self_test [options]\n"
      "  --suite=all|reliability|system|perf   select a suite (default: all)\n"
      "  --list                               list suites and case counts\n"
      "  --help                               show this help\n"
      "  --plugin=<path>                      plugin dll path "
      "(default: <exe dir>\\demo_plugin.dll)\n");
}

void SetPluginPath(std::string path) {
  PluginPathStorage() = std::move(path);
}

const std::string& PluginPath() {
  std::string& path = PluginPathStorage();
  if (path.empty()) {
    path = GetExecutableDir() + "\\demo_plugin.dll";
  }
  return path;
}

}  // namespace test
