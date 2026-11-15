#include "jsapi/js_registry.h"

#include <QApplication>
#include <QFile>
#include <QMainWindow>
#include <QMessageBox>
#include <QMetaType>
#include <QSettings>
#include <QStatusBar>
#include <QString>
#include <QThread>
#include <QVariant>
#include <QWidget>

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <vector>

#include <windows.h>

namespace {

// 注册表固定读写 HKEY_CURRENT_USER\Software\jsapi_demo：
// QSettings 的 NativeFormat 在 Windows 上直接映射到注册表。
const char kRegistryOrg[] = "HKEY_CURRENT_USER\\Software\\jsapi_demo";

QVariant ToQVariant(const jsapi::JsValue& value) {
  switch (value.type()) {
    case jsapi::JsType::Bool:
      return QVariant(value.AsBool());
    case jsapi::JsType::Int:
      return QVariant(static_cast<qlonglong>(value.AsInt()));
    case jsapi::JsType::Double:
      return QVariant(value.AsDouble());
    case jsapi::JsType::String:
      return QVariant(QString::fromStdString(value.AsString()));
    case jsapi::JsType::Array:
    case jsapi::JsType::Object:
      // 复杂类型按 JSON 文本落库，读回时是字符串。
      return QVariant(QString::fromStdString(value.ToJson()));
    case jsapi::JsType::Null:
      return QVariant(QString());
    default:
      return QVariant();
  }
}

jsapi::JsValue ToJsValue(const QVariant& value) {
  if (value.isNull()) {
    return jsapi::JsValue::Null();
  }
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
  switch (value.typeId()) {
#else
  switch (value.userType()) {
#endif
    case QMetaType::Bool:
      return jsapi::JsValue::Bool(value.toBool());
    case QMetaType::Int:
    case QMetaType::UInt:
    case QMetaType::LongLong:
    case QMetaType::ULongLong:
      return jsapi::JsValue::Int(value.toLongLong());
    case QMetaType::Double:
    case QMetaType::Float:
      return jsapi::JsValue::Double(value.toDouble());
    default:
      return jsapi::JsValue::String(value.toString().toStdString());
  }
}

// Qt 控件只能在 GUI 线程操作。宿主通过 SetMainThreadDispatcher 把 JSAPI
// 的执行点固定在 GUI 线程，所以这里可以直接找主窗口并改控件。
QMainWindow* FindHostWindow() {
  if (!qApp) {
    return nullptr;
  }
  for (QWidget* widget : QApplication::topLevelWidgets()) {
    auto* window = qobject_cast<QMainWindow*>(widget);
    if (window && window->isVisible()) {
      return window;
    }
  }
  return nullptr;
}

class DemoChildNode final : public jsapi::JsObjectBase {
 public:
  // 子对象按临时对象处理：代理回收后由注册表释放句柄。
  JSAPI_NODE_LIFE_TYPE(jsapi::LifeType::Transient);

  DemoChildNode() : id_(++next_id_) {}

 protected:
  void DeclareMembers(jsapi::JsTableBuilder& table) const override {
    table.Function("ping", jsapi::JsType::String,
                   {{"text", jsapi::JsType::String}},
                   [](jsapi::JsObjectBase*, jsapi::JsCallContext& ctx) {
                     ctx.Return(jsapi::JsValue::String("pong: " +
                                                       ctx.args()[0].AsString()));
                   });

    table.Property("label", jsapi::JsType::String,
                   [](jsapi::JsObjectBase*) {
                     return jsapi::JsValue::String("demo-child");
                   });

    // 每个临时节点实例的编号：用于验证同一句柄/不同句柄的代理身份。
    table.Property("id", jsapi::JsType::Int,
                   [](jsapi::JsObjectBase* self) {
                     return jsapi::JsValue::Int(
                         static_cast<DemoChildNode*>(self)->id_);
                   });

    // 可写状态：用于验证多个代理之间互不共享实例。
    table.Property(
        "note", jsapi::JsType::String,
        [](jsapi::JsObjectBase* self) {
          return jsapi::JsValue::String(
              static_cast<DemoChildNode*>(self)->note_);
        },
        [](jsapi::JsObjectBase* self, const jsapi::JsValue& value,
           std::string* /*error*/) {
          static_cast<DemoChildNode*>(self)->note_ = value.AsString();
          return true;
        });
  }

 private:
  static std::atomic<int> next_id_;
  int id_ = 0;
  std::string note_;
};

std::atomic<int> DemoChildNode::next_id_{0};

// 跨进程性能测试用的单调时钟（QPC 全系统一致，两个进程可直接比较）。
double NowMs() {
  static const double kFreq = [] {
    LARGE_INTEGER freq = {};
    ::QueryPerformanceFrequency(&freq);
    return static_cast<double>(freq.QuadPart);
  }();
  LARGE_INTEGER counter = {};
  ::QueryPerformanceCounter(&counter);
  return static_cast<double>(counter.QuadPart) * 1000.0 / kFreq;
}

// 只服务于 jsapi_perf.html 的空接口节点：函数体不含任何业务逻辑。
class BenchNode final : public jsapi::JsObjectBase {
 public:
  JSAPI_NODE_LIFE_TYPE(jsapi::LifeType::Singleton);

 protected:
  void DeclareMembers(jsapi::JsTableBuilder& table) const override {
    // 同步空接口：trace=true 时返回 C++ 进入/离开时刻，供单程估算使用。
    table.Function(
        "ping", jsapi::JsType::Any,
        {{"trace", jsapi::JsType::Bool, true}},
        [](jsapi::JsObjectBase*, jsapi::JsCallContext& ctx) {
          const double t0 = NowMs();
          if (ctx.args().size() > 0 && ctx.args()[0].AsBool()) {
            jsapi::JsValue out = jsapi::JsValue::Object();
            out.Set("t0", jsapi::JsValue::Double(t0));
            out.Set("t1", jsapi::JsValue::Double(NowMs()));
            ctx.Return(out);
            return;
          }
          ctx.Return(jsapi::JsValue::Int(1));
        });

    // 异步空接口：渲染进程立刻拿到 Promise。
    table.AsyncFunction(
        "pingAsync", jsapi::JsType::Any,
        {{"trace", jsapi::JsType::Bool, true}},
        [](jsapi::JsObjectBase*, jsapi::JsCallContext& ctx) {
          const double t0 = NowMs();
          if (ctx.args().size() > 0 && ctx.args()[0].AsBool()) {
            jsapi::JsValue out = jsapi::JsValue::Object();
            out.Set("t0", jsapi::JsValue::Double(t0));
            out.Set("t1", jsapi::JsValue::Double(NowMs()));
            ctx.Return(out);
            return;
          }
          ctx.Return(jsapi::JsValue::Int(1));
        });

    // 时钟对齐：C++ 进入时刻 + 返回前时刻，用于估算 JS/C++ 的时钟偏移。
    table.Function("clock", jsapi::JsType::Object, {},
                   [](jsapi::JsObjectBase*, jsapi::JsCallContext& ctx) {
                     const double t0 = NowMs();
                     jsapi::JsValue out = jsapi::JsValue::Object();
                     out.Set("t0", jsapi::JsValue::Double(t0));
                     out.Set("t1", jsapi::JsValue::Double(NowMs()));
                     ctx.Return(out);
                   });

    // 连续发布 count 条事件，并记录每条的发布时间（用于反向单程估算）。
    table.Function(
        "emit", jsapi::JsType::Int,
        {{"count", jsapi::JsType::Int},
         {"name", jsapi::JsType::String, true}},
        [](jsapi::JsObjectBase* self, jsapi::JsCallContext& ctx) {
          auto* node = static_cast<BenchNode*>(self);
          const int count = static_cast<int>(ctx.args()[0].AsInt());
          const std::string name =
              ctx.args().size() > 1 ? ctx.args()[1].AsString() : "bench";
          node->publishes_.clear();
          node->publishes_.reserve(static_cast<std::size_t>(count));
          for (int i = 0; i < count; ++i) {
            node->publishes_.push_back(NowMs());
            self->BroadcastEvent(name, {jsapi::JsValue::Int(i)});
          }
          ctx.Return(jsapi::JsValue::Int(count));
        });

    table.Function("publishes", jsapi::JsType::Array, {},
                   [](jsapi::JsObjectBase* self, jsapi::JsCallContext& ctx) {
                     const auto& times =
                         static_cast<BenchNode*>(self)->publishes_;
                     std::vector<jsapi::JsValue> out;
                     out.reserve(times.size());
                     for (double t : times) {
                       out.push_back(jsapi::JsValue::Double(t));
                     }
                     ctx.Return(jsapi::JsValue::Array(std::move(out)));
                   });

    // 测试结果回传：打到 stderr，外部脚本据此生成报告。
    table.Function("report", jsapi::JsType::Bool,
                   {{"text", jsapi::JsType::String}},
                   [](jsapi::JsObjectBase*, jsapi::JsCallContext& ctx) {
                     std::fprintf(stderr, "JSAPIBENCH %s\n",
                                  ctx.args()[0].AsString().c_str());
                     std::fflush(stderr);
                     ctx.Return(jsapi::JsValue::Bool(true));
                   });
  }

 private:
  std::vector<double> publishes_;
};

// 动态插件示例节点。
// 插件只需要声明函数表：函数调用、属性读写、异步函数、事件广播都在这里定义。
class DemoNode final : public jsapi::JsObjectBase {
 public:
  // 不写时默认就是 Singleton，这里显式写出来，作为插件侧声明生命周期的示例。
  JSAPI_NODE_LIFE_TYPE(jsapi::LifeType::Singleton);

  DemoNode() = default;

 protected:
  void DeclareMembers(jsapi::JsTableBuilder& table) const override {
    table.Function(
        "add", jsapi::JsType::Int,
        {{"a", jsapi::JsType::Int}, {"b", jsapi::JsType::Int}},
        [](jsapi::JsObjectBase*, jsapi::JsCallContext& ctx) {
          ctx.Return(jsapi::JsValue::Int(ctx.args()[0].AsInt() +
                                         ctx.args()[1].AsInt()));
        });

    table.Function(
        "concat", jsapi::JsType::String,
        {{"a", jsapi::JsType::String}, {"b", jsapi::JsType::String}},
        [](jsapi::JsObjectBase*, jsapi::JsCallContext& ctx) {
          ctx.Return(jsapi::JsValue::String(ctx.args()[0].AsString() +
                                            ctx.args()[1].AsString()));
        });

    // 异步声明：渲染进程立即返回 Promise，浏览器进程通过异步消息回包。
    table.AsyncFunction(
        "sumAsync", jsapi::JsType::Int,
        {{"a", jsapi::JsType::Int}, {"b", jsapi::JsType::Int}},
        [](jsapi::JsObjectBase*, jsapi::JsCallContext& ctx) {
          ctx.Return(jsapi::JsValue::Int(ctx.args()[0].AsInt() +
                                         ctx.args()[1].AsInt()));
        });

    // 演示异常通路：浏览器侧的 error 会被还原成 JS 异常。
    table.Function("fail", jsapi::JsType::Null, {},
                   [](jsapi::JsObjectBase*, jsapi::JsCallContext& ctx) {
                     ctx.Fail("this is an intentional jsapi failure");
                   });

    // 主动向本节点的所有监听者广播事件，事件经过浏览器进程再回到渲染进程。
    table.Function(
        "emit", jsapi::JsType::Int,
        {{"name", jsapi::JsType::String}, {"args", jsapi::JsType::Array, true}},
        [](jsapi::JsObjectBase* self, jsapi::JsCallContext& ctx) {
          std::vector<jsapi::JsValue> args;
          if (ctx.args().size() > 1 &&
              ctx.args()[1].type() == jsapi::JsType::Array) {
            args = ctx.args()[1].AsArray();
          }
          ctx.Return(jsapi::JsValue::Int(
              self->BroadcastEvent(ctx.args()[0].AsString(), args)));
        });

    // 返回另一个节点句柄：渲染进程会为它单独建立一层 V8 代理。
    // 子节点是临时对象，每次调用都新建一个，句柄随代理回收释放。
    table.Function("child", jsapi::JsType::ObjectHandle, {},
                   [](jsapi::JsObjectBase*, jsapi::JsCallContext& ctx) {
                     auto child = std::make_shared<DemoChildNode>();
                     ctx.Return(jsapi::JsValue::ObjectHandle(
                         ctx.registry()->AddObject(child, ctx.env())));
                   });

    // 性能测试节点：空接口 + 空事件，用来测纯桥接开销。
    table.Function("bench", jsapi::JsType::ObjectHandle, {},
                   [](jsapi::JsObjectBase*, jsapi::JsCallContext& ctx) {
                     auto node = std::make_shared<BenchNode>();
                     ctx.Return(jsapi::JsValue::ObjectHandle(
                         ctx.registry()->AddObject(node, ctx.env())));
                   });

    table.Property("version", jsapi::JsType::String,
                   [](jsapi::JsObjectBase*) {
                     return jsapi::JsValue::String("demo-1.2.0");
                   });

    table.Property(
        "count", jsapi::JsType::Int,
        [](jsapi::JsObjectBase* self) {
          return jsapi::JsValue::Int(static_cast<DemoNode*>(self)->count_);
        },
        [](jsapi::JsObjectBase* self, const jsapi::JsValue& value,
           std::string* error) {
          const std::int64_t next = value.AsInt();
          if (next < 0) {
            if (error) *error = "count must be >= 0";
            return false;
          }
          static_cast<DemoNode*>(self)->count_ = next;
          return true;
        });

    // ---- Qt 实现：注册表读写 + 异步读文件 ----
    table.Function(
        "setRegValue", jsapi::JsType::Bool,
        {{"name", jsapi::JsType::String}, {"value", jsapi::JsType::Any}},
        [](jsapi::JsObjectBase*, jsapi::JsCallContext& ctx) {
          const QVariant value = ToQVariant(ctx.args()[1]);
          if (!value.isValid()) {
            ctx.Fail("setRegValue: unsupported value type");
            return;
          }
          QSettings settings(QString::fromUtf8(kRegistryOrg),
                             QSettings::NativeFormat);
          settings.setValue(QString::fromStdString(ctx.args()[0].AsString()),
                            value);
          settings.sync();
          ctx.Return(
              jsapi::JsValue::Bool(settings.status() == QSettings::NoError));
        });

    table.Function(
        "getRegValue", jsapi::JsType::Any,
        {{"name", jsapi::JsType::String}, {"default", jsapi::JsType::Any}},
        [](jsapi::JsObjectBase*, jsapi::JsCallContext& ctx) {
          QSettings settings(QString::fromUtf8(kRegistryOrg),
                             QSettings::NativeFormat);
          const QVariant fallback = ToQVariant(ctx.args()[1]);
          const QVariant stored =
              settings.value(QString::fromStdString(ctx.args()[0].AsString()),
                             fallback);
          ctx.Return(ToJsValue(stored));
        });

    // 异步读文件：渲染进程立即拿到 Promise，文件内容通过异步回调返回。
    table.AsyncFunction(
        "readFile", jsapi::JsType::String, {{"path", jsapi::JsType::String}},
        [](jsapi::JsObjectBase*, jsapi::JsCallContext& ctx) {
          const QString path = QString::fromStdString(ctx.args()[0].AsString());
          QFile file(path);
          if (!file.open(QIODevice::ReadOnly)) {
            ctx.Fail("readFile failed: " + path.toStdString() + " (" +
                     file.errorString().toStdString() + ")");
            return;
          }
          const QByteArray content = file.readAll();
          ctx.Return(jsapi::JsValue::String(QString::fromUtf8(
              content.constData(), content.size()).toStdString()));
        });

    // 默认 Worker 亲和的对照成员：返回执行线程信息，和 uiInfo 对比即可看出
    // Worker 成员跑在所属页面的会话线程上，而不是 Qt GUI 线程。
    table.Function("threadInfo", jsapi::JsType::Object, {},
                   [](jsapi::JsObjectBase*, jsapi::JsCallContext& ctx) {
                     jsapi::JsValue info = jsapi::JsValue::Object();
                     info.Set("on_gui_thread",
                              jsapi::JsValue::Bool(
                                  qApp && QThread::currentThread() ==
                                              qApp->thread()));
                     info.Set("call_thread",
                              jsapi::JsValue::Int(static_cast<std::int64_t>(
                                  reinterpret_cast<std::uintptr_t>(
                                      QThread::currentThreadId()))));
                     ctx.Return(std::move(info));
                   });

    // 亲和性反例：本该操作 Qt 控件，却沿用默认 Worker 亲和。
    // 真实后果：Qt 控件只能在 GUI 线程操作，跨线程调用属于未定义行为——可能静默
    // 不生效、绘制异常，甚至崩溃。这里先检测线程，不在 GUI 线程时拒绝真实操作并
    // 返回诊断，避免把演示程序弄崩；把同一操作声明为 UI 亲和（touchUiAsUi）即可
    // 看到正确结果。
    table.Function(
        "touchUiAsWorker", jsapi::JsType::Object,
        {{"title", jsapi::JsType::String}},
        [](jsapi::JsObjectBase*, jsapi::JsCallContext& ctx) {
          QMainWindow* window = FindHostWindow();
          const bool on_gui =
              qApp && QThread::currentThread() == qApp->thread();
          jsapi::JsValue out = jsapi::JsValue::Object();
          out.Set("declared_affinity", jsapi::JsValue::String("Worker"));
          out.Set("on_gui_thread", jsapi::JsValue::Bool(on_gui));
          out.Set("window_found", jsapi::JsValue::Bool(window != nullptr));
          out.Set("call_thread",
                  jsapi::JsValue::Int(static_cast<std::int64_t>(
                      reinterpret_cast<std::uintptr_t>(
                          QThread::currentThreadId()))));
          if (on_gui && window) {
            // 只有亲和配置出错（Worker 声明却跑在 GUI 线程）才会走到这里。
            window->setWindowTitle(
                QString::fromStdString(ctx.args()[0].AsString()));
            out.Set("applied", jsapi::JsValue::Bool(true));
            out.Set("window_title",
                    jsapi::JsValue::String(window->windowTitle().toStdString()));
            out.Set("note", jsapi::JsValue::String(
                                "Worker 成员意外在 GUI 线程执行，操作已生效"));
          } else {
            out.Set("applied", jsapi::JsValue::Bool(false));
            out.Set("note", jsapi::JsValue::String(
                                "Worker 亲和不在 GUI 线程：Qt 控件跨线程操作属未定义"
                                "行为，已拒绝真实操作；该操作应声明 ThreadAffinity::UI"));
          }
          ctx.Return(std::move(out));
        });

    // 正例：同一个"改主窗口标题"的操作声明 UI 亲和，函数体被切到 Qt GUI 线程后生效。
    table.Function(
        "touchUiAsUi", jsapi::JsType::Object,
        {{"title", jsapi::JsType::String}},
        [](jsapi::JsObjectBase*, jsapi::JsCallContext& ctx) {
          QMainWindow* window = FindHostWindow();
          const bool on_gui =
              qApp && QThread::currentThread() == qApp->thread();
          jsapi::JsValue out = jsapi::JsValue::Object();
          out.Set("declared_affinity", jsapi::JsValue::String("UI"));
          out.Set("on_gui_thread", jsapi::JsValue::Bool(on_gui));
          out.Set("window_found", jsapi::JsValue::Bool(window != nullptr));
          out.Set("call_thread",
                  jsapi::JsValue::Int(static_cast<std::int64_t>(
                      reinterpret_cast<std::uintptr_t>(
                          QThread::currentThreadId()))));
          if (window) {
            window->setWindowTitle(
                QString::fromStdString(ctx.args()[0].AsString()));
            out.Set("applied", jsapi::JsValue::Bool(true));
            out.Set("window_title",
                    jsapi::JsValue::String(window->windowTitle().toStdString()));
          } else {
            out.Set("applied", jsapi::JsValue::Bool(false));
            out.Set("note", jsapi::JsValue::String("host window not found"));
          }
          ctx.Return(std::move(out));
        },
        /*async=*/false, jsapi::ThreadAffinity::UI);

    // 危险演示：故意把"操作 Qt 控件"声明为默认 Worker，且不做任何线程防护。
    // 这就是真实的跨线程操作 GUI：Qt 会输出跨线程告警，行为未定义——可能只是界面
    // 错乱，也可能直接崩掉宿主进程。崩溃属于该示例的预期结果。
    table.Function("unsafeUiAsWorker", jsapi::JsType::Null, {},
                   [](jsapi::JsObjectBase*, jsapi::JsCallContext& ctx) {
                     QMainWindow* window = FindHostWindow();
                     if (!window) {
                       ctx.Fail("unsafeUiAsWorker: host window not found");
                       return;
                     }
                     // 1) 跨线程写状态栏：showMessage(…, timeout) 内部会 startTimer。
                     window->statusBar()->showMessage(
                         QStringLiteral("Worker 线程正在操作状态栏…"), 5000);
                     // 2) 跨线程改主窗口标题。
                     window->setWindowTitle(QStringLiteral("标题由 Worker 线程修改"));
                     // 3) 跨线程创建并显示原生控件。
                     auto* box = new QMessageBox(window);
                     box->setAttribute(Qt::WA_DeleteOnClose);
                     box->setIcon(QMessageBox::Warning);
                     box->setWindowTitle(QStringLiteral("跨线程弹窗"));
                     box->setText(QStringLiteral(
                         "这个对话框由会话线程（非 GUI 线程）创建并显示，属于未定义行为。"));
                     box->setStandardButtons(QMessageBox::Ok);
                     box->show();
                     ctx.Return(jsapi::JsValue::Null());
                   });

    // ---- 原生界面：这几个成员声明 UI 亲和，函数体会被切到 Qt GUI 线程执行 ----
    // on_gui_thread 用来确认亲和生效（UI 成员必须为 true）。对比 Worker 成员可以看
    // 执行线程不同：Worker 调用跑在所属页面的会话线程上，不在 Qt GUI 线程。
    table.Function("uiInfo", jsapi::JsType::Object, {},
                   [](jsapi::JsObjectBase*, jsapi::JsCallContext& ctx) {
                     QMainWindow* window = FindHostWindow();
                     jsapi::JsValue info = jsapi::JsValue::Object();
                     info.Set("on_gui_thread",
                              jsapi::JsValue::Bool(
                                  qApp && QThread::currentThread() ==
                                              qApp->thread()));
                     info.Set("call_thread",
                              jsapi::JsValue::Int(static_cast<std::int64_t>(
                                  reinterpret_cast<std::uintptr_t>(
                                      QThread::currentThreadId()))));
                     info.Set("window_title",
                              jsapi::JsValue::String(
                                  window ? window->windowTitle().toStdString()
                                         : std::string()));
                     ctx.Return(std::move(info));
                   },
                   /*async=*/false, jsapi::ThreadAffinity::UI);

    table.Function(
        "setWindowTitle", jsapi::JsType::Bool,
        {{"title", jsapi::JsType::String}},
        [](jsapi::JsObjectBase*, jsapi::JsCallContext& ctx) {
          QMainWindow* window = FindHostWindow();
          if (!window) {
            ctx.Fail("setWindowTitle: host window not found");
            return;
          }
          window->setWindowTitle(
              QString::fromStdString(ctx.args()[0].AsString()));
          ctx.Return(jsapi::JsValue::Bool(true));
        },
        /*async=*/false, jsapi::ThreadAffinity::UI);

    table.Function(
        "showStatus", jsapi::JsType::Bool,
        {{"text", jsapi::JsType::String}},
        [](jsapi::JsObjectBase*, jsapi::JsCallContext& ctx) {
          QMainWindow* window = FindHostWindow();
          if (!window) {
            ctx.Fail("showStatus: host window not found");
            return;
          }
          window->statusBar()->showMessage(
              QString::fromStdString(ctx.args()[0].AsString()), 5000);
          ctx.Return(jsapi::JsValue::Bool(true));
        },
        /*async=*/false, jsapi::ThreadAffinity::UI);

    // 非模态弹窗：open() 立即返回，不会把渲染进程的同步调用卡在用户点击上。
    table.Function(
        "openDialog", jsapi::JsType::Bool,
        {{"title", jsapi::JsType::String}, {"text", jsapi::JsType::String}},
        [](jsapi::JsObjectBase*, jsapi::JsCallContext& ctx) {
          QMainWindow* window = FindHostWindow();
          if (!window) {
            ctx.Fail("openDialog: host window not found");
            return;
          }
          auto* box = new QMessageBox(window);
          box->setAttribute(Qt::WA_DeleteOnClose);
          box->setIcon(QMessageBox::Information);
          box->setWindowTitle(
              QString::fromStdString(ctx.args()[0].AsString()));
          box->setText(QString::fromStdString(ctx.args()[1].AsString()));
          box->setStandardButtons(QMessageBox::Ok);
          box->open();
          ctx.Return(jsapi::JsValue::Bool(true));
        },
        /*async=*/false, jsapi::ThreadAffinity::UI);
  }

 private:
  std::int64_t count_ = 10;
};

}  // namespace

// 导出工厂与类内声明的生命周期，宿主 LoadDll 后直接注册。
JSAPI_EXPORT_EXTENSION(DemoNode);
