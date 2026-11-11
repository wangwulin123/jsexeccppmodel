#include "qt_host_widget.h"

#include <windows.h>

#include <QPalette>
#include <QResizeEvent>
#include <QShowEvent>

#include "include/cef_browser.h"
#include "qt_host_client.h"

QtHostWidget::QtHostWidget(CefRefPtr<QtHostClient> client,
                           const QString& url,
                           QWidget* parent)
    : QWidget(parent), client_(client), url_(url) {
  setAttribute(Qt::WA_NativeWindow);
  setFocusPolicy(Qt::StrongFocus);
  setMinimumSize(320, 240);
  setAutoFillBackground(true);

  QPalette pal = palette();
  pal.setColor(QPalette::Window, Qt::black);
  setPalette(pal);
}

void QtHostWidget::showEvent(QShowEvent* event) {
  QWidget::showEvent(event);
  if (!browser_created_) {
    browser_created_ = true;
    CreateBrowser();
  }
}

void QtHostWidget::CreateBrowser() {
  HWND parent = reinterpret_cast<HWND>(winId());

  const qreal dpr = devicePixelRatioF();
  CefRect rect(0, 0, qRound(width() * dpr), qRound(height() * dpr));

  CefWindowInfo window_info;
  window_info.runtime_style = CEF_RUNTIME_STYLE_ALLOY;
  window_info.SetAsChild(parent, rect);

  CefBrowserSettings browser_settings;
  // CEF 在自己的 UI 线程上跑消息循环，创建浏览器必须投递过去。
  client_->CreateBrowserAsync(window_info, browser_settings,
                              url_.toStdString());
}

void QtHostWidget::resizeEvent(QResizeEvent* event) {
  QWidget::resizeEvent(event);
  client_->ResizePrimaryBrowser();
}
