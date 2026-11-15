#include <windows.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <string>
#include <thread>
#include <utility>
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

double ElapsedMs(std::chrono::steady_clock::time_point begin) {
  return std::chrono::duration<double, std::milli>(
             std::chrono::steady_clock::now() - begin)
      .count();
}

jsapi::RpcResponse GetByIndex(jsapi::RpcHost* host, int handle, int index) {
  jsapi::RpcRequest request;
  request.seq = 100;
  request.op = "get";
  request.handle = handle;
  request.index = index;
  return test::Roundtrip(host, std::move(request));
}

}  // namespace

TEST(perf, perf_baseline) {
  test::CoreFixture& f = Fixture();
  const int iterations = 500;
  const auto rpc_begin = std::chrono::steady_clock::now();
  bool rpc_ok = true;
  for (int i = 0; i < iterations; ++i) {
    const jsapi::RpcResponse desc = test::Describe(&f.rpc, f.host_handle);
    if (!desc.ok) rpc_ok = false;
  }
  const double rpc_ms = ElapsedMs(rpc_begin) / iterations;
  std::printf("  local describe roundtrip: %.3f ms/call\n", rpc_ms);
  test::Check(rpc_ok && rpc_ms < 5.0,
              "local describe roundtrip stays below 5ms/call",
              std::to_string(rpc_ms));

  jsapi::JsValue sample = jsapi::JsValue::Object();
  sample.Set("name", jsapi::JsValue::String("perf"));
  sample.Set("list", jsapi::JsValue::Array({jsapi::JsValue::Int(1),
                                            jsapi::JsValue::Int(2),
                                            jsapi::JsValue::String("x")}));
  const auto json_begin = std::chrono::steady_clock::now();
  bool json_ok = true;
  for (int i = 0; i < iterations; ++i) {
    const std::string encoded = sample.ToJson();
    jsapi::JsValue decoded;
    if (!jsapi::ParseJson(encoded, &decoded, nullptr)) json_ok = false;
  }
  const double json_ms = ElapsedMs(json_begin) / iterations;
  std::printf("  json encode+decode: %.4f ms/call\n", json_ms);
  test::Check(json_ok && json_ms < 1.0,
              "json roundtrip stays below 1ms/call", std::to_string(json_ms));
}

TEST(perf, perf_throughput) {
  test::CoreFixture& f = Fixture();
  const jsapi::RpcResponse desc = test::Describe(&f.rpc, f.host_handle);
  const int counter = test::FindMemberIndex(desc.value, "counter");
  const int mul = test::FindMemberIndex(desc.value, "mul");
  test::Check(counter >= 0 && mul >= 0, "perf: host members resolved",
              desc.error);

  const int iterations = 20000;

  const auto property_begin = std::chrono::steady_clock::now();
  bool property_ok = true;
  for (int i = 0; i < iterations; ++i) {
    if (!GetByIndex(&f.rpc, f.host_handle, counter).ok) property_ok = false;
  }
  const double property_ms = ElapsedMs(property_begin);
  const double property_ops = iterations * 1000.0 / property_ms;
  std::printf("  rpc property read: %.0f ops/s (%.2f us/op)\n", property_ops,
              property_ms * 1000.0 / iterations);
  test::Check(property_ok && property_ops > 50000.0,
              "perf: rpc property read throughput > 50k ops/s",
              std::to_string(static_cast<long long>(property_ops)) + " ops/s");

  const auto call_begin = std::chrono::steady_clock::now();
  bool call_ok = true;
  for (int i = 0; i < iterations; ++i) {
    const jsapi::RpcResponse response =
        test::Execute(&f.rpc, f.host_handle, mul,
                      {jsapi::JsValue::Int(6), jsapi::JsValue::Int(7)});
    if (!response.ok || response.value.AsInt() != 42) call_ok = false;
  }
  const double call_ms = ElapsedMs(call_begin);
  const double call_ops = iterations * 1000.0 / call_ms;
  std::printf("  rpc function call: %.0f ops/s (%.2f us/op)\n", call_ops,
              call_ms * 1000.0 / iterations);
  test::Check(call_ok && call_ops > 50000.0,
              "perf: rpc function call throughput > 50k ops/s",
              std::to_string(static_cast<long long>(call_ops)) + " ops/s");

  jsapi::JsValue sample = jsapi::JsValue::Object();
  sample.Set("name", jsapi::JsValue::String("perf"));
  sample.Set("n", jsapi::JsValue::Int(42));
  sample.Set("list", jsapi::JsValue::Array({jsapi::JsValue::Int(1),
                                            jsapi::JsValue::Int(2),
                                            jsapi::JsValue::String("x")}));
  const std::string sample_json = sample.ToJson();
  const auto json_begin = std::chrono::steady_clock::now();
  bool json_ok = true;
  for (int i = 0; i < iterations; ++i) {
    jsapi::JsValue decoded;
    if (!jsapi::ParseJson(sample_json, &decoded, nullptr)) json_ok = false;
  }
  const double json_ms = ElapsedMs(json_begin);
  const double json_ops = iterations * 1000.0 / json_ms;
  std::printf("  json decode: %.0f ops/s (%.2f us/op)\n", json_ops,
              json_ms * 1000.0 / iterations);
  test::Check(json_ok && json_ops > 50000.0,
              "perf: json decode throughput > 50k ops/s",
              std::to_string(static_cast<long long>(json_ops)) + " ops/s");
}

TEST(perf, perf_latency) {
  // 通道往返延迟：真实共享内存 + 跨线程唤醒。
  const unsigned long pid = 5000 + (::GetCurrentProcessId() % 400);
  jsapi_bridge::SyncChannel channel;
  std::string error;
  test::Check(channel.Start(pid, &error), "perf: sync channel starts", error);

  jsapi_bridge::RequestReader browser_in(pid);
  jsapi_bridge::ReplyWriter browser_out(pid);
  test::Check(browser_in.Ensure(&error) && browser_out.Ensure(&error),
              "perf: channel peer opens", error);

  std::atomic<bool> stop{false};
  std::thread peer([&] {
    while (!stop.load()) {
      jsapi_bridge::MessageKind kind = jsapi_bridge::kMessageRpc;
      long seq = 0;
      long invoke = 0;
      std::string payload;
      if (browser_in.TryRead(&kind, &seq, &invoke, &payload)) {
        browser_out.Write(seq, "ok");
      } else {
        ::WaitForSingleObject(browser_in.event(), 20);
      }
    }
  });

  // 预热，避免把首次唤醒成本算进平均值。
  for (int i = 0; i < 20; ++i) {
    std::string response;
    std::string send_error;
    channel.Send("warmup", 1000 + i, &response, &send_error, 1000);
  }

  const int iterations = 300;
  int failures = 0;
  const auto begin = std::chrono::steady_clock::now();
  for (int i = 0; i < iterations; ++i) {
    std::string response;
    std::string send_error;
    if (!channel.Send("perf", 2000 + i, &response, &send_error, 1000) ||
        response != "ok") {
      ++failures;
    }
  }
  const double total_us = ElapsedMs(begin) * 1000.0;
  const double avg_us = total_us / iterations;
  std::printf("  channel roundtrip: %.1f us/call (%d failures)\n", avg_us,
              failures);
  test::Check(failures == 0 && avg_us < 300.0,
              "perf: channel roundtrip latency < 300us/call",
              std::to_string(avg_us) + " us");

  stop.store(true);
  peer.join();
  channel.Stop();

  // 插件调用延迟：动态 DLL 节点上的纯计算函数。
  std::string plugin_error;
  jsapi::JsRegistry registry;
  test::TestEventSink sink;
  registry.SetEventSink(&sink);
  jsapi::JsRegistry::Factory factory =
      jsapi::plugins::LoadDll(test::PluginPath(), &plugin_error);
  test::Check(factory != nullptr, "perf: demo plugin available", plugin_error);
  if (!factory) {
    return;
  }
  registry.RegisterNode("demo", std::move(factory));
  jsapi::RpcHost rpc(&registry);
  const int demo_handle = test::GetNodeHandle(&rpc, "demo");
  const int add = test::FindMemberIndex(test::Describe(&rpc, demo_handle).value,
                                        "add");
  test::Check(demo_handle > 0 && add >= 0, "perf: plugin add resolved");

  for (int i = 0; i < 50; ++i) {
    test::Execute(&rpc, demo_handle, add,
                  {jsapi::JsValue::Int(1), jsapi::JsValue::Int(2)});
  }
  const int plugin_iterations = 2000;
  bool plugin_ok = true;
  const auto plugin_begin = std::chrono::steady_clock::now();
  for (int i = 0; i < plugin_iterations; ++i) {
    const jsapi::RpcResponse response =
        test::Execute(&rpc, demo_handle, add,
                      {jsapi::JsValue::Int(1), jsapi::JsValue::Int(2)});
    if (!response.ok || response.value.AsInt() != 3) plugin_ok = false;
  }
  const double plugin_avg_us =
      ElapsedMs(plugin_begin) * 1000.0 / plugin_iterations;
  std::printf("  plugin call: %.2f us/call\n", plugin_avg_us);
  test::Check(plugin_ok && plugin_avg_us < 500.0,
              "perf: plugin call latency < 500us/call",
              std::to_string(plugin_avg_us) + " us");
}
