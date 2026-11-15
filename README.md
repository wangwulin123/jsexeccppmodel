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
- **线程亲和可声明**：成员默认在 Worker 线程执行（不阻塞 UI）；需要操作窗口控件的成员声明为 `ThreadAffinity::UI`，由宿主 `SetMainThreadDispatcher` 投递到 Qt GUI 线程。
- **可靠性兜底**：插件异常自动转成 RPC 错误（不会终止进程）；同步调用超时（默认 10 s，可传 `timeout_ms`）后迟到回包按 `seq` 丢弃；渲染进程崩溃后通道自动失效并恢复、残留事件/回包/监听器全部作废；页面刷新后旧监听器自动清理。
- **可单测**：核心层（`jsapi.dll`）不依赖 CEF/Qt，`jsapi_self_test` 按可靠性 / 系统 / 性能三层组织，共 318 条检查（含超时、异常、并发与性能基线），可用 `--suite=` 选层运行。
- **分层交付**：三个动态库按依赖边界切分（`jsapi.dll` → `jsapi_cef.dll` → `jsapi_qt.dll`），业务层（`demo/`、`test/`）只 include `jsapi/` 与 `jsapi_qt/` 的公共头，构建期强制校验，见 [接入指南 · 分层与设计约定](docs/接入指南.md#1-分层与设计约定)。


## 架构

```
渲染进程 (renderer)                          浏览器进程 (browser)
┌──────────────────────────┐                ┌───────────────────────────────────┐
│ JS: window.Extention     │                │ JsApiBrowserBridge                │
│      .demo.add(2, 3)     │                │  ├─ JsRegistry  节点/句柄/事件表   │
│                          │  同步 RPC      │  ├─ RpcHost     解析→分发→执行     │
│  JsApiRenderHandler      │ ─────────────▶ │  └─ 读取线程(每浏览器) + 会话线程   │
│   ├─ V8 代理(Accessor)   │  共享内存+事件  │        │                          │
│   ├─ 回调句柄表           │ ◀───────────── │        ▼ 默认 Worker 线程          │
│   └─ pending_[seq]       │                │   Worker：插件函数执行              │
│                          │  异步 RPC      │   或 ThreadAffinity::UI → Qt GUI   │
│                          │ ─────────────▶ │                                   │
│                          │ CefProcessMsg  │  demo_plugin.dll / host_nodes      │
│                          │ ◀───────────── │                                   │
│  事件回调 (name, ...args) │ ◀───────────── │  事件：BroadcastEvent              │
│                          │                │        → CEF UI 线程 → 渲染进程    │
└──────────────────────────┘                └───────────────────────────────────┘
```

同步调用链路（以 `demo.add(2, 3)` 为例）：

```
V8 FunctionHandler::Execute
  → 编码 JSON → 写 request_map_ → SetEvent
  → 浏览器读取线程唤醒 → 读共享内存 → 解析 → 投递会话线程
  → 会话线程按成员线程亲和执行：
       Worker（默认）直接在当前线程执行
       UI             经 SetMainThreadDispatcher 投递 Qt GUI 线程（BlockingQueuedConnection）
  → 写 reply_map_ → SetEvent
  → 渲染进程 WaitForSingleObject(默认 10s) 唤醒 → 校验 seq → 解析回包 → 转 V8 返回值
```

> 读/执行分离：每个浏览器一条读取线程（唯一等待 `req_evt`）和一条会话线程（串行执行顶层请求），保证同一页面的调用严格串行；渲染进程在同步等待期间到达的嵌套请求会被内联执行，最多 8 层。
> 同步请求**不能**走 `CefProcessMessage`：调用发生在渲染主线程的 V8 回调里且立即阻塞，而进程消息要等主线程回到消息循环才会发出。

## 分层架构

```
业务层（demo/）                                  零 CEF、零 jsapi_cef 类型
  只允许 include: jsapi/*.h、jsapi_qt/*.h
        │
        ▼
jsapi_qt.dll    Qt 容器层    jsapi::qt::Runtime + WebContainer（继承 QWidget + pimpl）
        │                   把执行点绑定到 Qt GUI 线程；一容器 = 一浏览器 = 一通道
        ▼
jsapi_cef.dll   CEF 适配层   jsapi::cef::Runtime（子进程分流/初始化/插件加载/注册表）
        │                   + BrowserHandle（CEF-free 的浏览器句柄）
        │                   公共头不含任何 CEF 类型 → 非 Qt 宿主也能直接接
        ▼
jsapi.dll       执行模型层   JsValue / 函数表 / 节点基类 / JsRegistry / RpcHost / 同步通道
                            零 CEF、零 Qt；插件与自测只依赖这一层
```

三者的公共头就是 SDK 契约；`src/cef/jsapi_browser_bridge.h` 这类 CEF 内部头不对外安装。
业务 exe 的全部代码量 ≈ 初始化 Runtime + 加载插件 + N 个 `WebContainer`：

```cpp
if (int code = jsapi::qt::Runtime::RunSubprocess(); code >= 0) return code;  // CEF 子进程分流
QApplication app(argc, argv);
jsapi::qt::Runtime runtime(&app);                    // CefInitialize + Qt 线程投递器
runtime.LoadPlugin("demo_plugin.dll", "demo");        // 统一节点注册管理器
runtime.registry().RegisterNode("host", &CreateHostNode);

QMainWindow win;
auto* web = new jsapi::qt::WebContainer(&runtime);    // 嵌入主窗口
web->LoadUrl(QUrl::fromLocalFile(".../jsapi_demo.html"));
win.setCentralWidget(web); win.show();
return app.exec();                                    // ~Runtime → 关闭浏览器 → CefShutdown
```

## 目录结构

```
jsapi_demo/
├─ demo/
│  ├─ demo_app/             纯业务 exe（Runtime + WebContainer + 节点注册，零 CEF）
│  ├─ demo_plugin/          动态库插件示例（DemoNode / DemoChildNode / BenchNode）
│  ├─ host_nodes/           静态注册节点示例（HostNode）
│  └─ resources/
│     ├─ jsapi_demo.html    交互式功能示例页
│     └─ jsapi_perf.html    自动性能基准页
├─ include/
│  ├─ jsapi/                业务层唯一依赖：JsValue、函数表、节点基类、JsRegistry、ABI 版本
│  ├─ jsapi_rpc/            请求解析与按索引分发（不依赖 CEF）
│  ├─ jsapi_channel/        共享内存/命名事件通道 + 渲染侧 SyncChannel（不依赖 CEF/Qt）
│  ├─ jsapi_cef/            适配层公共头（CEF-free）：Runtime、BrowserHandle、BrowserObserver
│  └─ jsapi_qt/             容器层公共头：Runtime、WebContainer
├─ src/
│  ├─ core/ rpc/ channel/   jsapi.dll 的实现
│  ├─ cef/                  jsapi_cef.dll 的实现（CEF 内部头只在这里）
│  └─ qt/                   jsapi_qt.dll 的实现
├─ test/
│  ├─ reliability/          可靠性基础测试（值模型、RPC、事件、通道、生命周期）
│  ├─ system/               整体系统测试（插件端到端链路）
│  ├─ perf/                 性能基线（吞吐 / 延迟）
│  └─ support/              测试框架与共享夹具
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

CEF 与 Qt 体积较大，**不纳入本仓库**。缺失时 `jsapi.dll` / `jsapi_self_test` 仍可正常构建，只有 `jsapi_cef` / `jsapi_qt` / `demo_app` 会被跳过。

## 快速开始

```powershell
$root = "C:\path\to\jsapi_demo"

# 1) 配置（默认在 $root\build 生成 VS 工程）
cmake -S $root -B "$root\build" -G "Visual Studio 18 2026" -A x64 `
      -DCEF_ROOT="C:\path\to\cef_binary" `
      -DCMAKE_PREFIX_PATH="C:\path\to\qt\6.8.3\msvc2022_64"

# 2) 构建
cmake --build "$root\build" --config Release --target demo_app jsapi_self_test -- /m

# 3) 运行自测（三层共 318 条；也可 --suite=reliability|system|perf 单独跑）
& "$root\build\bin\Release\jsapi_self_test.exe"

# 4) 运行业务示例（默认加载同目录 jsapi_demo.html）
& "$root\build\bin\Release\demo_app.exe"

# 5) 指定页面
& "$root\build\bin\Release\demo_app.exe" --url=file:///C:/path/to/jsapi_perf.html

# 6) 多容器（每个容器独立浏览器/通道，用于并发与隔离验证）
& "$root\build\bin\Release\demo_app.exe" --pages=3

# 7) 导出 SDK（include/ + bin/ + lib/cmake/jsapi）
cmake --install "$root\build" --config Release --prefix "$root\build\sdk"
```

构建时会把 `libcef.dll` 等 CEF 运行时、`demo_plugin.dll`、示例页与 Qt 运行时 DLL 一并复制到 `demo_app.exe` 同目录，可直接运行。

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

// 同步事件：C++ 阻塞等待前端回调的返回值（回调里还能再次调用 C++，最多 8 层）
Extention.host.addEventListener("sync-echo", (name, text) => text.toUpperCase());
Extention.host.syncEcho("hi");        // → "HI"
```

> 成员函数的执行线程由声明决定：默认 `ThreadAffinity::Worker`（不阻塞 UI），需要操作窗口控件时声明 `ThreadAffinity::UI`。Worker 成员里不能直接碰 Qt 控件，否则行为未定义。

## 可靠性

| 场景 | 框架行为 |
| --- | --- |
| 插件函数耗时过久 | 前端同步调用默认 10 s 超时（可显式传 `timeout_ms`）；超时只放弃等待，**不会中断**已在执行的插件函数 |
| 前端回调耗时过久 | 同步事件等待受同一个超时约束；超时后按错误返回，迟到回包按 `seq` 丢弃 |
| 插件函数抛异常 | 被框架捕获并转成 RPC 错误（`... threw: ...`），进程不退出，后续调用立即可用 |
| 前端回调抛异常 | 转成错误回执返回给 C++，不穿透到原生栈，事件链路保持可用 |
| 前端页面崩溃 | 通道保留但快速失败：同步事件立刻返回 `render process terminated`，残留回包/事件/监听器清空；新渲染进程接入后首次调用自动恢复 |
| 页面刷新/导航 | 事件按当前 document 的 `(browser, frame)` 派发，旧监听器自动清理，不会命中已销毁的回调 |
| 嵌套调用过深 | 同步嵌套上限 8 层，超出返回明确错误；单次事件 payload 上限 1 MB |

> 架构性约束（无法在实现层消除、需要用规范规避的）见 [接入指南 §7](docs/接入指南.md#7-可靠性边界与使用约束)：同页调用严格串行、超时不取消执行、跨页共享状态需自行加锁。

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

- [接入指南](docs/接入指南.md)：分层与依赖关系、各层职责与 dll/头文件、三步接入（声明节点 → 注册节点 → 宿主装配）
- [API 参考](docs/API参考.md)：`JsValue`、函数表、`JsCallContext`、注册表、生命周期、事件
- [协议说明](docs/协议说明.md)：共享内存请求/回包格式、异步消息、事件消息
- [构建与调试](docs/构建与调试.md)：构建目标、SDK 安装、ABI 版本、运行参数、常见问题
- [性能基准](docs/性能基准.md)：测量方法与实测数据
- [架构升级方案](架构升级方案-同步RPC嵌套调用.md)：同步嵌套的设计推演、实现偏差与 v1.4→v1.7 落地记录

## 平台

当前实现基于 **Windows**（共享内存、命名事件、`LoadLibrary`）。执行模型层（`jsapi.dll`）与 CEF 无关，把 `src/channel` 的同步通道替换掉即可移植到其他平台；CEF 适配与 Qt 容器各自独立成库，移植时互不影响。

## License

[PolyForm Noncommercial 1.0.0](LICENSE)，禁止商业用途；商业授权请联系 wwllaowang@qq.com。
