cmake_minimum_required(VERSION 3.21)

project(jsapi_demo VERSION 1.7.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# 可执行文件与动态库统一输出到 bin 目录（多配置生成器会自动加 Debug/Release 子目录）。
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)

include(GNUInstallDirs)

#
# CEF 二进制包位置，可在配置时用 -DCEF_ROOT=... 覆盖。
#
if(NOT DEFINED CEF_ROOT)
  set(CEF_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../../cef_binary" CACHE PATH
      "CEF binary distribution root")
endif()

set(JSAPI_COMMON_DEFINITIONS
  WIN32
  _UNICODE
  UNICODE
  NOMINMAX
  WIN32_LEAN_AND_MEAN
  _WIN32_WINNT=0x0A00
  WINVER=0x0A00
)

function(jsapi_setup_target target)
  target_compile_definitions(${target} PRIVATE ${JSAPI_COMMON_DEFINITIONS})
  if(MSVC)
    target_compile_options(${target} PRIVATE /utf-8 /EHsc)
  endif()
endfunction()

# ----------------------------------------------------------------------------
# 分层边界检查（构建期强制，而不是口头约定）
#
# 依赖方向固定为： jsapi_qt.dll -> jsapi_cef.dll -> jsapi.dll
# 业务层（demo/ test/）只允许 include jsapi/ 与 jsapi_qt/ 的公共头，
# 出现 CEF 头即配置失败。
# ----------------------------------------------------------------------------
function(jsapi_assert_no_cef target)
  get_target_property(_sources ${target} SOURCES)
  if(NOT _sources)
    return()
  endif()
  foreach(_src ${_sources})
    if(IS_ABSOLUTE "${_src}")
      set(_path "${_src}")
    else()
      set(_path "${CMAKE_CURRENT_SOURCE_DIR}/${_src}")
    endif()
    if(NOT EXISTS "${_path}")
      continue()
    endif()
    file(READ "${_path}" _content)
    if(_content MATCHES "include/cef_")
      message(FATAL_ERROR
        "[分层边界] target '${target}' 的业务源文件 ${_src} 直接 include 了 CEF 头文件。\n"
        "业务层只能使用 jsapi/ 与 jsapi_qt/ 的公共头；CEF 适配请留在 jsapi_cef 层。")
    endif()
  endforeach()
endfunction()

# ----------------------------------------------------------------------------
# Qt：容器层与示例插件都依赖 Qt Widgets。
# ----------------------------------------------------------------------------
if(NOT CMAKE_PREFIX_PATH AND EXISTS
   "${CMAKE_CURRENT_SOURCE_DIR}/../../qt/6.8.3/msvc2022_64/lib/cmake/Qt6")
  set(CMAKE_PREFIX_PATH "${CMAKE_CURRENT_SOURCE_DIR}/../../qt/6.8.3/msvc2022_64"
      CACHE PATH "Qt installation prefix")
endif()

find_package(QT NAMES Qt6 Qt5 COMPONENTS Core Widgets QUIET)
if(QT_FOUND)
  find_package(Qt${QT_VERSION_MAJOR} REQUIRED COMPONENTS Core Widgets)
endif()

# ============================================================================
# L1  jsapi.dll —— 与 CEF / Qt 完全无关的执行模型层
#     JsValue / 函数表 / 节点基类 / 句柄注册表 / 插件加载 / 同步 RPC 通道
#     业务插件、单元测试、任何宿主实现都直接依赖这一层。
# ============================================================================
add_library(jsapi SHARED
  src/core/js_value.cpp
  src/core/js_table.cpp
  src/core/js_object.cpp
  src/core/js_registry.cpp
  src/rpc/jsapi_rpc.cpp
  src/channel/jsapi_channel.cpp
  src/channel/jsapi_sync_channel.cpp
)

target_include_directories(jsapi PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
  $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
)
# 导出全部符号：SDK 只对外承诺 include/jsapi/ + include/jsapi_channel/ 的公共头，
# 其余符号即便被导出也不在契约内。
set_target_properties(jsapi PROPERTIES
  WINDOWS_EXPORT_ALL_SYMBOLS ON
  OUTPUT_NAME "jsapi"
)
jsapi_setup_target(jsapi)

# ============================================================================
# 业务节点：静态注册示例（编进业务 exe）
# ============================================================================
add_library(jsapi_host_nodes STATIC
  demo/host_nodes/host_nodes.cpp
  demo/host_nodes/host_nodes.h
)
target_include_directories(jsapi_host_nodes PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/demo/host_nodes>
  $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}/host_nodes>
)
target_link_libraries(jsapi_host_nodes PUBLIC jsapi)
jsapi_setup_target(jsapi_host_nodes)
jsapi_assert_no_cef(jsapi_host_nodes)

# ============================================================================
# 测试辅助目标：让自测只依赖公共头，不再直连 src/ 内部目录
# ============================================================================
add_library(jsapi_testing INTERFACE)
target_link_libraries(jsapi_testing INTERFACE jsapi jsapi_host_nodes)
target_include_directories(jsapi_testing INTERFACE
  ${CMAKE_CURRENT_SOURCE_DIR}/include
  ${CMAKE_CURRENT_SOURCE_DIR}/demo/host_nodes
)

# ============================================================================
# 自测：不依赖 Qt / CEF，覆盖对象模型、同步/异步 RPC、事件、通道、插件加载
# ============================================================================
add_executable(jsapi_self_test
  test/main.cpp
  test/support/test_harness.h
  test/support/test_harness.cpp
  test/support/test_helpers.h
  test/support/test_helpers.cpp
  test/support/test_fixtures.h
  test/reliability/value_model_test.cpp
  test/reliability/rpc_parse_test.cpp
  test/reliability/rpc_dispatch_test.cpp
  test/reliability/event_sync_test.cpp
  test/reliability/channel_test.cpp
  test/reliability/lifecycle_test.cpp
  test/reliability/thread_affinity_test.cpp
  test/reliability/reliability_boundary_test.cpp
  test/system/plugin_system_test.cpp
  test/perf/perf_test.cpp
)
target_include_directories(jsapi_self_test PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/test
)
target_link_libraries(jsapi_self_test PRIVATE jsapi_testing)
jsapi_setup_target(jsapi_self_test)
jsapi_assert_no_cef(jsapi_self_test)

# ============================================================================
# 业务插件（动态库）：导出 GetExtensionJsApiObj()
# ============================================================================
if(QT_FOUND)
  add_library(demo_plugin SHARED
    demo/demo_plugin/demo_plugin.cpp
  )
  target_link_libraries(demo_plugin PRIVATE jsapi Qt${QT_VERSION_MAJOR}::Widgets)
  set_target_properties(demo_plugin PROPERTIES PREFIX "" OUTPUT_NAME "demo_plugin")
  jsapi_setup_target(demo_plugin)
  jsapi_assert_no_cef(demo_plugin)

  # 插件加载时需要有 Qt6Core/Gui/Widgets.dll：复制到与插件/自测程序相同目录。
  foreach(qt_module Core Gui Widgets)
    add_custom_command(TARGET demo_plugin POST_BUILD
      COMMAND ${CMAKE_COMMAND} -E copy_if_different
              "$<TARGET_FILE:Qt${QT_VERSION_MAJOR}::${qt_module}>"
              "$<TARGET_FILE_DIR:demo_plugin>"
    )
  endforeach()
else()
  message(STATUS "Qt Core not found, skip demo_plugin target")
endif()

# ============================================================================
# L2  jsapi_cef.dll —— 唯一依赖 CEF 的层
#     CEF 初始化/子进程分流、BrowserHandle、双侧 CefApp。
#     公共头 include/jsapi_cef/runtime.h 不含任何 CEF 类型，
#     因此非 Qt 宿主也能直接接入（自备容器）。
# ============================================================================
if(EXISTS "${CEF_ROOT}/include/cef_app.h")
  set(CMAKE_MODULE_PATH ${CMAKE_MODULE_PATH} "${CEF_ROOT}/cmake")
  # Qt 在 Windows 上使用 /MD，CEF wrapper 默认 /MT；清空该缓存变量避免运行库不匹配。
  set(CEF_RUNTIME_LIBRARY_FLAG "" CACHE STRING
      "MSVC runtime flag used by CEF wrapper")
  find_package(CEF REQUIRED)
  add_subdirectory(${CEF_LIBCEF_DLL_WRAPPER_PATH} libcef_dll_wrapper)
  ADD_LOGICAL_TARGET("libcef_lib" "${CEF_LIB_DEBUG}" "${CEF_LIB_RELEASE}")

  add_library(jsapi_cef SHARED
    src/cef/jsapi_runtime.cpp
    src/cef/jsapi_browser_handle.cpp
    src/cef/jsapi_browser_handle.h
    src/cef/jsapi_cef_apps.cpp
    src/cef/jsapi_cef_apps.h
    src/cef/jsapi_browser_bridge.cpp
    src/cef/jsapi_browser_bridge.h
    src/cef/jsapi_render_handler.cpp
    src/cef/jsapi_render_handler.h
    src/cef/jsapi_render_core.cpp
    src/cef/jsapi_render_core.h
    src/cef/jsapi_render_types.h
    src/cef/jsapi_v8_proxy.cpp
  )
  target_include_directories(jsapi_cef PRIVATE ${CEF_ROOT})
  target_compile_definitions(jsapi_cef PRIVATE JSAPI_CEF_BUILD_DLL)
  target_link_libraries(jsapi_cef
    PUBLIC jsapi
    PRIVATE libcef_lib libcef_dll_wrapper ${CEF_STANDARD_LIBS}
  )
  jsapi_setup_target(jsapi_cef)

  # ==========================================================================
  # L3  jsapi_qt.dll —— Qt 容器层（QtRuntime + WebContainer）
  #     把 JSAPI 执行点绑定到 Qt GUI 线程，业务代码只 include 本层与 jsapi/。
  # ==========================================================================
  if(QT_FOUND)
    add_library(jsapi_qt SHARED
      src/qt/jsapi_qt_runtime.cpp
      src/qt/jsapi_web_container.cpp
    )
    target_compile_definitions(jsapi_qt PRIVATE JSAPI_QT_BUILD_DLL)
    target_link_libraries(jsapi_qt
      PUBLIC jsapi_cef
      PUBLIC Qt${QT_VERSION_MAJOR}::Widgets
    )
    jsapi_setup_target(jsapi_qt)

    add_subdirectory(demo/demo_app)

    # 多 web 页压力测试（窗口化）：需要真实 CEF + Qt 窗口，与被测代码同目录部署。
    add_subdirectory(test/window)
  else()
    message(STATUS "Qt Widgets not found, skip jsapi_qt / demo_app targets")
  endif()
else()
  message(WARNING "CEF_ROOT not found (${CEF_ROOT}), jsapi_cef / jsapi_qt / demo_app skipped")
endif()

# ============================================================================
# 安装（SDK 交付形态）
#
#   cmake --install build --prefix <sdk>
#   <sdk>/include/jsapi/        <- 业务插件与业务 exe 使用
#   <sdk>/include/jsapi_cef/    <- 非 Qt 宿主使用（CEF-free 的 CEF 适配门面）
#   <sdk>/include/jsapi_qt/     <- Qt 宿主使用
#   <sdk>/bin/*.dll
#   <sdk>/lib/cmake/jsapi/      <- find_package(jsapi)
# ============================================================================
install(TARGETS jsapi jsapi_host_nodes
  EXPORT jsapiTargets
  RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
  LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
  ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
)

# 适配层与容器层依赖 CEF / Qt，不作为可移植的 CMake 包导出，只安装二进制与导入库。
if(TARGET jsapi_cef)
  install(TARGETS jsapi_cef
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
  )
endif()
if(TARGET jsapi_qt)
  install(TARGETS jsapi_qt
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
  )
endif()

install(DIRECTORY include/jsapi DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
install(DIRECTORY include/jsapi_cef DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
install(DIRECTORY include/jsapi_qt DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
install(DIRECTORY include/jsapi_channel DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
install(DIRECTORY demo/host_nodes
  DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
  FILES_MATCHING PATTERN "*.h"
)

install(EXPORT jsapiTargets
  NAMESPACE jsapi::
  DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/jsapi
)

file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/jsapiConfig.cmake"
"include(\"\${CMAKE_CURRENT_LIST_DIR}/jsapiTargets.cmake\")\n")
install(FILES "${CMAKE_CURRENT_BINARY_DIR}/jsapiConfig.cmake"
  DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/jsapi
)
