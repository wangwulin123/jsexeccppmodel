#pragma once

// Qt 容器层：一个 WebContainer 就是一个独立 web 页（一个 CefBrowser + 一条
// 同步通道），可以直接嵌入主窗口，也可以放进独立弹窗。
//
//   auto* web = new jsapi::qt::WebContainer(&runtime, parent);
//   web->LoadUrl(QUrl::fromLocalFile("..."));
//   layout->addWidget(web);          // 嵌入
//   // 或：new QMainWindow + setCentralWidget(web) + show()  → 独立弹窗
//
// 每个容器自治：尺寸、导航、关闭都只影响自己，不存在"宿主级 index 管理"。

#include <QUrl>
#include <QWidget>

#include <memory>

#include "jsapi_qt/jsapi_qt_export.h"

namespace jsapi {
namespace cef {
class BrowserObserver;
}
namespace qt {

class Runtime;

class JSAPI_QT_API WebContainer : public QWidget {
 public:
  explicit WebContainer(Runtime* runtime, QWidget* parent = nullptr);
  ~WebContainer() override;

  // 首次调用会立即创建浏览器；容器不在可见状态时延后到 showEvent。
  void LoadUrl(const QUrl& url);

  // 页面生命周期观察者（CEF-free）：加载完成、关闭、渲染进程崩溃。
  void SetObserver(std::shared_ptr<jsapi::cef::BrowserObserver> observer);

  bool browser_created() const;
  QUrl url() const;

 protected:
  void showEvent(QShowEvent* event) override;
  void resizeEvent(QResizeEvent* event) override;

 private:
  void EnsureBrowser();

  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace qt
}  // namespace jsapi
