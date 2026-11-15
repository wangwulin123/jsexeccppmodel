#pragma once

// jsapi_cef.dll 的公共接口：CEF 进程引导、节点注册、浏览器实例管理。
//
// 这一层是唯一允许依赖 CEF 的层，但它的公共头文件不含任何 CEF 类型：
// 宿主（Qt / Win32 / MFC / 其他 UI 框架）只通过本文件接入，因此不需要
// 编译 CEF 头文件，也不需要把 CEF 头文件加入 include 路径。
//
// 典型用法（非 Qt 宿主）：
//   int main(int argc, char** argv) {
//     if (int code = jsapi::cef::Runtime::RunSubprocess(); code >= 0) return code;
//     YourUiInit();
//     jsapi::cef::Runtime runtime;                 // CefInitialize
//     runtime.LoadPlugin("demo_plugin.dll", nullptr);
//     auto browser = runtime.CreateBrowser(opt);   // opt.parent_handle = HWND
//     YourUiLoop();
//     return 0;                                    // ~Runtime -> CefShutdown
//   }
//
// Qt 宿主请直接用 jsapi_qt 层的 jsapi::qt::Runtime + WebContainer。

#include <functional>
#include <memory>
#include <string>

#include "jsapi/js_registry.h"
#include "jsapi_cef/jsapi_cef_export.h"

namespace jsapi {
namespace cef {

struct RuntimeOptions {
  // CEF 的 root_cache_path；为空时用 <exe 目录>\cef_cache。
  std::string cache_path;
  bool no_sandbox = true;
  // CEF 使用独立 UI 线程跑消息循环（推荐）：渲染进程的同步调用不受宿主界面
  // 消息泵影响。宿主必须有自己稳定的消息循环（Qt / Win32 均可）。
  bool multi_threaded_message_loop = true;
};

// 绑定一个浏览器实例的生命周期回调。实现方在 CEF UI 线程被调用。
class BrowserObserver {
 public:
  virtual ~BrowserObserver() = default;
  virtual void OnBrowserCreated() {}
  virtual void OnBeforeClose() {}
  virtual void OnRenderProcessTerminated() {}
};

struct BrowserOptions {
  // 宿主窗口句柄（HWND）。为空时由 CEF 创建自己的顶层窗口。
  void* parent_handle = nullptr;
  int x = 0;
  int y = 0;
  int width = 0;
  int height = 0;
  // 高分屏缩放：控件的 physicalSize = 逻辑尺寸 * device_scale_factor。
  double device_scale_factor = 1.0;
  std::string url;
  std::shared_ptr<BrowserObserver> observer;
};

class BrowserHandleImpl;

// 单个浏览器实例的操作句柄。所有方法线程安全，内部投递到 CEF UI 线程执行。
class JSAPI_CEF_API BrowserHandle {
 public:
  BrowserHandle();
  ~BrowserHandle();
  BrowserHandle(const BrowserHandle&) = delete;
  BrowserHandle& operator=(const BrowserHandle&) = delete;

  // 把 CEF 子窗口铺满父窗口的 (x, y, width*dsf, height*dsf) 区域。
  void Resize(int x, int y, int width, int height, double device_scale_factor);
  void LoadUrl(const std::string& url);
  void Close(bool force);
  // 浏览器仍在生命周期内（已创建且未销毁）。
  bool valid() const;

 private:
  friend class Runtime;

  explicit BrowserHandle(std::shared_ptr<BrowserHandleImpl> impl);
  std::shared_ptr<BrowserHandleImpl> impl_;
};

class RuntimeImpl;

// 进程级运行时。一个进程只能有一个实例，且必须在 UI 初始化之前创建。
//
// 生命周期约束（由 CEF 进程模型决定，非本框架限制）：
//  - RunSubprocess() 必须在 main() 最开头、创建 QApplication 之前调用一次；
//  - Runtime 析构会关闭所有浏览器并 CefShutdown，之后不能再创建浏览器；
//  - 动态库一旦加载不允许 FreeLibrary（业务 exe 用导入库链接即天然满足）。
class JSAPI_CEF_API Runtime {
 public:
  // 当前进程是 CEF 的 renderer/GPU 子进程时返回 >= 0 的退出码，调用方应立即返回；
  // 浏览器进程返回 jsapi::cef::kNotSubprocess。
  static int RunSubprocess();
  static constexpr int kNotSubprocess = -1;

  explicit Runtime(const RuntimeOptions& options = RuntimeOptions());
  ~Runtime();
  Runtime(const Runtime&) = delete;
  Runtime& operator=(const Runtime&) = delete;

  bool initialized() const;

  // 统一的节点注册管理器（根节点 window.Extention）。
  jsapi::JsRegistry& registry();

  // 加载导出 GetExtensionJsApiObj() 的动态插件，并按 name 注册为命名节点。
  bool LoadPlugin(const std::string& path_utf8,
                  const std::string& name,
                  std::string* error = nullptr);

  // 阻塞式 UI 线程投递器：声明 ThreadAffinity::UI 的成员通过它切换执行线程，
  // 返回时任务必须已执行完（Qt 用 BlockingQueuedConnection）。
  void SetMainThreadDispatcher(
      std::function<void(std::function<void()>)> dispatcher);
  // 非阻塞投递器：同时作为"宿主支持同步嵌套事件"的能力开关。
  void SetMainThreadPoster(std::function<void(std::function<void()>)> poster);

  std::shared_ptr<BrowserHandle> CreateBrowser(const BrowserOptions& options);

  void CloseAllBrowsers(bool force_close);
  bool WaitForAllClosed(int timeout_ms);

 private:
  std::unique_ptr<RuntimeImpl> impl_;
};

}  // namespace cef
}  // namespace jsapi
