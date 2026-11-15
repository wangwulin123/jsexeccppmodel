#include "jsapi_cef/runtime.h"

#include <windows.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <utility>
#include <vector>

#include "include/cef_app.h"
#include "include/cef_command_line.h"

#include "jsapi_browser_bridge.h"
#include "jsapi_browser_handle.h"
#include "jsapi_cef_apps.h"

namespace jsapi {
namespace cef {

namespace {

std::wstring ExecutableDir() {
  wchar_t path[MAX_PATH] = {};
  ::GetModuleFileNameW(::GetModuleHandleW(nullptr), path, MAX_PATH);
  std::wstring dir(path);
  const std::size_t pos = dir.find_last_of(L"\\/");
  if (pos != std::wstring::npos) {
    dir.resize(pos);
  }
  return dir;
}

std::wstring Utf8ToWide(const std::string& text) {
  if (text.empty()) {
    return std::wstring();
  }
  const int size = ::MultiByteToWideChar(CP_UTF8, 0, text.data(),
                                         static_cast<int>(text.size()), nullptr, 0);
  std::wstring wide(static_cast<std::size_t>(size), L'\0');
  ::MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()),
                        wide.data(), size);
  return wide;
}

// 浏览器进程与渲染进程各自一个 CefApp：CEF 启动子进程时会带上 --type=。
CefRefPtr<CefApp> CreateProcessApp() {
  CefRefPtr<CefCommandLine> command_line = CefCommandLine::CreateCommandLine();
  command_line->InitFromString(::GetCommandLineW());
  if (command_line->GetSwitchValue("type").ToString() == "renderer") {
    return new jsapi_bridge::JsApiRendererApp();
  }
  return new jsapi_bridge::JsApiBrowserApp();
}

}  // namespace

class RuntimeImpl final : public BrowserHost {
 public:
  explicit RuntimeImpl(const RuntimeOptions& options) : options_(options) {
    CefMainArgs main_args(::GetModuleHandleW(nullptr));

    CefSettings settings;
    settings.no_sandbox = options_.no_sandbox;
    // CEF 用自己的 UI 线程跑消息循环，渲染进程的同步调用不受宿主界面消息泵影响。
    settings.multi_threaded_message_loop = options_.multi_threaded_message_loop;

    const std::wstring cache =
        options_.cache_path.empty()
            ? ExecutableDir() + L"\\cef_cache"
            : Utf8ToWide(options_.cache_path);
    CefString(&settings.root_cache_path).FromWString(cache);

    if (!CefInitialize(main_args, settings, CreateProcessApp(), nullptr)) {
      initialized_ = false;
      return;
    }
    bridge_ = std::make_shared<jsapi_bridge::JsApiBrowserBridge>();
    initialized_ = true;
  }

  ~RuntimeImpl() override {
    CloseAllBrowsers(true);
    WaitForAllClosed(5000);

    // 先释放桥（停掉每个浏览器的读取/会话线程），再关闭 CEF。
    {
      std::lock_guard<std::mutex> lock(browsers_mutex_);
      browsers_.clear();
    }
    bridge_.reset();

    if (initialized_) {
      CefShutdown();
      initialized_ = false;
    }
  }

  bool initialized() const { return initialized_; }

  jsapi::JsRegistry& registry() { return bridge_->registry(); }

  void SetMainThreadDispatcher(
      std::function<void(std::function<void()>)> dispatcher) {
    if (bridge_) {
      bridge_->SetMainThreadDispatcher(std::move(dispatcher));
    }
  }

  void SetMainThreadPoster(
      std::function<void(std::function<void()>)> poster) {
    if (bridge_) {
      bridge_->SetMainThreadPoster(std::move(poster));
    }
  }

  std::shared_ptr<BrowserHandleImpl> CreateBrowserImpl(
      const BrowserOptions& options) {
    // 引用计数由 CEF 的 IMPLEMENT_REFCOUNTING 统一管理，std::shared_ptr 只是
    // 其中一个引用：删除器走 Release。不能用 make_shared —— 那样 CEF 计数归零
    // 时的 delete this 会和控制块的释放撞在一起，进程退出时报
    // STATUS_HEAP_CORRUPTION（多浏览器时必现）。
    auto* raw = new BrowserHandleImpl(this, bridge_, options);
    raw->AddRef();
    std::shared_ptr<BrowserHandleImpl> handle(
        raw, [](BrowserHandleImpl* self) { self->Release(); });
    {
      std::lock_guard<std::mutex> lock(browsers_mutex_);
      browsers_.push_back(handle);
      open_count_.fetch_add(1);
    }
    handle->Start();
    return handle;
  }

  void CloseAllBrowsers(bool force_close) {
    std::vector<std::shared_ptr<BrowserHandleImpl>> alive;
    {
      std::lock_guard<std::mutex> lock(browsers_mutex_);
      for (auto it = browsers_.begin(); it != browsers_.end();) {
        if (auto handle = it->lock()) {
          alive.push_back(std::move(handle));
          ++it;
        } else {
          it = browsers_.erase(it);
        }
      }
    }
    for (const auto& handle : alive) {
      handle->Close(force_close);
    }
  }

  bool WaitForAllClosed(int timeout_ms) {
    std::unique_lock<std::mutex> lock(closed_mutex_);
    return closed_cv_.wait_for(lock, std::chrono::milliseconds(timeout_ms),
                               [this] { return open_count_.load() <= 0; });
  }

  // BrowserHost
  void OnBrowserCreated(CefRefPtr<CefBrowser> browser) override {
    if (bridge_) {
      // 先建同步通道：渲染进程可能在任何时刻发起首个同步 RPC。
      bridge_->OnAfterCreated(browser);
    }
  }

  void OnBrowserClosed(CefRefPtr<CefBrowser> browser) override {
    if (bridge_) {
      bridge_->OnBeforeClose(browser);
    }
    // 已关闭实例的 weak_ptr 在 CloseAllBrowsers 里顺带清理，这里只记账。
    open_count_.fetch_sub(1);
    closed_cv_.notify_all();
  }

  void OnRenderTerminated(CefRefPtr<CefBrowser> browser) override {
    if (bridge_) {
      bridge_->OnRenderProcessTerminated(browser);
    }
  }

  bool OnProcessMessage(CefRefPtr<CefBrowser> browser,
                        CefRefPtr<CefFrame> frame,
                        CefProcessId source_process,
                        CefRefPtr<CefProcessMessage> message) override {
    if (!bridge_) {
      return false;
    }
    return bridge_->OnProcessMessageReceived(browser, frame, source_process,
                                             message);
  }

  bool LoadPlugin(const std::string& path_utf8,
                  const std::string& name,
                  std::string* error) {
    auto factory = jsapi::plugins::LoadDll(path_utf8, error);
    if (!factory) {
      return false;
    }
    bridge_->registry().RegisterNode(name, std::move(factory));
    return true;
  }

 private:
  RuntimeOptions options_;
  bool initialized_ = false;
  std::shared_ptr<jsapi_bridge::JsApiBrowserBridge> bridge_;

  mutable std::mutex browsers_mutex_;
  std::vector<std::weak_ptr<BrowserHandleImpl>> browsers_;
  std::atomic<int> open_count_{0};

  std::mutex closed_mutex_;
  std::condition_variable closed_cv_;
};

// ---------------------------------------------------------------------------
// BrowserHandle
// ---------------------------------------------------------------------------

BrowserHandle::BrowserHandle() = default;
BrowserHandle::~BrowserHandle() = default;

BrowserHandle::BrowserHandle(std::shared_ptr<BrowserHandleImpl> impl)
    : impl_(std::move(impl)) {}

void BrowserHandle::Resize(int x,
                           int y,
                           int width,
                           int height,
                           double device_scale_factor) {
  if (impl_) {
    impl_->Resize(x, y, width, height, device_scale_factor);
  }
}

void BrowserHandle::LoadUrl(const std::string& url) {
  if (impl_) {
    impl_->LoadUrl(url);
  }
}

void BrowserHandle::Close(bool force) {
  if (impl_) {
    impl_->Close(force);
  }
}

bool BrowserHandle::valid() const { return impl_ && impl_->valid(); }

// ---------------------------------------------------------------------------
// Runtime
// ---------------------------------------------------------------------------

int Runtime::RunSubprocess() {
  CefMainArgs main_args(::GetModuleHandleW(nullptr));
  // 浏览器进程返回 -1，子进程返回自己的退出码。
  return CefExecuteProcess(main_args, CreateProcessApp(), nullptr);
}

Runtime::Runtime(const RuntimeOptions& options)
    : impl_(std::make_unique<RuntimeImpl>(options)) {}

Runtime::~Runtime() = default;

bool Runtime::initialized() const { return impl_ && impl_->initialized(); }

jsapi::JsRegistry& Runtime::registry() { return impl_->registry(); }

bool Runtime::LoadPlugin(const std::string& path_utf8,
                         const std::string& name,
                         std::string* error) {
  return impl_ && impl_->LoadPlugin(path_utf8, name, error);
}

void Runtime::SetMainThreadDispatcher(
    std::function<void(std::function<void()>)> dispatcher) {
  if (impl_) {
    impl_->SetMainThreadDispatcher(std::move(dispatcher));
  }
}

void Runtime::SetMainThreadPoster(
    std::function<void(std::function<void()>)> poster) {
  if (impl_) {
    impl_->SetMainThreadPoster(std::move(poster));
  }
}

std::shared_ptr<BrowserHandle> Runtime::CreateBrowser(
    const BrowserOptions& options) {
  if (!impl_ || !impl_->initialized()) {
    return nullptr;
  }
  return std::shared_ptr<BrowserHandle>(
      new BrowserHandle(impl_->CreateBrowserImpl(options)));
}

void Runtime::CloseAllBrowsers(bool force_close) {
  if (impl_) {
    impl_->CloseAllBrowsers(force_close);
  }
}

bool Runtime::WaitForAllClosed(int timeout_ms) {
  return impl_ && impl_->WaitForAllClosed(timeout_ms);
}

}  // namespace cef
}  // namespace jsapi
