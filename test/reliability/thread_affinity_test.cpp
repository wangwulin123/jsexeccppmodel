#include <chrono>
#include <cstdint>
#include <future>
#include <memory>
#include <string>
#include <thread>

#include "jsapi/js_registry.h"
#include "jsapi_rpc/jsapi_rpc.h"
#include "support/test_fixtures.h"
#include "support/test_harness.h"

namespace {

test::CoreFixture& Fixture() {
  static test::CoreFixture fixture;
  return fixture;
}

}  // namespace

TEST(reliability, thread_affinity) {
  test::CoreFixture& f = Fixture();
  f.registry.RegisterNode("affinity", std::make_shared<test::AffinityTestNode>());
  const int affinity_handle = test::GetNodeHandle(&f.rpc, "affinity");
  test::Check(affinity_handle > 0, "affinity node handle");
  if (affinity_handle <= 0) {
    return;
  }

  const jsapi::RpcResponse desc = test::Describe(&f.rpc, affinity_handle);
  const int worker_fn = test::FindMemberIndex(desc.value, "workerFn");
  const int ui_fn = test::FindMemberIndex(desc.value, "uiFn");
  const int worker_prop = test::FindMemberIndex(desc.value, "workerProp");
  const int ui_prop = test::FindMemberIndex(desc.value, "uiProp");
  test::Check(worker_fn >= 0 && ui_fn >= 0 && worker_prop >= 0 &&
                  ui_prop >= 0,
              "affinity node members resolved", desc.error);

  const DWORD caller_tid = ::GetCurrentThreadId();
  DWORD dispatch_tid = 0;
  int dispatches = 0;
  // 模拟宿主：把函数体阻塞切到另一条线程执行。
  jsapi::RpcHost::UiExecutor ui_executor =
      [&](const std::function<void()>& body) {
        ++dispatches;
        std::thread worker([&] {
          dispatch_tid = ::GetCurrentThreadId();
          body();
        });
        worker.join();
      };

  auto process = [&](const std::string& op, int index,
                     const jsapi::RpcHost::UiExecutor& executor) {
    jsapi::RpcRequest request;
    request.seq = 11;
    request.op = op;
    request.handle = affinity_handle;
    request.index = index;
    return f.rpc.Process(request, executor);
  };

  const jsapi::RpcResponse worker_call =
      process("execute", worker_fn, ui_executor);
  test::Check(worker_call.ok &&
                  worker_call.value.AsInt() ==
                      static_cast<std::int64_t>(caller_tid) &&
                  dispatches == 0,
              "worker member executes on the calling thread", worker_call.error);

  const jsapi::RpcResponse ui_call = process("execute", ui_fn, ui_executor);
  test::Check(ui_call.ok && dispatches == 1 &&
                  ui_call.value.AsInt() ==
                      static_cast<std::int64_t>(dispatch_tid) &&
                  dispatch_tid != caller_tid,
              "ui member executes on the dispatcher thread", ui_call.error);

  const jsapi::RpcResponse worker_get =
      process("get", worker_prop, ui_executor);
  test::Check(worker_get.ok &&
                  worker_get.value.AsInt() ==
                      static_cast<std::int64_t>(caller_tid) &&
                  dispatches == 1,
              "worker property getter stays on the calling thread",
              worker_get.error);

  const jsapi::RpcResponse ui_get = process("get", ui_prop, ui_executor);
  test::Check(ui_get.ok && dispatches == 2 &&
                  ui_get.value.AsInt() ==
                      static_cast<std::int64_t>(dispatch_tid),
              "ui property getter executes on the dispatcher thread",
              ui_get.error);

  // 无 UI 宿主（单测 / 未设置 dispatcher）：UI 成员就地执行，不阻塞。
  const jsapi::RpcResponse ui_inline = process("execute", ui_fn, nullptr);
  test::Check(ui_inline.ok && dispatches == 2 &&
                  ui_inline.value.AsInt() ==
                      static_cast<std::int64_t>(caller_tid),
              "ui member falls back to the calling thread without a dispatcher",
              ui_inline.error);
}

TEST(reliability, thread_affinity_without_ui_executor) {
  test::CoreFixture& f = Fixture();
  f.registry.RegisterNode("affinity", std::make_shared<test::AffinityTestNode>());
  const int affinity_handle = test::GetNodeHandle(&f.rpc, "affinity");
  if (affinity_handle <= 0) {
    test::Check(false, "affinity worker node handle");
    return;
  }
  const jsapi::RpcResponse desc = test::Describe(&f.rpc, affinity_handle);
  const int ui_fn = test::FindMemberIndex(desc.value, "uiFn");

  // 从 worker 线程调用 UI 亲和成员，且宿主没有配置 UI 执行器：
  // 必须明确降级到调用线程，并且能在超时预算内返回（不能死锁）。
  std::promise<std::int64_t> worker_tid_promise;
  std::future<std::int64_t> worker_tid = worker_tid_promise.get_future();
  std::future<jsapi::RpcResponse> future = std::async(
      std::launch::async, [&]() {
        worker_tid_promise.set_value(
            static_cast<std::int64_t>(::GetCurrentThreadId()));
        jsapi::RpcRequest request;
        request.seq = 12;
        request.op = "execute";
        request.handle = affinity_handle;
        request.index = ui_fn;
        return f.rpc.Process(request, nullptr);
      });

  const bool finished =
      future.wait_for(std::chrono::milliseconds(3000)) ==
      std::future_status::ready;
  test::Check(finished, "ui member called from a worker without a dispatcher "
                        "completes within the timeout");
  if (!finished) {
    return;
  }
  const jsapi::RpcResponse response = future.get();
  test::Check(response.ok && response.value.AsInt() == worker_tid.get(),
              "ui member degrades to the worker thread instead of blocking",
              response.error);
}
