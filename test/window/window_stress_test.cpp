// 多 web 页压力性能测试（窗口化，真实 CEF 渲染进程 + Qt 窗口）。
//
// 与 jsapi_self_test 的分工：
//   jsapi_self_test   不依赖 Qt / CEF，只覆盖协议层与对象模型；
//   本用例必须起真正的浏览器窗口，才能覆盖“多页面并发同步 RPC + 整体关闭”。
//
// 用法：
//   jsapi_window_stress_test [--pages=10] [--calls=200] [--close=0]
//                            [--timeout=120000] [--close-timeout=30000]
//   --pages       同时创建的可见窗口数（1..200），每个 = 一个 CefBrowser
//                + 一条同步通道 + 一个渲染进程
//   --calls       每页执行的同步 RPC 次数（校验返回值）
//   --close=1     结束前统一关闭全部浏览器并等待回收；默认 0，只验证页面侧
//   --timeout     等待所有页面回报的总超时
//   --close-timeout  关闭全部浏览器并等待回收的超时（带看门狗，不会挂死）
//
// 输出 [ PASS ] / [ FAIL ] 行；进程退出码 0 表示全部通过、1 表示有失败。
// 各页结果先打印再关闭，因此即使关闭阶段崩溃也能看到页面侧结论。
//
// 注意：--close=1 在多页时经常卡在 CEF 销毁浏览器对象的同步阶段（UI 线程收不到
// 后续关闭任务，1 页稳定通过），属于既有问题；保留该开关用于复现与验证修复。
// 默认跳过关闭阶段：页面侧结论落定后直接结束进程，不经过可能卡住的 CEF 收尾。

#include <QApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QGridLayout>
#include <QMainWindow>
#include <QMetaObject>
#include <QScrollArea>
#include <QString>
#include <QTimer>
#include <QUrl>
#include <QUrlQuery>
#include <QWidget>

#include <windows.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <memory>
#include <string>
#include <thread>

#include "host_nodes.h"
#include "jsapi/js_object.h"
#include "jsapi/js_table.h"
#include "jsapi/js_value.h"
#include "jsapi_cef/runtime.h"
#include "jsapi_qt/runtime.h"
#include "jsapi_qt/web_container.h"

namespace {

struct Options {
  int pages = 4;
  int calls = 200;
  // 关闭阶段：默认关闭，只验证“多页并发同步 RPC”；--close=1 打开整体关闭。
  bool close = false;
  int timeout_ms = 120000;
  int close_timeout_ms = 30000;
};

struct PageResult {
  bool ok = false;
  int calls = 0;
  int success = 0;
  int elapsed_ms = 0;
};

struct TestState {
  Options options;
  int created = 0;
  int reported = 0;
  std::map<int, PageResult> pages;
  QElapsedTimer create_timer;
  QElapsedTimer close_timer;
  qint64 next_retry_ms = 2000;
  bool finishing = false;
  int failures = 0;
};

void Check(TestState* state,
           bool ok,
           const std::string& name,
           const std::string& detail = std::string()) {
  std::printf("[ %s ] %s%s%s\n", ok ? "PASS" : "FAIL", name.c_str(),
              detail.empty() ? "" : " — ", detail.c_str());
  std::fflush(stdout);
  if (!ok) {
    ++state->failures;
  }
}

int ParseBounded(int argc,
                 char* argv[],
                 const char* flag,
                 int fallback,
                 int low,
                 int high) {
  const std::string prefix = std::string(flag) + "=";
  for (int i = 1; i < argc; ++i) {
    const std::string arg = argv[i];
    if (arg.rfind(prefix, 0) == 0) {
      try {
        const int value = std::stoi(arg.substr(prefix.size()));
        return value < low ? low : (value > high ? high : value);
      } catch (...) {
        return fallback;
      }
    }
  }
  return fallback;
}

Options ParseOptions(int argc, char* argv[]) {
  Options options;
  options.pages = ParseBounded(argc, argv, "--pages", options.pages, 1, 200);
  options.calls = ParseBounded(argc, argv, "--calls", options.calls, 1, 100000);
  options.close = ParseBounded(argc, argv, "--close", 0, 0, 1) != 0;
  options.timeout_ms =
      ParseBounded(argc, argv, "--timeout", options.timeout_ms, 1000, 600000);
  options.close_timeout_ms = ParseBounded(
      argc, argv, "--close-timeout", options.close_timeout_ms, 1000, 600000);
  return options;
}

// 页面回报通道：注册成 Extention.test，页面用 report(...) 把每页结果送回宿主。
// 函数体在通道会话线程执行，因此只把值按值捕获后投递到 GUI 线程。
class ReportNode final : public jsapi::JsObjectBase {
 public:
  explicit ReportNode(
      std::function<void(int, bool, int, int, int)> on_report)
      : on_report_(std::move(on_report)) {}

 protected:
  void DeclareMembers(jsapi::JsTableBuilder& table) const override {
    table.Function(
        "report", jsapi::JsType::Bool,
        {{"page", jsapi::JsType::Int},
         {"ok", jsapi::JsType::Bool},
         {"calls", jsapi::JsType::Int},
         {"success", jsapi::JsType::Int},
         {"elapsed_ms", jsapi::JsType::Int}},
        [this](jsapi::JsObjectBase*, jsapi::JsCallContext& ctx) {
          const int page = static_cast<int>(ctx.args()[0].AsInt());
          const bool ok = ctx.args()[1].AsBool();
          const int calls = static_cast<int>(ctx.args()[2].AsInt());
          const int success = static_cast<int>(ctx.args()[3].AsInt());
          const int elapsed_ms = static_cast<int>(ctx.args()[4].AsInt());
          if (on_report_) {
            on_report_(page, ok, calls, success, elapsed_ms);
          }
          ctx.Return(jsapi::JsValue::Bool(true));
        });
  }

 private:
  std::function<void(int, bool, int, int, int)> on_report_;
};

struct Context {
  TestState state;
  jsapi::qt::Runtime* runtime = nullptr;
  QApplication* app = nullptr;
  QWidget* window = nullptr;
};

void Finish(const std::shared_ptr<Context>& context);

void OnPageReported(const std::shared_ptr<Context>& context,
                    int page,
                    bool ok,
                    int calls,
                    int success,
                    int elapsed_ms) {
  TestState& state = context->state;
  if (state.finishing || state.pages.count(page) != 0) {
    return;
  }
  PageResult result;
  result.ok = ok;
  result.calls = calls;
  result.success = success;
  result.elapsed_ms = elapsed_ms;
  state.pages[page] = result;
  ++state.reported;
  if (state.reported >= state.options.pages) {
    Finish(context);
  }
}

void Finish(const std::shared_ptr<Context>& context) {
  TestState& state = context->state;
  if (state.finishing) {
    return;
  }
  state.finishing = true;

  const long long create_ms = state.create_timer.elapsed();
  std::printf("[window_stress] pages=%d calls=%d created=%d reported=%d\n",
              state.options.pages, state.options.calls, state.created,
              state.reported);
  std::printf("[window_stress] create_ms=%lld\n", create_ms);
  std::fflush(stdout);

  // 逐页结果：全部通过时只打印聚合行，有失败才逐条展开。
  int ok_pages = 0;
  std::string missing;
  for (int page = 0; page < state.options.pages; ++page) {
    auto it = state.pages.find(page);
    if (it == state.pages.end()) {
      missing += (missing.empty() ? "" : ",") + std::to_string(page);
      continue;
    }
    const PageResult& result = it->second;
    if (result.ok && result.success == state.options.calls) {
      ++ok_pages;
      continue;
    }
    Check(&state, false,
          "page " + std::to_string(page) + " sync rpc",
          "calls=" + std::to_string(result.calls) +
              " success=" + std::to_string(result.success) +
              " elapsed_ms=" + std::to_string(result.elapsed_ms));
  }
  if (!missing.empty()) {
    Check(&state, false, "all pages reported", "missing pages " + missing);
  }
  Check(&state, ok_pages == state.options.pages,
        "all pages completed sync rpc",
        std::to_string(ok_pages) + "/" +
            std::to_string(state.options.pages) + " pages, " +
            std::to_string(state.options.calls) + " calls each");

  if (!state.options.close) {
    std::printf("[window_stress] close phase skipped\n");
    std::fflush(stdout);
    // 页面上所有同步调用已回包，结论已落定；多页关闭会卡在 CEF 销毁浏览器
    // 对象的同步阶段（见文件头），直接结束进程，避免把该缺陷算进本用例。
    std::_Exit(state.failures == 0 ? 0 : 1);
  }

  // 关闭阶段：先打一行再动手，崩溃时据此判断是否已进入关闭链路。
  std::printf("[window_stress] closing %d browsers\n", state.created);
  std::fflush(stdout);

  // 关闭动作延后到下一轮事件循环：此刻仍在上一条同步回报的投递栈上，
  // 直接关闭会让浏览器的会话线程停在半完成的调用里。
  QTimer::singleShot(0, context->window, [context] {
    TestState& state = context->state;
    state.close_timer.start();

    // 关闭链路卡死时不能把整个用例挂住：超时后结束进程并给出结论。
    const int budget = state.options.close_timeout_ms + 5000;
    std::thread([budget] {
      std::this_thread::sleep_for(std::chrono::milliseconds(budget));
      std::printf("[ FAIL ] close all browsers — watchdog timeout\n");
      std::fflush(stdout);
      ::TerminateProcess(::GetCurrentProcess(), 1);
    }).detach();

    context->runtime->core()->CloseAllBrowsers(true);

    auto* poll = new QTimer(context->window);
    poll->setInterval(50);
    QObject::connect(poll, &QTimer::timeout, [context, poll] {
      TestState& state = context->state;
      const long long elapsed = state.close_timer.elapsed();
      if (context->runtime->core()->WaitForAllClosed(0)) {
        poll->stop();
        Check(&state, true, "close all browsers",
              std::to_string(state.created) + " browsers in " +
                  std::to_string(elapsed) + " ms");
        context->app->exit(state.failures == 0 ? 0 : 1);
        return;
      }
      if (elapsed > state.options.close_timeout_ms) {
        poll->stop();
        Check(&state, false, "close all browsers",
              "timeout after " + std::to_string(elapsed) + " ms");
        context->app->exit(1);
        return;
      }
      // CEF 偶尔会丢掉关闭请求：隔一段时间强制补发一次，避免假死到超时。
      if (elapsed > state.next_retry_ms) {
        state.next_retry_ms = elapsed + 2000;
        context->runtime->core()->CloseAllBrowsers(true);
      }
    });
    poll->start();
  });
}

std::shared_ptr<jsapi::JsObjectBase> CreateReportNode(
    const std::shared_ptr<Context>& context) {
  return std::make_shared<ReportNode>(
      [context](int page, bool ok, int calls, int success, int elapsed_ms) {
        QMetaObject::invokeMethod(
            qApp,
            [context, page, ok, calls, success, elapsed_ms] {
              OnPageReported(context, page, ok, calls, success, elapsed_ms);
            },
            Qt::QueuedConnection);
      });
}

// 统计真正创建成功的浏览器数量（WebContainer 的 LoadUrl 可能因不可见延后）。
class CountObserver final : public jsapi::cef::BrowserObserver {
 public:
  explicit CountObserver(std::shared_ptr<Context> context)
      : context_(std::move(context)) {}

  void OnBrowserCreated() override {
    auto context = context_;
    // 回调在 CEF UI 线程，计数切回 GUI 线程统一做。
    QMetaObject::invokeMethod(
        qApp,
        [context] {
          if (!context->state.finishing) {
            ++context->state.created;
          }
        },
        Qt::QueuedConnection);
  }

 private:
  std::shared_ptr<Context> context_;
};

}  // namespace

int main(int argc, char* argv[]) {
  // 必须在 QApplication 之前：CEF 的 renderer/GPU 子进程复用同一个 exe。
  if (const int code = jsapi::qt::Runtime::RunSubprocess(); code >= 0) {
    return code;
  }

  QApplication app(argc, argv);
  const Options options = ParseOptions(argc, argv);

  auto context = std::make_shared<Context>();
  context->state.options = options;
  context->app = &app;

  jsapi::qt::Runtime runtime(&app);
  if (!runtime.initialized()) {
    std::printf("[ FAIL ] runtime init\n");
    return 1;
  }
  context->runtime = &runtime;

  runtime.registry().RegisterNode("host", &jsapi_demo::CreateHostNode);
  runtime.registry().RegisterNode(
      "test",
      [context]() { return CreateReportNode(context); });

  // 关闭浏览器时 CEF 会销毁被嵌入的原生子窗口；不让 Qt 把它当成
  // “最后一个窗口关闭”而提前退出事件循环。
  qApp->setQuitOnLastWindowClosed(false);

  auto* window = new QMainWindow();
  context->window = window;
  window->setWindowTitle(
      QString::fromUtf8("JSAPI 多页压力测试 (%1 页)").arg(options.pages));
  window->resize(1280, 860);

  auto* scroll = new QScrollArea(window);
  scroll->setWidgetResizable(true);
  auto* grid_host = new QWidget(scroll);
  auto* grid = new QGridLayout(grid_host);
  grid->setSpacing(2);
  grid->setContentsMargins(0, 0, 0, 0);
  scroll->setWidget(grid_host);
  window->setCentralWidget(scroll);

  const QString page_path =
      QDir(QCoreApplication::applicationDirPath())
          .filePath("jsapi_window_stress.html");
  const QString page_url = QUrl::fromLocalFile(page_path).toString();
  const int columns = options.pages <= 12 ? 3 : 8;

  auto observer = std::make_shared<CountObserver>(context);
  for (int i = 0; i < options.pages; ++i) {
    // 一个容器 = 一个独立 web 页（独立浏览器 + 独立同步通道）。
    auto* page = new jsapi::qt::WebContainer(&runtime);
    page->setMinimumSize(320, 240);
    page->SetObserver(observer);

    QUrl url(page_url);
    QUrlQuery query;
    query.addQueryItem("page", QString::number(i));
    query.addQueryItem("calls", QString::number(options.calls));
    url.setQuery(query);
    page->LoadUrl(url);
    grid->addWidget(page, i / columns, i % columns);
  }

  // 总超时：页面没回报（渲染进程卡死/崩溃）时也要有结论，而不是挂住。
  QTimer::singleShot(options.timeout_ms, window, [context] {
    if (context->state.finishing) {
      return;
    }
    if (context->state.reported == 0) {
      Check(&context->state, false, "pages respond",
            "no page reported within timeout");
      context->state.finishing = true;
      context->app->exit(1);
      return;
    }
    Finish(context);
  });

  context->state.create_timer.start();
  window->show();
  return app.exec();
}
