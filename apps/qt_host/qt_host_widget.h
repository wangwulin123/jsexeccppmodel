#pragma once

#include <QWidget>

#include "include/cef_browser.h"

class QtHostClient;

// 把 CEF 的浏览器子窗口嵌入 Qt 控件。
class QtHostWidget final : public QWidget {
 public:
  QtHostWidget(CefRefPtr<QtHostClient> client,
               const QString& url,
               QWidget* parent = nullptr);

 protected:
  void showEvent(QShowEvent* event) override;
  void resizeEvent(QResizeEvent* event) override;

 private:
  void CreateBrowser();

  CefRefPtr<QtHostClient> client_;
  QString url_;
  bool browser_created_ = false;
};
