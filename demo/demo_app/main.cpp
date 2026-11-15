// 业务层示例：应用层只使用 jsapi / jsapi_qt 的公共头，不接触任何 CEF 类型。
//
// 本文件里剩下的全部内容就是业务决策：
//   1. 加载哪些插件、注册哪些节点；
//   2. 开几个 web 容器、加载什么地址；
//   3. 如何摆放到主窗口。

#include <QApplication>
#include <QDebug>
#include <QDir>
#include <QMainWindow>
#include <QString>
#include <QTabWidget>
#include <QUrl>

#include "host_nodes.h"
#include "jsapi/jsapi_version.h"
#include "jsapi_qt/runtime.h"
#include "jsapi_qt/web_container.h"

namespace {

struct StartupArgs {
  QString url;
  int pages = 4;
};

// --url=<地址> 覆盖默认示例页；--pages=N 打开 N 个独立 web 容器（1..16）。
StartupArgs ParseStartupArgs(int argc, char* argv[]) {
  StartupArgs args;
  for (int i = 1; i < argc; ++i) {
    const QString arg = QString::fromLocal8Bit(argv[i]);
    if (arg.startsWith("--url=")) {
      args.url = arg.mid(6);
    } else if (arg.startsWith("--pages=")) {
      bool ok = false;
      const int pages = arg.mid(8).toInt(&ok);
      if (ok) {
        args.pages = qBound(1, pages, 16);
      }
    }
  }
  if (args.url.isEmpty()) {
    args.url = QUrl::fromLocalFile(
                   QDir(QCoreApplication::applicationDirPath())
                       .filePath("jsapi_demo.html"))
                   .toString();
  }
  return args;
}

}  // namespace

int main(int argc, char* argv[]) {
  // 必须在 QApplication 之前：CEF 的 renderer/GPU 子进程复用同一个 exe。
  if (const int code = jsapi::qt::Runtime::RunSubprocess(); code >= 0) {
    return code;
  }

  QApplication app(argc, argv);

  jsapi::qt::Runtime runtime(&app);
  if (!runtime.initialized()) {
    qCritical() << "JSAPI runtime init failed";
    return 1;
  }
  qInfo() << "JSAPI ABI" << JSAPI_ABI_VERSION;

  // 1) 动态插件。
  const QString plugin_path =
      QDir(QCoreApplication::applicationDirPath()).filePath("demo_plugin.dll");
  QString plugin_error;
  if (!runtime.LoadPlugin(plugin_path, "demo", &plugin_error)) {
    qWarning() << "load demo_plugin.dll failed:" << plugin_error;
  }

  // 2) 静态注册的节点（编进本 exe）。
  runtime.registry().RegisterNode("host", &jsapi_demo::CreateHostNode);

  const StartupArgs args = ParseStartupArgs(argc, argv);

  QMainWindow window;
  window.setWindowTitle(
      QString::fromUtf8("JSAPI - 业务层示例 (%1 页)").arg(args.pages));
  window.resize(1280, 860);

  auto* tabs = new QTabWidget(&window);
  tabs->setDocumentMode(true);
  for (int i = 0; i < args.pages; ++i) {
    // 一个容器 = 一个独立 web 页（独立浏览器 + 独立同步通道）。
    auto* page = new jsapi::qt::WebContainer(&runtime);
    page->LoadUrl(QUrl(args.url));
    tabs->addTab(page, QString::fromUtf8("页面 %1").arg(i + 1));
  }
  window.setCentralWidget(tabs);
  window.show();

  return app.exec();
}
