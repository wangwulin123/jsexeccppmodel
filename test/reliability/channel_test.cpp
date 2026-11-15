#include <windows.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <map>
#include <string>
#include <thread>
#include <vector>

#include "jsapi_channel/jsapi_channel.h"
#include "jsapi_channel/jsapi_sync_channel.h"
#include "support/test_harness.h"

TEST(reliability, sync_channel) {
  using jsapi_bridge::DecodeEventPayload;
  using jsapi_bridge::DecodeEventResult;
  using jsapi_bridge::EncodeEventPayload;
  using jsapi_bridge::EncodeEventResult;
  using jsapi_bridge::EventRingReader;
  using jsapi_bridge::EventRingWriter;
  using jsapi_bridge::kEventSlots;
  using jsapi_bridge::kMessageEventResult;
  using jsapi_bridge::kMessageRpc;
  using jsapi_bridge::MakeChannelNames;
  using jsapi_bridge::ReplyReader;
  using jsapi_bridge::ReplyWriter;
  using jsapi_bridge::RequestReader;
  using jsapi_bridge::RequestWriter;

  const unsigned long pid = 4000 + (::GetCurrentProcessId() % 1000);
  std::string error;

  // 请求通道：pending 电平只允许消费一次。
  RequestWriter request_writer(pid);
  RequestReader request_reader(pid);
  test::Check(request_writer.Ensure(&error) && request_reader.Ensure(&error),
              "request channel opens", error);
  jsapi_bridge::MessageKind kind = kMessageRpc;
  long seq = 0;
  long invoke = 0;
  std::string payload;
  test::Check(!request_reader.TryRead(&kind, &seq, &invoke, &payload),
              "request channel empty before write");
  test::Check(request_writer.Write(kMessageRpc, 7, 0, "hello", &error),
              "request channel write", error);
  test::Check(request_reader.TryRead(&kind, &seq, &invoke, &payload) &&
                  kind == kMessageRpc && seq == 7 && invoke == 0 &&
                  payload == "hello",
              "request channel roundtrip keeps seq/kind/payload");
  test::Check(!request_reader.TryRead(&kind, &seq, &invoke, &payload),
              "request pending flag is level cleared after read");

  // 回包通道：seq 不匹配时不消费，正确回包仍然可读。
  ReplyWriter reply_writer(pid);
  ReplyReader reply_reader(pid);
  reply_reader.Ensure(&error);
  reply_reader.Reset();
  test::Check(reply_writer.Write(9, "resp-9"), "reply channel write");
  std::string reply;
  test::Check(!reply_reader.TryTake(8, nullptr, &reply),
              "stale reply is not consumed by wrong seq");
  test::Check(reply_reader.TryTake(9, nullptr, &reply) && reply == "resp-9",
              "reply channel delivers matching seq");
  test::Check(!reply_reader.TryTake(9, nullptr, &reply),
              "reply pending is cleared after consume");

  // 事件环形缓冲：FIFO、invoke_id 透传、槽满返回失败。
  EventRingWriter event_writer(pid);
  EventRingReader event_reader(pid);
  event_writer.Ensure(&error);
  event_reader.Ensure(&error);
  test::Check(event_writer.Write(0, "async-1") &&
                  event_writer.Write(33, "sync-2"),
              "event ring write");
  long event_invoke = -1;
  std::string event_payload;
  test::Check(event_reader.TryRead(&event_invoke, &event_payload) &&
                  event_invoke == 0 && event_payload == "async-1",
              "event ring preserves fifo order");
  test::Check(event_reader.TryRead(&event_invoke, &event_payload) &&
                  event_invoke == 33 && event_payload == "sync-2",
              "event ring keeps invoke id");
  test::Check(!event_reader.TryRead(&event_invoke, &event_payload),
              "event ring empty after drain");
  bool filled = true;
  for (int i = 0; i < kEventSlots + 1; ++i) {
    if (!event_writer.Write(i + 1, "e" + std::to_string(i))) {
      filled = false;
      test::Check(i == kEventSlots, "event ring rejects the first overflow write",
                  std::to_string(i));
    }
  }
  test::Check(!filled, "event ring reports overflow instead of dropping silently");
  for (int i = 0; i < kEventSlots; ++i) {
    event_reader.TryRead(&event_invoke, &event_payload);
  }

  // payload / 回执编解码。
  const std::string encoded = EncodeEventPayload(
      "evt", 5, 42, {jsapi::JsValue::Int(1), jsapi::JsValue::String("a")},
      "frame-1");
  std::string name;
  int callback = -1;
  int decoded_invoke = 0;
  std::vector<jsapi::JsValue> args;
  std::string frame;
  test::Check(DecodeEventPayload(encoded, &name, &callback, &decoded_invoke,
                                 &args, &frame) &&
                  name == "evt" && callback == 5 && decoded_invoke == 42 &&
                  args.size() == 2 && args[0].AsInt() == 1 &&
                  args[1].AsString() == "a" && frame == "frame-1",
              "event payload roundtrip");

  bool ok = false;
  jsapi::JsValue value;
  test::Check(DecodeEventResult(
                  EncodeEventResult(42, true, jsapi::JsValue::Int(9), ""),
                  &decoded_invoke, &ok, &value, &error) &&
                  decoded_invoke == 42 && ok && value.AsInt() == 9,
              "event result success roundtrip");
  test::Check(DecodeEventResult(
                  EncodeEventResult(43, false, jsapi::JsValue::Null(), "boom"),
                  &decoded_invoke, &ok, &value, &error) &&
                  decoded_invoke == 43 && !ok && error == "boom",
              "event result error roundtrip");

  test::Check(MakeChannelNames(pid).request_map ==
                  L"Local\\jsapi_req_" + std::to_wstring(pid),
              "channel names are isolated per renderer pid");

  // 事件单条容量：超过一个槽必须拒绝（调用方回退进程消息）。
  test::Check(!EventRingWriter::Fits(
                  std::string(jsapi_bridge::kEventCapacity + 1, 'x')),
              "event payload above slot capacity is rejected");

  // 嵌套编排：用真实通道模拟"同步 RPC -> 同步事件 -> 回调内再发同步 RPC"。
  {
    const unsigned long nested_pid = pid + 1;
    RequestWriter renderer_out(nested_pid);
    RequestReader browser_in(nested_pid);
    ReplyReader renderer_in(nested_pid);
    ReplyWriter browser_out(nested_pid);
    EventRingReader renderer_events(nested_pid);
    EventRingWriter browser_events(nested_pid);
    test::Check(renderer_out.Ensure(&error) && browser_in.Ensure(&error) &&
                    renderer_in.Ensure(&error) && browser_out.Ensure(&error) &&
                    renderer_events.Ensure(&error) &&
                    browser_events.Ensure(&error),
                "nesting channels open", error);

    std::atomic<bool> browser_ok{false};
    std::thread browser([&] {
      auto read_one = [&](jsapi_bridge::MessageKind* out_kind, long* out_seq,
                          long* out_invoke, std::string* out) {
        const ULONGLONG limit = ::GetTickCount64() + 5000;
        while (::GetTickCount64() < limit) {
          if (browser_in.TryRead(out_kind, out_seq, out_invoke, out)) {
            return true;
          }
          ::Sleep(1);
        }
        return false;
      };

      jsapi_bridge::MessageKind local_kind = kMessageRpc;
      long local_seq = 0;
      long local_invoke = 0;
      std::string local_payload;
      if (!read_one(&local_kind, &local_seq, &local_invoke, &local_payload) ||
          local_seq != 1) {
        return;
      }
      // 外层请求执行中派发同步事件。
      browser_events.Write(100, "nested-event");
      // 回调里的嵌套同步请求。
      if (!read_one(&local_kind, &local_seq, &local_invoke, &local_payload) ||
          local_seq != 2) {
        return;
      }
      browser_out.Write(2, "reply-2");
      // 回调返回后渲染侧发回事件回执。
      if (!read_one(&local_kind, &local_seq, &local_invoke, &local_payload) ||
          local_kind != kMessageEventResult || local_invoke != 100) {
        return;
      }
      browser_ok.store(true);
      browser_out.Write(1, "reply-1");
    });

    renderer_out.Write(kMessageRpc, 1, 0, "req-1", &error);
    long nested_event_invoke = 0;
    std::string nested_event_payload;
    bool got_event = false;
    ULONGLONG limit = ::GetTickCount64() + 5000;
    while (!got_event && ::GetTickCount64() < limit) {
      got_event = renderer_events.TryRead(&nested_event_invoke,
                                          &nested_event_payload) &&
                  nested_event_invoke == 100;
      if (!got_event) ::Sleep(1);
    }
    test::Check(got_event,
                "nesting: sync event is delivered while outer call waits");

    renderer_out.Write(kMessageRpc, 2, 0, "req-2", &error);
    std::string nested_reply;
    bool got_nested = false;
    limit = ::GetTickCount64() + 5000;
    while (!got_nested && ::GetTickCount64() < limit) {
      if (renderer_in.TryTake(2, nullptr, &nested_reply)) {
        got_nested = (nested_reply == "reply-2");
      } else {
        ::Sleep(1);
      }
    }
    test::Check(got_nested, "nesting: inner request gets its own reply");

    renderer_out.Write(kMessageEventResult, 0, 100,
                       EncodeEventResult(100, true, jsapi::JsValue::Int(7), ""),
                       &error);
    std::string outer_reply;
    bool got_outer = false;
    limit = ::GetTickCount64() + 5000;
    while (!got_outer && ::GetTickCount64() < limit) {
      if (renderer_in.TryTake(1, nullptr, &outer_reply)) {
        got_outer = (outer_reply == "reply-1");
      } else {
        ::Sleep(1);
      }
    }
    test::Check(got_outer,
                "nesting: outer reply arrives after the nested exchange");

    browser.join();
    test::Check(browser_ok.load(),
                "nesting: browser consumed nested request and event result");
  }
}

TEST(reliability, channel_edge_cases) {
  using jsapi_bridge::EventRingReader;
  using jsapi_bridge::EventRingWriter;
  using jsapi_bridge::kMessageRpc;
  using jsapi_bridge::ReplyReader;
  using jsapi_bridge::ReplyWriter;
  using jsapi_bridge::RequestReader;
  using jsapi_bridge::RequestWriter;

  const unsigned long pid = 5400 + (::GetCurrentProcessId() % 800);
  std::string error;

  RequestWriter request_writer(pid);
  RequestReader request_reader(pid);
  test::Check(request_writer.Ensure(&error) && request_reader.Ensure(&error),
              "channel edge: request channels open", error);
  test::Check(request_writer.Write(kMessageRpc, 11, 0, "", &error),
              "channel: zero-length request is accepted", error);
  jsapi_bridge::MessageKind kind = kMessageRpc;
  long seq = 0;
  long invoke = 0;
  std::string payload;
  test::Check(request_reader.TryRead(&kind, &seq, &invoke, &payload) &&
                  seq == 11 && payload.empty(),
              "channel: zero-length request roundtrips");

  ReplyWriter reply_writer(pid);
  ReplyReader reply_reader(pid);
  test::Check(reply_writer.Ensure(&error) && reply_reader.Ensure(&error),
              "channel edge: reply channels open", error);
  reply_reader.Reset();
  test::Check(reply_writer.Write(12, ""),
              "channel: zero-length reply is accepted");
  std::string reply;
  test::Check(reply_reader.TryTake(12, nullptr, &reply) && reply.empty(),
              "channel: zero-length reply roundtrips");

  EventRingWriter event_writer(pid);
  EventRingReader event_reader(pid);
  test::Check(event_writer.Ensure(&error) && event_reader.Ensure(&error),
              "channel edge: event ring opens", error);
  event_writer.Clear();
  test::Check(event_writer.Write(0, ""),
              "channel: zero-length event is accepted");
  long event_invoke = -1;
  std::string event_payload;
  test::Check(event_reader.TryRead(&event_invoke, &event_payload) &&
                  event_invoke == 0 && event_payload.empty(),
              "channel: zero-length event roundtrips");

  // 超容量回包：必须像请求一样被显式拒绝，而不是静默截断。
  const std::string huge(jsapi_bridge::kReplyCapacity + 1, 'x');
  test::Check(!reply_writer.Write(13, huge),
              "channel: oversized reply is rejected");

  // 重复 Stop 幂等，之后 Send 给出明确错误。
  const unsigned long stop_pid = pid + 1;
  jsapi_bridge::SyncChannel channel;
  test::Check(channel.Start(stop_pid, &error), "channel: stop test starts",
              error);
  channel.Stop();
  channel.Stop();
  std::string response;
  std::string send_error;
  test::Check(!channel.Send("after-stop", 1, &response, &send_error, 100) &&
                  send_error.find("not started") != std::string::npos,
              "channel: Stop is idempotent and Send fails clearly", send_error);
}

TEST(reliability, channel_concurrency) {
  using jsapi_bridge::EventRingReader;
  using jsapi_bridge::EventRingWriter;

  const unsigned long pid = 6200 + (::GetCurrentProcessId() % 700);
  EventRingWriter writer(pid);
  EventRingReader reader(pid);
  std::string error;
  test::Check(writer.Ensure(&error) && reader.Ensure(&error),
              "channel: concurrency ring opens", error);
  writer.Clear();

  const int kPerProducer = 300;
  const int kTotal = 2 * kPerProducer;
  std::atomic<int> produced{0};
  std::atomic<bool> produce_failed{false};

  auto produce = [&](int tag) {
    for (int i = 0; i < kPerProducer; ++i) {
      const std::string payload =
          "p" + std::to_string(tag) + "-" + std::to_string(i);
      bool wrote = false;
      for (int attempt = 0; attempt < 100000 && !wrote; ++attempt) {
        wrote = writer.Write(0, payload);
        if (!wrote) ::Sleep(0);
      }
      if (!wrote) {
        produce_failed.store(true);
        return;
      }
      produced.fetch_add(1);
    }
  };

  std::thread first(produce, 1);
  std::thread second(produce, 2);

  std::map<int, int> last_index;
  last_index[1] = -1;
  last_index[2] = -1;
  int received = 0;
  int order_errors = 0;
  int parse_errors = 0;
  const ULONGLONG deadline = ::GetTickCount64() + 15000;
  while (received < kTotal && ::GetTickCount64() < deadline) {
    long invoke = -1;
    std::string payload;
    if (!reader.TryRead(&invoke, &payload)) {
      ::Sleep(0);
      continue;
    }
    ++received;
    if (payload.size() < 4 || payload[0] != 'p') {
      ++parse_errors;
      continue;
    }
    const int tag = payload[1] - '0';
    const int index = std::atoi(payload.c_str() + 3);
    const auto it = last_index.find(tag);
    if (it == last_index.end() || index != it->second + 1) {
      ++order_errors;
    } else {
      it->second = index;
    }
  }

  first.join();
  second.join();

  test::Check(!produce_failed.load() &&
                  produced.load() == kTotal && received == kTotal &&
                  parse_errors == 0,
              "channel: concurrent producers deliver every message without loss",
              "produced=" + std::to_string(produced.load()) +
                  " received=" + std::to_string(received) +
                  " parse_errors=" + std::to_string(parse_errors));
  test::Check(order_errors == 0,
              "channel: per-producer FIFO order is preserved",
              std::to_string(order_errors));
}

TEST(reliability, channel_nesting_guard) {
  const unsigned long pid = 7200 + (::GetCurrentProcessId() % 600);
  jsapi_bridge::SyncChannel channel;
  std::string error;
  test::Check(channel.Start(pid, &error), "channel: nesting guard starts",
              error);

  jsapi_bridge::RequestReader browser_in(pid);
  jsapi_bridge::ReplyWriter browser_out(pid);
  jsapi_bridge::EventRingWriter browser_events(pid);
  test::Check(browser_in.Ensure(&error) && browser_out.Ensure(&error) &&
                  browser_events.Ensure(&error),
              "channel: nesting guard peer opens", error);
  browser_events.Clear();

  std::atomic<bool> stop{false};
  std::atomic<int> dispatched{0};
  std::atomic<int> depth_rejections{0};
  std::atomic<int> nested_timeouts{0};
  std::atomic<int> unexpected_errors{0};

  std::thread peer([&] {
    bool holding_outer = false;
    ULONGLONG outer_reply_at = 0;
    int emitted = 0;
    while (!stop.load()) {
      jsapi_bridge::MessageKind kind = jsapi_bridge::kMessageRpc;
      long seq = 0;
      long invoke = 0;
      std::string payload;
      if (browser_in.TryRead(&kind, &seq, &invoke, &payload)) {
        if (seq == 1) {
          // 压住外层回包，给嵌套层级留出真正的执行窗口。
          holding_outer = true;
          outer_reply_at = ::GetTickCount64() + 1000;
        }
        // 其余回包全部压住：让嵌套 Send 持续等待并不断重入以触发深度门限。
        continue;
      }
      if (holding_outer && ::GetTickCount64() >= outer_reply_at) {
        browser_out.Write(1, "ok:1");
        holding_outer = false;
      }
      if (emitted < 40) {
        const int invoke_id = 100 + emitted;
        const std::string event_payload = jsapi_bridge::EncodeEventPayload(
            "guard", 1, invoke_id, {}, "frame-guard");
        if (browser_events.Write(invoke_id, event_payload)) {
          ++emitted;
        }
      }
      ::WaitForSingleObject(browser_in.event(), 1);
    }
  });

  channel.SetInlineDispatch(
      [&](const std::string&, int, const std::vector<jsapi::JsValue>&,
          jsapi::JsValue* result, std::string* /*dispatch_error*/) {
        const int level = dispatched.fetch_add(1) + 1;
        if (level <= 12) {
          std::string response;
          std::string send_error;
          if (!channel.Send("nest-" + std::to_string(level), 200 + level,
                            &response, &send_error, 300)) {
            if (send_error.find("depth exceeded") != std::string::npos) {
              depth_rejections.fetch_add(1);
            } else if (send_error.find("timed out") != std::string::npos) {
              nested_timeouts.fetch_add(1);
            } else {
              unexpected_errors.fetch_add(1);
            }
          }
        }
        if (result) *result = jsapi::JsValue::Int(level);
        return true;
      });

  std::string outer_error;
  std::string outer_response;
  bool outer_ok = false;
  const auto begin = std::chrono::steady_clock::now();
  std::thread caller([&] {
    outer_ok = channel.Send("outer", 1, &outer_response, &outer_error, 5000);
  });
  caller.join();
  const double elapsed_ms =
      std::chrono::duration<double, std::milli>(
          std::chrono::steady_clock::now() - begin)
          .count();

  stop.store(true);
  peer.join();
  channel.Stop();

  test::Check(outer_ok && outer_response == "ok:1",
              "channel: outermost call still completes", outer_error);
  test::Check(dispatched.load() >= 9,
              "channel: nested sync events re-enter one level per handoff",
              std::to_string(dispatched.load()));
  test::Check(depth_rejections.load() >= 1,
              "channel: depth guard rejects nesting beyond kMaxSyncDepth",
              std::to_string(depth_rejections.load()) + " rejected, " +
                  std::to_string(nested_timeouts.load()) + " timed out");
  test::Check(unexpected_errors.load() == 0 && elapsed_ms < 5000.0,
              "channel: depth guard unwinds without deadlock",
              std::to_string(unexpected_errors.load()) + " unexpected errors, " +
                  std::to_string(elapsed_ms) + " ms");
}
