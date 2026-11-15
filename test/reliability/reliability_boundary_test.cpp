#include <windows.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "jsapi/js_registry.h"
#include "jsapi_channel/jsapi_channel.h"
#include "jsapi_channel/jsapi_sync_channel.h"
#include "jsapi_rpc/jsapi_rpc.h"
#include "support/test_fixtures.h"
#include "support/test_harness.h"

namespace {

test::CoreFixture& Fixture() {
  static test::CoreFixture fixture;
  return fixture;
}

}  // namespace

TEST(reliability, boundary_exceptions) {
  test::CoreFixture& f = Fixture();

  // 插件抛 C++ 异常必须转成 RPC 错误，且异常后节点仍可正常调用。
  f.registry.RegisterNode("throwing", std::make_shared<test::ThrowingTestNode>());
  const int throw_handle = test::GetNodeHandle(&f.rpc, "throwing");
  test::Check(throw_handle > 0, "throwing node handle");
  if (throw_handle <= 0) {
    return;
  }

  const jsapi::RpcResponse desc = test::Describe(&f.rpc, throw_handle);
  const int throw_std = test::FindMemberIndex(desc.value, "throwStd");
  const int throw_non_std = test::FindMemberIndex(desc.value, "throwNonStd");
  const int ok_fn = test::FindMemberIndex(desc.value, "okFn");
  const int throw_prop = test::FindMemberIndex(desc.value, "throwProp");
  const int ok_prop = test::FindMemberIndex(desc.value, "okProp");
  test::Check(throw_std >= 0 && throw_non_std >= 0 && ok_fn >= 0 &&
                  throw_prop >= 0 && ok_prop >= 0,
              "throwing node members resolved", desc.error);

  const jsapi::RpcResponse std_err =
      test::Execute(&f.rpc, throw_handle, throw_std, {});
  test::Check(!std_err.ok &&
                  std_err.error.find("throwStd") != std::string::npos &&
                  std_err.error.find("std failure") != std::string::npos,
              "std::exception in function becomes RPC error", std_err.error);

  const jsapi::RpcResponse non_std_err =
      test::Execute(&f.rpc, throw_handle, throw_non_std, {});
  test::Check(!non_std_err.ok &&
                  non_std_err.error.find("unknown exception") !=
                      std::string::npos,
              "non-std exception becomes RPC error", non_std_err.error);

  const jsapi::RpcResponse get_err =
      test::GetProperty(&f.rpc, throw_handle, throw_prop);
  test::Check(!get_err.ok &&
                  get_err.error.find("getter threw") != std::string::npos,
              "property getter exception becomes RPC error", get_err.error);

  const jsapi::RpcResponse after = test::Execute(&f.rpc, throw_handle, ok_fn, {});
  test::Check(after.ok && after.value.AsInt() == 7,
              "node still serves requests after an exception", after.error);
  const jsapi::RpcResponse after_get =
      test::GetProperty(&f.rpc, throw_handle, ok_prop);
  test::Check(after_get.ok && after_get.value.AsInt() == 9,
              "property getter works after an exception", after_get.error);

  // 异常后的重复调用与普通 host 调用都必须继续可用。
  const jsapi::RpcResponse again = test::Execute(&f.rpc, throw_handle, ok_fn, {});
  test::Check(again.ok && again.value.AsInt() == 7,
              "throwing node keeps serving repeated calls", again.error);
  const jsapi::RpcResponse host_ping =
      test::GetProperty(&f.rpc, f.host_handle,
                        test::FindMemberIndex(
                            test::Describe(&f.rpc, f.host_handle).value, "app"));
  test::Check(host_ping.ok && host_ping.value.AsString() == "jsapi_demo_qt_host",
              "host keeps serving after a node threw", host_ping.error);
}

TEST(reliability, boundary_channels) {
  using jsapi_bridge::EventRingReader;
  using jsapi_bridge::EventRingWriter;
  using jsapi_bridge::kMessageRpc;
  using jsapi_bridge::ReplyReader;
  using jsapi_bridge::ReplyWriter;
  using jsapi_bridge::RequestReader;
  using jsapi_bridge::RequestWriter;

  const unsigned long pid = 9000 + (::GetCurrentProcessId() % 500);
  std::string error;
  RequestWriter request_writer(pid);
  RequestReader request_reader(pid);
  test::Check(request_writer.Ensure(&error) && request_reader.Ensure(&error),
              "reliability channels open", error);

  const std::string huge(jsapi_bridge::kRequestCapacity + 1, 'x');
  test::Check(!request_writer.Write(kMessageRpc, 1, 0, huge, &error) &&
                  error.find("exceeds") != std::string::npos,
              "oversized request is rejected explicitly", error);

  jsapi_bridge::MessageKind kind = kMessageRpc;
  long seq = 0;
  long invoke = 0;
  std::string payload;
  error.clear();
  test::Check(request_writer.Write(kMessageRpc, 2, 0, "small", &error) &&
                  request_reader.TryRead(&kind, &seq, &invoke, &payload) &&
                  seq == 2 && payload == "small",
              "request channel still usable after rejection", error);

  // 崩溃残留回包：Reset 后同 seq 的新回包必须能被正确消费。
  ReplyWriter reply_writer(pid);
  ReplyReader reply_reader(pid);
  reply_writer.Ensure(&error);
  reply_reader.Ensure(&error);
  reply_writer.Write(5, "stale-from-dead-renderer");
  reply_reader.Reset();
  std::string reply;
  test::Check(!reply_reader.TryTake(5, nullptr, &reply),
              "Reset drops the reply left by a crashed renderer");
  test::Check(reply_writer.Write(5, "fresh-reply"), "reply write after reset");
  test::Check(reply_reader.TryTake(5, nullptr, &reply) && reply == "fresh-reply",
              "same seq reply is consumable after Reset", reply);

  // 崩溃残留事件：旧页面的异步事件不得在新页面被派发。
  EventRingWriter event_writer(pid);
  EventRingReader event_reader(pid);
  event_writer.Ensure(&error);
  event_reader.Ensure(&error);
  event_writer.Write(0, "orphan-1");
  event_writer.Write(0, "orphan-2");
  event_reader.DiscardPending();
  long event_invoke = -1;
  std::string event_payload;
  test::Check(!event_reader.TryRead(&event_invoke, &event_payload),
              "DiscardPending drops events from a dead renderer");
  test::Check(event_writer.Write(7, "fresh-event"),
              "event write after discard");
  test::Check(event_reader.TryRead(&event_invoke, &event_payload) &&
                  event_invoke == 7 && event_payload == "fresh-event",
              "event ring works after DiscardPending");
  event_writer.Write(0, "orphan-3");
  event_writer.Clear();
  test::Check(!event_reader.TryRead(&event_invoke, &event_payload),
              "EventRingWriter::Clear drops pending events");
}

TEST(reliability, boundary_event_recovery) {
  test::CoreFixture& f = Fixture();
  auto host_object = f.registry.GetObjectByHandle(f.host_handle);
  test::Check(host_object != nullptr, "resolve host node for recovery tests");
  if (!host_object) {
    return;
  }

  const jsapi::RpcEnv env = test::MakeEnv();
  host_object->AddEventListener("recover-evt", 501, env);
  f.sink.sync_handler = [](int, const std::vector<jsapi::JsValue>&,
                           jsapi::JsValue*, std::string* err) {
    if (err) *err = "callback exploded";
    return 1;
  };
  std::string error;
  jsapi::JsValue result =
      host_object->DispatchEventSync("recover-evt", env, {}, 1000, &error);
  test::Check(result.is_null() && error == "callback exploded",
              "callback failure surfaces as sync event error", error);

  f.sink.sync_handler = nullptr;
  error.clear();
  result = host_object->DispatchEventSync("recover-evt", env, {}, 1000, &error);
  test::Check(error.empty() && result.AsString() == "sync:recover-evt",
              "sync event path recovers after a callback failure", error);
  host_object->RemoveEventListener("recover-evt", 501);
}

TEST(reliability, boundary_listener_cleanup) {
  test::CoreFixture& f = Fixture();
  auto host_object = f.registry.GetObjectByHandle(f.host_handle);
  test::Check(host_object != nullptr,
              "resolve host node for listener cleanup test");
  if (!host_object) {
    return;
  }

  jsapi::RpcEnv old_doc;
  old_doc.browser_id = 42;
  old_doc.frame_id = "5-old";
  jsapi::RpcEnv new_doc;
  new_doc.browser_id = 42;
  new_doc.frame_id = "5-new";
  const jsapi::RpcEnv other_browser = test::MakeEnv();  // 7 / test-frame

  host_object->AddEventListener("cleanup-evt", 601, old_doc);
  host_object->AddEventListener("cleanup-evt", 602, new_doc);
  host_object->AddEventListener("cleanup-evt", 603, other_browser);

  test::Check(host_object->DiscardListeners(42, "5-new") == 1,
              "DiscardListeners drops only the stale document of that browser");
  f.sink.events.clear();
  test::Check(host_object->DispatchEvent("cleanup-evt", new_doc, {}) == 1,
              "current document listener survives cleanup");
  test::Check(host_object->DispatchEvent("cleanup-evt", other_browser, {}) == 1,
              "listeners of other browsers are untouched");

  test::Check(host_object->DiscardListeners(42, std::string()) == 1,
              "empty keep frame clears the whole browser (renderer crash)");
  test::Check(host_object->DiscardListeners(42, std::string()) == 0,
              "cleanup is idempotent");
  test::Check(host_object->DispatchEvent("cleanup-evt", other_browser, {}) == 1,
              "other browser listener survives a renderer crash cleanup");
  host_object->RemoveEventListener("cleanup-evt", 603);
}

TEST(reliability, boundary_sync_timeout) {
  // 后端超时后才回包时，调用方按超时失败且不误收迟到回包；
  // 迟到的回包被 seq 校验忽略，通道随后仍可正常收发。
  const unsigned long timeout_pid = 9600 + (::GetCurrentProcessId() % 300);
  jsapi_bridge::SyncChannel channel;
  std::string error;
  test::Check(channel.Start(timeout_pid, &error), "sync channel starts", error);

  jsapi_bridge::RequestReader browser_in(timeout_pid);
  jsapi_bridge::ReplyWriter browser_out(timeout_pid);
  test::Check(browser_in.Ensure(&error) && browser_out.Ensure(&error),
              "timeout peer channels open", error);

  std::atomic<bool> stop_responder{false};
  std::thread responder([&] {
    bool first = true;
    while (!stop_responder.load()) {
      jsapi_bridge::MessageKind kind = jsapi_bridge::kMessageRpc;
      long seq = 0;
      long invoke = 0;
      std::string payload;
      if (browser_in.TryRead(&kind, &seq, &invoke, &payload)) {
        if (first) {
          // 模拟插件执行过久：超过调用方预算之后才回包。
          ::Sleep(600);
          first = false;
        }
        browser_out.Write(seq, "pong:" + std::to_string(seq));
      } else {
        ::Sleep(1);
      }
    }
  });

  const auto begin = std::chrono::steady_clock::now();
  std::string response;
  std::string send_error;
  const bool slow_ok = channel.Send("slow-request", 1, &response, &send_error,
                                    /*timeout_ms=*/300);
  const double elapsed_ms =
      std::chrono::duration<double, std::milli>(
          std::chrono::steady_clock::now() - begin)
          .count();
  test::Check(!slow_ok && send_error.find("timed out") != std::string::npos,
              "sync call times out when the backend is too slow", send_error);
  test::Check(elapsed_ms >= 250.0 && elapsed_ms < 3000.0,
              "timeout honors the caller budget",
              std::to_string(elapsed_ms) + " ms");

  // 迟到回包（seq=1）不得被后续调用误收；seq 对不上就继续等自己的回包。
  bool recovered = false;
  for (int attempt = 0; attempt < 3 && !recovered; ++attempt) {
    response.clear();
    send_error.clear();
    const int seq = 10 + attempt;
    if (channel.Send("fast-request", seq, &response, &send_error,
                     /*timeout_ms=*/2000) &&
        response == "pong:" + std::to_string(seq)) {
      recovered = true;
    }
  }
  test::Check(recovered, "channel recovers after a timed-out call", send_error);

  stop_responder.store(true);
  responder.join();
  channel.Stop();
}
