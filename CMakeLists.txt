cmake_minimum_required(VERSION 3.21)

project(jsapi_demo LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# 可执行文件与插件统一输出到 bin 目录（多配置生成器会自动加 Debug/Release 子目录）。
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)

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
# Qt：插件内部实现（注册表/文件读取）与 Qt 宿主都依赖 Qt。
# 仓库内准备了最小 Qt（qtbase），也可用 -DCMAKE_PREFIX_PATH=... 覆盖。
# ----------------------------------------------------------------------------
if(NOT CMAKE_PREFIX_PATH AND EXISTS
   "${CMAKE_CURRENT_SOURCE_DIR}/../../qt/6.8.3/msvc2022_64/lib/cmake/Qt6")
  set(CMAKE_PREFIX_PATH "${CMAKE_CURRENT_SOURCE_DIR}/../../qt/6.8.3/msvc2022_64"
      CACHE PATH "Qt installation prefix")
endif()

find_package(QT NAMES Qt6 Qt5 COMPONENTS Core Widgets QUIET)
if(QT_FOUND)
  find_package(Qt${QT_VERSION_MAJOR} REQUIRED COMPONENTS Core)
endif()

# ----------------------------------------------------------------------------
# jsapi_core：与 CEF/Qt 完全无关的 JSAPI 对象模型
#   JsValue / 函数表 / 节点基类 / 句柄注册表 / 动态插件加载
# ----------------------------------------------------------------------------
add_library(jsapi_core STATIC
  src/core/js_value.cpp
  src/core/js_table.cpp
  src/core/js_object.cpp
  src/core/js_registry.cpp
)
target_include_directories(jsapi_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
jsapi_setup_target(jsapi_core)

# ----------------------------------------------------------------------------
# jsapi_rpc：浏览器进程侧的请求解析与按函数表索引分发
# ----------------------------------------------------------------------------
add_library(jsapi_rpc STATIC
  src/rpc/jsapi_rpc.cpp
)
target_include_directories(jsapi_rpc PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(jsapi_rpc PUBLIC jsapi_core)
jsapi_setup_target(jsapi_rpc)

# ----------------------------------------------------------------------------
# 业务插件：两种扩展方式
#   demo_plugin  -> 动态库，导出 GetExtensionJsApiObj()
#   jsapi_host_nodes -> 静态注册示例，直接编进宿主
# ----------------------------------------------------------------------------
if(QT_FOUND)
  # 插件要直接操作宿主窗口控件，需要 Qt Widgets。
  find_package(Qt${QT_VERSION_MAJOR} REQUIRED COMPONENTS Widgets)
  add_library(demo_plugin SHARED
    plugins/demo_plugin/demo_plugin.cpp
  )
  target_link_libraries(demo_plugin PRIVATE jsapi_core
    Qt${QT_VERSION_MAJOR}::Widgets)
  set_target_properties(demo_plugin PROPERTIES PREFIX "" OUTPUT_NAME "demo_plugin")
  jsapi_setup_target(demo_plugin)

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

add_library(jsapi_host_nodes STATIC
  plugins/host_nodes/host_nodes.cpp
)
target_include_directories(jsapi_host_nodes PUBLIC
  ${CMAKE_CURRENT_SOURCE_DIR}/plugins/host_nodes
)
target_link_libraries(jsapi_host_nodes PUBLIC jsapi_core)
jsapi_setup_target(jsapi_host_nodes)

# ----------------------------------------------------------------------------
# 自测：不依赖 Qt，验证函数表查找、同步/异步 RPC、属性、事件、DLL 加载
# ----------------------------------------------------------------------------
add_executable(jsapi_self_test tests/self_test.cpp)
target_link_libraries(jsapi_self_test PRIVATE jsapi_rpc jsapi_host_nodes)
jsapi_setup_target(jsapi_self_test)

# ----------------------------------------------------------------------------
# jsapi_bridge：CEF 渲染进程/V8 与浏览器进程之间的桥
# ----------------------------------------------------------------------------
if(EXISTS "${CEF_ROOT}/include/cef_app.h")
  set(CEF_INCLUDE_PATH "${CEF_ROOT}")
  add_library(jsapi_bridge STATIC
    src/bridge/jsapi_render_handler.cpp
    src/bridge/jsapi_browser_bridge.cpp
  )
  target_include_directories(jsapi_bridge PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
  )
  target_include_directories(jsapi_bridge PRIVATE ${CEF_ROOT})
  target_link_libraries(jsapi_bridge PUBLIC jsapi_core jsapi_rpc)
  jsapi_setup_target(jsapi_bridge)
else()
  message(WARNING "CEF_ROOT not found (${CEF_ROOT}), jsapi_bridge / qt host skipped")
endif()

# ----------------------------------------------------------------------------
# Qt 宿主：把 CEF 嵌入 Qt 窗口，加载 resources/jsapi_demo.html
# ----------------------------------------------------------------------------
option(JSAPI_BUILD_QT_HOST "Build the Qt + CEF demo host" ON)
if(JSAPI_BUILD_QT_HOST AND QT_FOUND AND EXISTS "${CEF_ROOT}/include/cef_app.h")
  find_package(Qt${QT_VERSION_MAJOR} REQUIRED COMPONENTS Widgets)
  set(CMAKE_MODULE_PATH ${CMAKE_MODULE_PATH} "${CEF_ROOT}/cmake")
  # Qt 在 Windows 上使用 /MD，CEF wrapper 默认 /MT；清空该缓存变量避免运行库不匹配。
  set(CEF_RUNTIME_LIBRARY_FLAG "" CACHE STRING
      "MSVC runtime flag used by CEF wrapper")
  find_package(CEF REQUIRED)
  add_subdirectory(apps/qt_host)
else()
  message(STATUS "Qt Widgets / CEF not found, skip jsapi_qt_host target")
endif()
