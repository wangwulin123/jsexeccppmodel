# API 参考

头文件位于 `include/`：

| 头文件 | 内容 | 使用方 |
| --- | --- | --- |
| `jsapi/js_value.h` | `JsType`、`JsValue`、`ParseJson` | 业务 / 插件 |
| `jsapi/js_table.h` | `Param`、`JsMember`、`JsTable`、`JsTableBuilder` | 业务 / 插件 |
| `jsapi/js_object.h` | `RpcEnv`、`JsCallContext`、`LifeType`、`JsObjectBase`、`JSAPI_NODE_LIFE_TYPE` | 业务 / 插件 |
| `jsapi/js_registry.h` | `JsEventSink`、`JsRegistry`、`plugins::LoadDll`、`JSAPI_EXPORT_EXTENSION` | 业务 / 插件 |
| `jsapi/jsapi_version.h` | `JSAPI_ABI_VERSION` | 业务 / 插件 |
| `jsapi_rpc/jsapi_rpc.h` | `RpcRequest`、`RpcResponse`、`RpcHost`、`CoerceValue` | 适配层 / 高级 |
| `jsapi_channel/jsapi_channel.h` | 通道名推导、共享内存布局、读写端 | 高级 / 自测 |
| `jsapi_channel/jsapi_sync_channel.h` | `SyncChannel`（渲染侧同步调用） | 高级 / 自测 |
| `jsapi_cef/runtime.h` | `jsapi::cef::Runtime`、`BrowserHandle`、`BrowserObserver`（**不含 CEF 类型**） | 非 Qt 宿主 |
| `jsapi_qt/runtime.h` | `jsapi::qt::Runtime`（CEF 初始化 + Qt 线程投递器） | Qt 宿主 |
| `jsapi_qt/web_container.h` | `jsapi::qt::WebContainer`（QWidget 容器） | Qt 宿主 |

命名空间：执行模型层在 `namespace jsapi`；适配层公共接口在 `namespace jsapi::cef`；容器层在 `namespace jsapi::qt`。`src/cef/` 下的内部实现沿用历史命名 `namespace jsapi_bridge`，属于私有头，不对外安装。

CEF 内部头（`jsapi_browser_bridge.h`、`jsapi_render_handler.h`、`jsapi_render_core.h`、`jsapi_render_types.h`、`jsapi_sync_channel.cpp` 等）只存在于 `src/cef/`，业务层不可见。分层规范见 [接入指南 · 分层与设计约定](接入指南.md#1-分层与设计约定)。

---

## JsType / JsValue

### JsType

```cpp
enum class JsType : int {
  Null = 0, Bool, Int, Double, String, Array, Object,
  ObjectHandle, CallbackHandle, Any,
};
```

- `ObjectHandle`：浏览器进程对象句柄，JSON 编码为 `{"$obj": id}`。
- `CallbackHandle`：渲染进程回调句柄，JSON 编码为 `{"$cb": id}`。
- `Any`：不做类型校验，原样透传。
- `TypeName()` / `TypeFromName()` 用于 describe 与调试输出。

### JsValue

统一的值容器（`std::variant`），跨进程时序列化为 JSON。

```cpp
// 构造
JsValue::Null();
JsValue::Bool(true);
JsValue::Int(42);
JsValue::Double(3.14);
JsValue::String("text");
JsValue::Array({JsValue::Int(1), JsValue::Int(2)});
JsValue::Object({{"key", JsValue::String("v")}});
JsValue::ObjectHandle(handle);
JsValue::CallbackHandle(id);

// 读取（类型不匹配时返回 fallback，不做隐式转换）
JsType type() const;
bool is_null() const;
bool AsBool(bool fallback = false) const;
std::int64_t AsInt(std::int64_t fallback = 0) const;
double AsDouble(double fallback = 0.0) const;
std::string AsString(const std::string& fallback = {}) const;
const std::vector<JsValue>& AsArray() const;
const std::map<std::string, JsValue>& AsObject() const;
int AsHandle(int fallback = -1) const;

// 便捷修改（仅 Object / Array 生效）
void Set(const std::string& key, JsValue value);
const JsValue* Find(const std::string& key) const;
void Push(JsValue value);

std::string ToJson() const;

// 解析 JSON；失败返回 false 并写 error
bool ParseJson(const std::string& json, JsValue* out, std::string* error);
```

`AsObject()` / `AsArray()` 在类型不符时返回静态空容器，可直接遍历。

---

## 函数表

### Param

```cpp
struct Param {
  std::string name;          // 仅用于错误信息与 describe
  JsType type = JsType::Any; // 入参强转目标
  bool optional = false;     // 可选参数必须排在必填参数之后
};
```

### JsTableBuilder

```cpp
enum class ThreadAffinity { Worker, UI };

JsTableBuilder& Function(const std::string& name,
                         JsType return_type,
                         std::vector<Param> params,
                         JsFunction function,
                         bool async = false,
                         ThreadAffinity affinity = ThreadAffinity::Worker);

JsTableBuilder& AsyncFunction(const std::string& name,
                              JsType return_type,
                              std::vector<Param> params,
                              JsFunction function,
                              ThreadAffinity affinity = ThreadAffinity::Worker);

JsTableBuilder& Property(const std::string& name,
                         JsType type,
                         JsGetter getter,
                         JsSetter setter = nullptr,
                         ThreadAffinity affinity = ThreadAffinity::Worker);
```

```cpp
using JsFunction = std::function<void(JsObjectBase* self, JsCallContext& ctx)>;
using JsGetter   = std::function<JsValue(JsObjectBase* self)>;
using JsSetter   = std::function<bool(JsObjectBase* self,
                                      const JsValue& value,
                                      std::string* error)>;
```

规则：

- 成员**声明顺序即索引**，`JsTable` 内 `FindByIndex` 用它分发；已下发给渲染进程的索引不能漂移。
- 属性不带 setter 即只读，渲染进程侧也标记为不可写。
- 属性在渲染进程是 accessor：读取/赋值都会产生一次同步 RPC，注意不要在循环里频繁读写大量属性。
- `affinity` 决定函数体在哪条线程执行：
  - `Worker`（默认）：该页面的会话线程。可自由阻塞，适合计算、IO、注册表等与线程亲和无关的操作；
  - `UI`：宿主 UI 主线程（经 `SetMainThreadDispatcher` 阻塞切换），适合操作 Qt 控件等必须 UI 线程的代码。
  - 声明为 `UI` 时函数体应尽量短：它占用宿主 UI 线程，长耗时操作会拖慢所有页面。

### JsTable

```cpp
const JsMember* FindByName(const std::string& name) const;
const JsMember* FindByIndex(int index) const;
const std::vector<JsMember>& members() const;
```

`JsMember` 包含 `name / is_property / index / type / params / async / writable / affinity` 以及对应回调。

---

## JsObjectBase

```cpp
class JsObjectBase {
 public:
  virtual ~JsObjectBase() = default;

  const JsTable& GetTable() const;
  void InvalidateTable() const;
  void Attach(JsRegistry* registry, int handle, const RpcEnv& env);

  virtual LifeType life_type() const;
  static constexpr LifeType kLifeType = LifeType::Singleton;

  int handle() const;
  JsRegistry* registry() const;
  const RpcEnv& env() const;

  bool AddEventListener(const std::string& event, int callback_id, const RpcEnv& env);
  bool RemoveEventListener(const std::string& event, int callback_id);
  int  BroadcastEvent(const std::string& event, const std::vector<JsValue>& args);
  int  DispatchEvent(const std::string& event, const RpcEnv& target,
                     const std::vector<JsValue>& args);
  JsValue DispatchEventSync(const std::string& event, const RpcEnv& target,
                            const std::vector<JsValue>& args,
                            int timeout_ms = 10000, std::string* error = nullptr);
  JsValue GetAllEventInfo() const;
  int DiscardListeners(int browser_id, const std::string& keep_frame_id);

 protected:
  virtual void DeclareMembers(JsTableBuilder& table) const {}
};
```

- 函数表懒构建并缓存（`table_mutex_` 保护）。`InvalidateTable()` 丢弃缓存，下次 `GetTable()` 重新声明；注册新命名节点时 `JsRegistry` 会自动调用。
- `handle()` / `registry()` / `env()` 只在被注册表接管后有效；`env()` 是**创建时**的环境。
- 事件监听表由 `listener_mutex_` 保护，`BroadcastEvent` / `DispatchEvent` 返回命中的监听者数量。
- `DispatchEventSync` 只支持单播：取首个 `event` 与 `target` 都匹配的监听者，阻塞等待其返回值；`timeout_ms <= 0` 时使用默认 10 s。失败通过 `error` 返回（`no listener for event 'x'`、`sync event timed out`、`render process terminated`）。
- `DiscardListeners(browser_id, keep_frame_id)` 清理失效监听器：`keep_frame_id` 非空时只保留该 frame（当前 document），为空则清空该浏览器的全部监听（渲染进程崩溃）。框架在「新 document 建根对象」与「渲染进程终止」时自动调用，插件通常不需要手动调用。

### JSAPI_NODE_LIFE_TYPE

```cpp
#define JSAPI_NODE_LIFE_TYPE(TYPE)                        \
  static constexpr ::jsapi::LifeType kLifeType = (TYPE);  \
  ::jsapi::LifeType life_type() const override { return kLifeType; }
```

不写时默认 `LifeType::Singleton`。

| 值 | 创建 | 释放 |
| --- | --- | --- |
| `Singleton` | 命名节点首次访问时创建一次 | 随注册表析构（宿主退出） |
| `Transient` | 每次 `AddObject` 都是新实例 | 渲染进程代理 GC → `release` 消息 → 引用计数归零后释放 |

---

## JsCallContext

```cpp
JsCallContext(JsObjectBase* self, std::vector<JsValue> args,
              JsRegistry* registry, const RpcEnv& env);

JsObjectBase* self() const;
const std::vector<JsValue>& args() const;
JsRegistry* registry() const;
const RpcEnv& env() const;

void Return(JsValue value);
void Fail(std::string error);

bool has_value() const;
bool has_error() const;
const JsValue& value() const;
const std::string& error() const;
```

- `args()` 已按声明类型强转（`Any` 原样），长度在 `[必填数, 声明总数]` 内。
- `Return()` 之后再 `Fail()` 以 `Fail` 为准（`Fail` 会清掉 value）。
- 未调用 `Return` 时函数返回 `null`。

### RpcEnv

```cpp
struct RpcEnv {
  int browser_id = -1;
  std::string frame_id;
  bool valid() const;
};
```

标识一次调用来自哪个浏览器、哪个 frame。`browser_id < 0` 表示不限，`frame_id` 为空表示不限。

---

## JsRegistry

```cpp
using Factory = std::function<std::shared_ptr<JsObjectBase>()>;

struct ObjectStats {
  int handles = 0;         // 已分配的句柄槽位（含已释放的空槽）
  int transient_live = 0;  // 仍存活的临时对象数
  int release_refs = 0;    // 临时对象当前代理引用计数总和
};

void SetEventSink(JsEventSink* sink);
int  AddObject(std::shared_ptr<JsObjectBase> object, const RpcEnv& env);
std::shared_ptr<JsObjectBase> GetObjectByHandle(int handle);
ObjectStats GetObjectStats() const;
bool ReleaseObject(int handle);
std::shared_ptr<JsObjectBase> Root(const RpcEnv& env);
void RegisterNode(const std::string& name, Factory factory);
void RegisterNode(const std::string& name, std::shared_ptr<JsObjectBase> node);
std::shared_ptr<JsObjectBase> GetNode(const std::string& name, const RpcEnv& env);
std::vector<std::string> NodeNames() const;
JsValue Describe(int handle);
```

- 构造即创建根节点，句柄固定为 `0`。
- `AddObject` 分配自增句柄并 `Attach` 注册表/句柄/`env`，返回句柄；失败返回 `-1`。
- `Describe(handle)` 返回：

  ```json
  {
    "handle": 3,
    "members": [
      {"name": "add", "kind": "function", "index": 0, "type": "int",
       "async": false, "writable": false,
       "params": [{"name": "a", "type": "int", "optional": false}]},
      {"name": "count", "kind": "property", "index": 1, "type": "int",
       "async": false, "writable": true, "params": []}
    ]
  }
  ```

- `RegisterNode` 保留已有实例句柄；重复注册不会替换已实例化的对象。
- 句柄槽位**不回收复用**，`handles` 只增不减（长时间频繁创建临时对象会增长，但对象本身会被释放）。

### 插件加载

```cpp
namespace jsapi::plugins {
JsRegistry::Factory LoadDll(const std::string& path, std::string* error);
}
```

`LoadLibrary` + `GetProcAddress("GetExtensionJsApiObj")`，成功后模块常驻。工厂返回裸指针，被包装为 `shared_ptr`。

```cpp
#define JSAPI_EXPORT_EXTENSION(CLASSNAME)                  \
  extern "C" __declspec(dllexport) ::jsapi::JsObjectBase*  \
  GetExtensionJsApiObj() { return new CLASSNAME(); }
```

---

## RpcHost（内部执行器）

```cpp
struct RpcRequest {
  int seq = 0;
  std::string op;            // describe / get / set / execute
  int handle = 0;
  int index = -1;
  bool async = false;
  std::vector<JsValue> args;
  RpcEnv env;
};

struct RpcResponse {
  int seq = 0;
  bool ok = true;
  JsValue value;
  std::string error;
};

static bool ParseRequest(const std::string& json, RpcRequest*, std::string*);
static bool ParseRequest(const JsValue& root, RpcRequest*, std::string*);
static std::string EncodeRequest(const RpcRequest&,
                                 const std::map<std::string, JsValue>& extras = {});
static std::string EncodeResponse(const RpcResponse&);
RpcResponse Process(const RpcRequest&);
```

`RpcHost` 不依赖 CEF，可脱离宿主单测（`test/reliability/` 即如此）。

处理顺序：

1. `GetObjectByHandle`，失败返回 `invalid object handle`
2. `describe` → 直接返回 describe 结果
3. 其余按 `index` 找 `JsMember`，找不到返回 `invalid member index`
4. `get` → getter；`set` → `CoerceValue` + setter；`execute` → 参数个数/类型校验 + 函数体

---

## 同步与异步语义

| 项 | 同步（默认） | 异步（`AsyncFunction`） |
| --- | --- | --- |
| JS 返回值 | 直接返回结果 | 原生 `Promise` |
| 传输 | 共享内存 + 命名事件 | `CefProcessMessage` |
| 渲染进程 | 阻塞等待 | 不阻塞，挂起在 `pending_[seq]` |
| 超时 | 默认 10 s，可按调用传 `timeout_ms`；超时**不取消**已开始的执行，迟到回包按 `seq` 丢弃 | **无超时**，浏览器不回包则 Promise 永久 pending |
| 失败 | `throw` 异常 | `Promise.reject` |
| 浏览器侧执行 | 该页面的会话线程，按成员线程亲和执行（默认 Worker；`ThreadAffinity::UI` 投递宿主 UI 线程），执行完再回包 | 宿主 UI 线程（`ThreadAffinity::UI` 原地执行），执行完再回包（异步只影响渲染侧） |

- 同步调用在**主线程**发起，会阻塞该 frame 的 JS 执行；不要在 UI 交互路径调用耗时接口。
- 同步调用按页面会话线程并行：A 页的耗时调用不会挡住 B 页；同一页面内仍是串行。
- 异步调用的插件函数仍在宿主 UI 线程串行执行，不要指望它带来并行。
- 单条请求/回包上限 **1 MB**（`kRequestCapacity` / `kReplyCapacity`），超限返回错误。

---

## 参数与返回值校验

### 入参强转（`CoerceValue`）

| 声明类型 | 接受的 JS 值 | 结果 |
| --- | --- | --- |
| `Bool` | bool / int / double | `AsBool()`（非 0 为真） |
| `Int` | int / double / bool | `AsInt()`（double 截断） |
| `Double` | int / double | `AsDouble()` |
| `String` | string | 原样 |
| `Array` | array | 原样 |
| `Object` | object | 原样 |
| `ObjectHandle` / `CallbackHandle` | 对应句柄 | 原样 |
| `Null` | 任意 | 变为 null |
| `Any` | 任意 | 原样 |

不匹配时报错，例如：

```
function 'add' arg 'a': expected int, got string
```

### 返回值

- 返回类型**不做运行时强转**，请返回与声明一致的 `JsValue`。
- 渲染进程转 V8 时：`Int → CreateInt`（32 位）、`Double → CreateDouble`、`String → CreateString`、`Array/Object` 递归转换、`ObjectHandle` 触发一次 describe 并建立新代理。
- `Undefined` / `Null` 参数会转成 `JsValue::Null()`；`Date` / `Promise` / `ArrayBuffer` 等类型不支持传参，会返回 `unsupported js value type for jsapi argument`。

---

## 事件语义

| 能力 | C++ | JS |
| --- | --- | --- |
| 登记 | `AddEventListener(event, cb, env)` | `addEventListener(event, fn)` |
| 移除 | `RemoveEventListener(event, cb)` | `removeEventListener(event, fn)` |
| 广播 | `BroadcastEvent(event, args)` | `dispatchEvent(event, args)` |
| 定向 | `DispatchEvent(event, target, args)` | `dispatchEvent(event, args, {browser, frame})` |
| 同步等待 | `DispatchEventSync(event, target, args, timeout_ms)` | 由插件函数内部调用，JS 侧无直接入口 |
| 排除自己 | — | `dispatchEvent(event, args, {excludeSelf: true})` |
| 查询 | `GetAllEventInfo()` | `getAllEventInfo()` |
| 失效清理 | `DiscardListeners(browser, keep_frame)` | 框架自动调用，页面无需处理 |

匹配规则（`EmitTo`）：

- `target.browser_id >= 0` 时只匹配同 browser 的监听；
- `target.frame_id` 非空时只匹配同 frame 的监听（frame id 取自当前 document，导航/刷新后会变）；
- `target` 为空（`BroadcastEvent`）时不筛选；
- `exclude` 命中的监听者跳过（按 `browser + frame` 全等比较）。

JS 回调签名固定为 `(eventName, ...args)`，`args` 由 `JsValue` 转 V8。

事件回调在渲染进程主线程执行；回调中再调用同步 RPC 是允许的（会再次阻塞主线程），同步嵌套上限 8 层。回调抛异常会被捕获并转成错误结果，不会穿透到 C++。

---

## 句柄与临时对象

- `describe(handle)` 对 `Transient` 对象会把引用计数 `+1`，对应 V8 代理销毁时发送 `jsapi.object.release`，`-1` 后归零才真正 `release`。
- 同一个句柄可以被多层代理引用（例如插件两次返回同一子对象），引用计数保证最后一个代理销毁才释放。
- 句柄 `0` 是根节点，永不释放；`Singleton` 的 `release` 会被忽略。
- `Extention.objectStats()` 可用于观察：

  ```js
  { handles: 42, transient_live: 5, release_refs: 5 }
  ```

- V8 的 GC 时机不确定，临时对象不会立即释放；需要确定性释放时请在插件里主动管理（例如提供 `dispose()` 或改用 `Singleton`）。

---

## 线程安全

| 组件 | 保护方式 | 说明 |
| --- | --- | --- |
| `JsRegistry` | `mutex_` | 句柄表、节点表、引用计数 |
| `JsObjectBase` 函数表 | `table_mutex_` | 懒构建缓存 |
| `JsObjectBase` 监听表 | `listener_mutex_` | 增删与派发快照 |
| 插件函数执行 | 每页面一条会话线程 | 同页串行、跨页并行（无全局锁） |
| 回包通道缓存 | `reply_mutex_` | 读取线程与会话线程并发 |

插件函数没有全局执行锁：同一页面内由会话线程串行，跨页面的同一节点实例可能并发调用。**跨页面共享状态的节点必须自行加锁**；也可以把该成员声明为 `ThreadAffinity::UI`，让它在宿主 UI 线程与其他 UI 成员串行执行。

临时对象释放投递到宿主 UI 线程（`SetMainThreadDispatcher`）；`ThreadAffinity::UI` 的成员同样在该线程执行。
