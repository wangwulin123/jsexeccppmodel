# 多 web 页压力性能测试（窗口化）：每个页面 = 一个 CefBrowser + 一条同步通道
# + 一个渲染进程，覆盖多页面并发同步 RPC 与整体关闭回收。
#
#   jsapi_window_stress_test --pages=10 --calls=200
#
# 输出与 jsapi_self_test 一致的 [ PASS ] / [ FAIL ] 行，退出码 0 表示通过。

add_executable(jsapi_window_stress_test window_stress_test.cpp)

target_link_libraries(jsapi_window_stress_test PRIVATE
  jsapi_qt
  jsapi_host_nodes
)

jsapi_setup_target(jsapi_window_stress_test)
jsapi_assert_no_cef(jsapi_window_stress_test)

# 与 demo_app 同目录：libcef.dll / icudtl.dat / Qt 运行时 / platform 插件
# 都由 demo_app 的 POST_BUILD 部署好，测试程序直接复用。
set_target_properties(jsapi_window_stress_test PROPERTIES
  RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin/$<CONFIGURATION>"
)
add_dependencies(jsapi_window_stress_test demo_app)

add_custom_command(TARGET jsapi_window_stress_test POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E copy_if_different
          "${CMAKE_SOURCE_DIR}/test/resources/jsapi_window_stress.html"
          "$<TARGET_FILE_DIR:jsapi_window_stress_test>/jsapi_window_stress.html"
)
