cmake_minimum_required(VERSION 3.21)

# 由顶层 CMake 已找到 Qt 与 CEF，这里只构建宿主可执行文件。

add_subdirectory(${CEF_LIBCEF_DLL_WRAPPER_PATH} libcef_dll_wrapper)

set(JSAPI_QT_HOST_SOURCES
  main.cpp
  qt_host_app.cpp
  qt_host_client.cpp
  qt_host_widget.cpp
)

add_executable(jsapi_qt_host WIN32 ${JSAPI_QT_HOST_SOURCES})

target_include_directories(jsapi_qt_host PRIVATE
  ${CEF_ROOT}
  ${CMAKE_SOURCE_DIR}/plugins/host_nodes
)

target_compile_definitions(jsapi_qt_host PRIVATE
  WIN32
  _UNICODE
  UNICODE
  NOMINMAX
  WIN32_LEAN_AND_MEAN
  _WIN32_WINNT=0x0A00
  WINVER=0x0A00
)

if(MSVC)
  target_compile_options(jsapi_qt_host PRIVATE /utf-8)
endif()

ADD_LOGICAL_TARGET("libcef_lib" "${CEF_LIB_DEBUG}" "${CEF_LIB_RELEASE}")

target_link_libraries(jsapi_qt_host PRIVATE
  libcef_lib
  libcef_dll_wrapper
  ${CEF_STANDARD_LIBS}
  Qt${QT_VERSION_MAJOR}::Widgets
  jsapi_bridge
  jsapi_host_nodes
)

add_dependencies(jsapi_qt_host demo_plugin)

SET_CEF_TARGET_OUT_DIR()
set_target_properties(jsapi_qt_host PROPERTIES
  RUNTIME_OUTPUT_DIRECTORY ${CEF_TARGET_OUT_DIR}
  LINK_FLAGS "/STACK:0x800000"
)

COPY_FILES(jsapi_qt_host "${CEF_BINARY_FILES}" "${CEF_BINARY_DIR}" "${CEF_TARGET_OUT_DIR}")
COPY_FILES(jsapi_qt_host "${CEF_RESOURCE_FILES}" "${CEF_RESOURCE_DIR}" "${CEF_TARGET_OUT_DIR}")

# 插件 DLL 与示例页必须和宿主 exe 放在同一目录。
add_custom_command(TARGET jsapi_qt_host POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E copy_if_different
          "$<TARGET_FILE:demo_plugin>"
          "$<TARGET_FILE_DIR:jsapi_qt_host>/demo_plugin.dll"
  COMMAND ${CMAKE_COMMAND} -E copy_if_different
          "${CMAKE_SOURCE_DIR}/resources/jsapi_demo.html"
          "$<TARGET_FILE_DIR:jsapi_qt_host>/jsapi_demo.html"
  COMMAND ${CMAKE_COMMAND} -E copy_if_different
          "${CMAKE_SOURCE_DIR}/resources/jsapi_perf.html"
          "$<TARGET_FILE_DIR:jsapi_qt_host>/jsapi_perf.html"
)

# Qt 运行时 DLL（Core/Gui/Widgets）与 Windows 平台插件要和 exe 同目录，
# 否则程序启动时会找不到 Qt6Core.dll 或 "windows" 平台插件。
get_target_property(_qt_core_location Qt${QT_VERSION_MAJOR}::Core IMPORTED_LOCATION_RELEASE)
if(NOT _qt_core_location)
  get_target_property(_qt_core_location Qt${QT_VERSION_MAJOR}::Core IMPORTED_LOCATION)
endif()
get_filename_component(_qt_bin_dir "${_qt_core_location}" DIRECTORY)
get_filename_component(_qt_root_dir "${_qt_bin_dir}" DIRECTORY)

add_custom_command(TARGET jsapi_qt_host POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E make_directory
          "$<TARGET_FILE_DIR:jsapi_qt_host>/platforms"
  COMMAND ${CMAKE_COMMAND} -E copy_if_different
          "$<TARGET_FILE:Qt${QT_VERSION_MAJOR}::Core>"
          "$<TARGET_FILE_DIR:jsapi_qt_host>"
  COMMAND ${CMAKE_COMMAND} -E copy_if_different
          "$<TARGET_FILE:Qt${QT_VERSION_MAJOR}::Gui>"
          "$<TARGET_FILE_DIR:jsapi_qt_host>"
  COMMAND ${CMAKE_COMMAND} -E copy_if_different
          "$<TARGET_FILE:Qt${QT_VERSION_MAJOR}::Widgets>"
          "$<TARGET_FILE_DIR:jsapi_qt_host>"
  COMMAND ${CMAKE_COMMAND} -E copy_if_different
          "${_qt_root_dir}/plugins/platforms/qwindows.dll"
          "$<TARGET_FILE_DIR:jsapi_qt_host>/platforms/qwindows.dll"
)
