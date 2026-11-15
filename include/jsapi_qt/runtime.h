#pragma once

// jsapi_qt.dll 的公共接口：Qt 宿主运行时。
//
// 本层是 jsapi_cef 之上的 Qt 容器层，负责两件事：
//  1. 把 JSAPI 的执行点绑定到 Qt GUI 线程（ThreadAffinity::UI 成员、同步事件）；
//  2. 提供 WebContainer 控件（见 web_container.h）。
//
// 业务代码只 include 本文件与 jsapi/*，不接触 CEF 与 jsapi_cef 的任何类型。

#include <QApplication>
#include <QString>

#include <memory>

#include "jsapi/js_registry.h"
#include "jsapi_cef/runtime.h"
#include "jsapi_qt/jsapi_qt_export.h"

namespace jsapi {
namespace qt {

class JSAPI_QT_API Runtime {
 public:
  // 必须在 main() 最开头、QApplication 之前调用。
  // 返回 >= 0 表示当前进程是 CEF 的 renderer/GPU 子进程，main 应直接返回该值。
  static int RunSubprocess() { return jsapi::cef::Runtime::RunSubprocess(); }

  // app 必须已构造。Runtime 负责 CEF 初始化与线程投递器装配。
  explicit Runtime(QApplication* app,
                   const jsapi::cef::RuntimeOptions& options = {});
  ~Runtime();
  Runtime(const Runtime&) = delete;
  Runtime& operator=(const Runtime&) = delete;

  bool initialized() const;

  // 统一的节点注册管理器。前端的 window.Extention.<node> 即注册的命名节点。
  jsapi::JsRegistry& registry();

  // 加载动态插件（导出 GetExtensionJsApiObj）并注册为命名节点 node_name。
  bool LoadPlugin(const QString& path,
                  const QString& node_name,
                  QString* error = nullptr);

  jsapi::cef::Runtime* core() { return core_.get(); }
  QApplication* application() const { return app_; }

 private:
  QApplication* app_ = nullptr;
  std::unique_ptr<jsapi::cef::Runtime> core_;
};

}  // namespace qt
}  // namespace jsapi
