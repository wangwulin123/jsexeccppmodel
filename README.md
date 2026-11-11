# JSAPI Lite

一个以**函数表**驱动的 CEF 精简 JSAPI 执行模型：前端通过 `window.Extention.<节点>.<方法>()` 直接调用浏览器进程里的 C++ 对象，支持同步/异步调用、属性读写、事件回调、对象句柄与临时对象生命周期管理。

适用于「Web 前端 + C++ 宿主（Qt/MFC/自绘）」的混合应用：业务能力以节点（Node）为单位注册，插件只需声明函数表，不需要手写 IPC 编解码、也不需要暴露原生指针给 V8。

## 特性

- **函数表驱动**：节点在 `DeclareMembers()` 里声明函数/属性，运行时按 `(handle, index)` 分发，前端拿不到明文函数名。
- **两种调用语义**：同步调用走共享内存 + 命名事件（约 30 µs 往返）；异步调用返回 Promise，走 `CefProcessMessage`。
- **属性即 RPC**：`obj.count` / `obj.count = 5` 分别映射到 getter / setter，支持只读、类型校验与错误回传。
- **事件系统**：所有节点自带 `addEventListener` / `removeEventListener` / `dispatchEvent` / `getAllEventInfo`，支持按 `browser` / `frame` 定向派发与 `excludeSelf`。
- **对象句柄**：C++ 可返回新的节点句柄，渲染进程自动建立新的 V8 代理；临时对象随代理 GC 释放（引用计数）。
- **两种扩展方式**：动态库插件（导出 `GetExtensionJsApiObj()`）或静态注册（直接编进宿主）。
- **执行点可控**：通过 `SetMainThreadDispatcher` 把所有插件函数切到宿主 UI 线程（示例为 Qt GUI 线程），插件里可直接操作窗口控件。
- **可单测**：核心层（`jsapi_core` / `jsapi_rpc`）不依赖 CEF/Qt，`jsapi_self_test` 覆盖 113 条断言。

## 架构

```
渲染进程 (renderer)                          浏览器进程 (browser)
┌──────────────────────────┐                ┌───────────────────────────────────┐
│ JS: window.Extention     │                │ JsApiBrowserBridge                │
│      .demo.add(2, 3)     │                │  ├─ JsRegistry  节点/句柄/事件表   │
│                          │  同步 RPC      │  ├─ RpcHost     解析→分发→执行     │
│  JsApiRenderHandler      │ ─────────────▶ │  └─ SyncLoop 工作线程              │
│   ├─ V8 代理(Accessor)   │  共享内存+事件  │        │                          │
│   ├─ 回调句柄表           │ ◀───────────── │        ▼ SetMainThreadDispatcher  │
│   └─ pending_[seq]       │                │   Qt GUI 线程：插件函数执行        │
│                          │  异步 RPC      │                                   │
│                          │ ─────────────▶ │  demo_plugin.dll / host_nodes      │
│                          │ CefProcessMsg  │                                   │
│                          │ ◀───────────── │  事件：BroadcastEvent              │
│  事件回调 (name, ...args) │ ◀───────────── │        → CEF UI 线程 → 渲染进程    │
└──────────────────────────┘                └───────────────────────────────────┘
```

同步调用链路（以 `demo.add(2, 3)` 为例）：

```
V8 FunctionHandler::Execute
  → 编码 JSON → 写 request_map_ → SetEvent
  → 浏览器 worker_ 线程 SyncLoop 唤醒 → 读共享内存 → 解析
  → 投递 Qt GUI 线程执行插件函数（BlockingQueuedConnection）
  → 写 reply_map_ → SetEvent
  → 渲染进程 WaitForSingleObject(10s) 唤醒 → 解析回包 → 转 V8 返回值
```

> 同步请求**不能**走 `CefProcessMessage`：调用发生在渲染主线程的 V8 回调里且立即阻塞，而进程消息要等主线程回到消息循环才会发出。

## 目录结构

```
jsapi_demo/
├─ apps/qt_host/            Qt + CEF 宿主（浏览器进程、窗口、CefClient）
├─ include/
│  ├─ jsapi/                JsValue、函数表、节点基类、句柄注册表
│  ├─ jsapi_rpc/            请求解析与按索引分发（不依赖 CEF）
│  └─ jsapi_bridge/         渲染侧 JS 代理、浏览器侧桥、协议常量
├─ src/core|rpc|bridge/     上述三层的实现
├─ plugins/
│  ├─ demo_plugin/          动态库插件示例（DemoNode / DemoChildNode / BenchNode）
│  └─ host_nodes/           静态注册节点示例（HostNode）
├─ resources/
│  ├─ jsapi_demo.html       交互式功能示例页
│  └─ jsapi_perf.html       自动性能基准页
├─ tests/self_test.cpp      113 条自测（不依赖 CEF/Qt）
├─ docs/                    接入指南 / API 参考 / 协议说明 / 构建与调试 / 性能基准
└─ CMakeLists.txt
```

## 依赖

| 依赖 | 版本 | 说明 |
| --- | --- | --- |
| CEF | 二进制包（含 `include/cef_app.h`） | 默认识别 `<repo>/../../cef_binary`，可用 `-DCEF_ROOT=` 覆盖 |
| Qt | 6.8.3 msvc2022_64（Core/Widgets） | 默认识别 `<repo>/../../qt/6.8.3/msvc2022_64`，可用 `-DCMAKE_PREFIX_PATH=` 覆盖 |
| CMake | ≥ 3.21 | 使用 Visual Studio 生成器（x64） |
| MSVC | VS 2022/2026 | `/utf-8`，C++20 |

CEF 与 Qt 体积较大，**不纳入本仓库**。缺失时 `jsapi_core` / `jsapi_rpc` / `jsapi_self_test` 仍可正常构建，只有 `jsapi_bridge` 与 `jsapi_qt_host` 会被跳过。

## 快速开始

```powershell
$root = "C:\path\to\jsapi_demo"

# 1) 配置（默认在 $root\build 生成 VS 工程）
cmake -S $root -B "$root\build" -G "Visual Studio 18 2026" -A x64 `
      -DCEF_ROOT="C:\path\to\cef_binary" `
      -DCMAKE_PREFIX_PATH="C:\path\to\qt\6.8.3\msvc2022_64"

# 2) 构建
cmake --build "$root\build" --config Release --target jsapi_qt_host demo_plugin jsapi_self_test -- /m

# 3) 运行自测
& "$root\build\bin\Release\jsapi_self_test.exe"

# 4) 运行宿主（默认加载同目录 jsapi_demo.html）
& "$root\build\apps\qt_host\Release\jsapi_qt_host.exe"

# 5) 指定页面
& "$root\build\apps\qt_host\Release\jsapi_qt_host.exe" --url=file:///C:/path/to/jsapi_perf.html
```

构建时会把 `demo_plugin.dll`、`jsapi_demo.html`、`jsapi_perf.html` 以及 Qt 运行时 DLL 一并复制到宿主 exe 同目录，可直接运行。

## 前端用法速览

```js
// 节点由 C++ 注册：registry.RegisterNode("demo", factory)
const demo = Extention.demo;

demo.add(2, 3);                       // 同步：直接拿到返回值
const value = await demo.sumAsync(20, 22);  // 异步：返回 Promise
demo.count = 25;                      // 属性写
console.log(demo.count);              // 属性读

const child = demo.child();           // 返回对象句柄 → 新的 V8 代理
child.ping("hi");                     // 临时对象，代理回收后自动释放

demo.addEventListener("hello", (name, ...args) => {
  console.log(name, args);            // 回调签名固定为 (eventName, ...args)
});
demo.emit("hello", ["world", 42]);    // 广播
demo.dispatchEvent("hello", ["x"], { browser, frame, excludeSelf: true }); // 定向
```

更多细节见 [接入指南](docs/接入指南.md) 与 [API 参考](docs/API参考.md)。

## 性能

Release、i5-13490F、Win10 19045、Chrome 156，空接口 / 空 JS 回调（不含 describe）：

| 场景 | 中位耗时 |
| --- | --- |
| 同步空接口往返 | **30.3 µs**（≈ 3.3 万次/秒） |
| 异步空接口往返 | **68.6 µs** |
| C++ → JS 事件，空载单发 | **267 µs (p50)** / 604 µs (p90) |
| C++ → JS 事件，连续 300 条 | 1.57 ms (p50)，吞吐 ≈ 11 万条/秒 |

完整口径、链路分解与局限见 [性能基准](docs/性能基准.md)。

## 文档

- [接入指南](docs/接入指南.md)：三步接入（声明节点 → 注册节点 → 宿主装配）
- [API 参考](docs/API参考.md)：`JsValue`、函数表、`JsCallContext`、注册表、生命周期、事件
- [协议说明](docs/协议说明.md)：共享内存请求/回包格式、异步消息、事件消息
- [构建与调试](docs/构建与调试.md)：构建目标、运行参数、常见问题
- [性能基准](docs/性能基准.md)：测量方法与实测数据

## 平台

当前实现基于 **Windows**（共享内存、命名事件、`LoadLibrary`）。核心模型与 CEF 无关，移植到其他平台需要替换 `src/bridge` 下的同步通道实现。

## License

[MIT](LICENSE)
