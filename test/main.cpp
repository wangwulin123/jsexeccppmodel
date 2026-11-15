#include <windows.h>

#include <cstdio>
#include <string>

#include "support/test_harness.h"

int main(int argc, char** argv) {
  ::SetConsoleOutputCP(CP_UTF8);

  std::string suite = "all";
  for (int i = 1; i < argc; ++i) {
    const std::string arg = argv[i];
    if (arg == "--list") {
      test::PrintList();
      return 0;
    }
    if (arg == "--help" || arg == "-h") {
      test::PrintHelp();
      return 0;
    }
    if (arg.rfind("--suite=", 0) == 0) {
      suite = arg.substr(std::string("--suite=").size());
      continue;
    }
    if (arg.rfind("--plugin=", 0) == 0) {
      test::SetPluginPath(arg.substr(std::string("--plugin=").size()));
      continue;
    }
    std::printf("unknown argument: %s\n", arg.c_str());
    test::PrintHelp();
    return 2;
  }
  return test::RunSuite(suite);
}
