#include <windows.h>

#include <QApplication>
#include <QMainWindow>
#include <QThread>
#include <QUrl>
#include <QDebug>

#include <functional>
#include <memory>
#include <string>

#include "include/cef_app.h"
#include "include/cef_command_line.h"
#include "include/cef_render_process_handler.h"

#include "host_nodes.h"
#include "jsapi/js_registry.h"
#include "jsapi_bridge/jsapi_browser_bridge.h"

#include "qt_host_app.h"
#include "qt_host_client.h"
#include "qt_host_widget.h"

namespace {

std::wstring GetExecutableDir() {
  wchar_t path[MAX_PATH] = {};
  ::GetModuleFileNameW(::GetModuleHandleW(nullptr), path, MAX_PATH);
  std::wstring dir(path);
  const std::size_t pos = dir.find_last_of(L"\\/");
  if (pos != std::wstring::npos) {
    dir.resize(pos);
  }
  return dir;
}

}  // namespace

int main(int argc, char* argv[]) {
  CefMainArgs main_args(::GetModuleHandleW(nullptr));
  CefRefPtr<QtHostApp> app(new QtHostApp());

  // CEF 的 renderer/GPU 子进程复用同一个 exe，必须先分流并直接退出。
  const int subprocess_exit_code = CefExecuteProcess(main_args, app, nullptr);
  if (subprocess_exit_code >= 0) {
    return subprocess_exit_code;
  }

  QApplication qt_app(argc, argv);

  CefSettings settings;
  settings.no_sandbox = true;
  // CEF 使用自己的独立 UI 线程跑消息循环，Qt 主线程只处理界面事件；
  // 这样渲染进程的同步 JSAPI 调用不会因为 Qt 的消息泵而延迟。
  settings.multi_threaded_message_loop = true;
  CefString(&settings.root_cache_path)
      .FromWString(GetExecutableDir() + L"\\cef_cache");

  if (!CefInitialize(main_args, settings, app, nullptr)) {
    return CefGetExitCode();
  }

  // 浏览器进程侧的 JSAPI 宿主。
  auto bridge = std::make_shared<jsapi_bridge::JsApiBrowserBridge>();

  // 插件函数统一在 Qt GUI 线程执行：业务节点可以直接操作 Qt 控件。
  // 投递必须是阻塞式的——bridge 在 dispatcher 返回后立刻写回包。
  bridge->SetMainThreadDispatcher([](std::function<void()> task) {
    if (QThread::currentThread() == qApp->thread()) {
      task();
      return;
    }
    QMetaObject::invokeMethod(qApp, std::move(task),
                              Qt::BlockingQueuedConnection);
  });

  // 1) 动态插件：demo_plugin.dll 导出 GetExtensionJsApiObj()。
  const std::wstring plugin_wide = GetExecutableDir() + L"\\demo_plugin.dll";
  std::string plugin_utf8;
  {
    const int size = ::WideCharToMultiByte(CP_UTF8, 0, plugin_wide.data(),
                                           static_cast<int>(plugin_wide.size()),
                                           nullptr, 0, nullptr, nullptr);
    plugin_utf8.resize(static_cast<std::size_t>(size));
    ::WideCharToMultiByte(CP_UTF8, 0, plugin_wide.data(),
                          static_cast<int>(plugin_wide.size()),
                          plugin_utf8.data(), size, nullptr, nullptr);
  }

  std::string plugin_error;
  auto plugin_factory = jsapi::plugins::LoadDll(plugin_utf8, &plugin_error);
  if (plugin_factory) {
    // 生命周期由插件节点类里的 JSAPI_NODE_LIFE_TYPE 声明，注册时不参与。
    bridge->registry().RegisterNode("demo", std::move(plugin_factory));
  } else {
    qWarning() << "load demo_plugin.dll failed:" << plugin_error.c_str();
  }

  // 2) 静态注册：直接编进宿主的节点。
  bridge->registry().RegisterNode("host", &jsapi_demo::CreateHostNode);

  CefRefPtr<QtHostClient> client(new QtHostClient(bridge));

  // 默认加载随程序一起拷贝的示例页，可通过 --url= 覆盖。
  std::string url;
  CefRefPtr<CefCommandLine> command_line = CefCommandLine::CreateCommandLine();
  command_line->InitFromString(::GetCommandLineW());
  url = command_line->GetSwitchValue("url").ToString();
  if (url.empty()) {
    url = QUrl::fromLocalFile(QString::fromStdWString(
                                  GetExecutableDir() + L"\\jsapi_demo.html"))
              .toString()
              .toStdString();
  }

  int exit_code = 0;
  {
    QMainWindow window;
    window.setWindowTitle(QString::fromUtf8("JSAPI Lite - 精简 JSAPI 执行模型示例"));
    window.resize(1280, 860);
    window.setCentralWidget(
        new QtHostWidget(client, QString::fromStdString(url)));
    window.show();

    exit_code = qt_app.exec();

    client->CloseAllBrowsers(true);
    client->WaitForAllClosed(5000);
  }

  CefShutdown();
  return exit_code;
}
