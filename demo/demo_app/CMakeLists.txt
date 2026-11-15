# 业务层示例可执行文件：只依赖伞形 DLL（jsapi_qt）与业务节点库。
# jsapi / Qt::Widgets 由 jsapi_qt 的 PUBLIC 依赖传递获得，此处不再重复声明。
# 这里没有任何 CEF target、没有 CEF include 路径。

add_executable(demo_app WIN32 main.cpp)

target_link_libraries(demo_app PRIVATE
  jsapi_qt
  jsapi_host_nodes
)

jsapi_setup_target(demo_app)
jsapi_assert_no_cef(demo_app)

# 与 CEF 二进制同目录，保证 libcef.dll / icudtl.dat / locales 等可被找到。
set(JSAPI_APP_OUT_DIR "${CMAKE_BINARY_DIR}/bin/$<CONFIGURATION>")
set_target_properties(demo_app PROPERTIES
  RUNTIME_OUTPUT_DIRECTORY "${JSAPI_APP_OUT_DIR}"
  LINK_FLAGS "/STACK:0x800000"
)

COPY_FILES(demo_app "${CEF_BINARY_FILES}" "${CEF_BINARY_DIR}" "${JSAPI_APP_OUT_DIR}")
COPY_FILES(demo_app "${CEF_RESOURCE_FILES}" "${CEF_RESOURCE_DIR}" "${JSAPI_APP_OUT_DIR}")

add_dependencies(demo_app demo_plugin)

# 插件 DLL、示例页、Qt 运行时必须和 exe 同目录。
add_custom_command(TARGET demo_app POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E copy_if_different
          "$<TARGET_FILE:demo_plugin>"
          "$<TARGET_FILE_DIR:demo_app>/demo_plugin.dll"
  COMMAND ${CMAKE_COMMAND} -E copy_if_different
          "${CMAKE_SOURCE_DIR}/demo/resources/jsapi_demo.html"
          "$<TARGET_FILE_DIR:demo_app>/jsapi_demo.html"
  COMMAND ${CMAKE_COMMAND} -E copy_if_different
          "${CMAKE_SOURCE_DIR}/demo/resources/jsapi_perf.html"
          "$<TARGET_FILE_DIR:demo_app>/jsapi_perf.html"
)

get_target_property(_qt_core_location Qt${QT_VERSION_MAJOR}::Core IMPORTED_LOCATION_RELEASE)
if(NOT _qt_core_location)
  get_target_property(_qt_core_location Qt${QT_VERSION_MAJOR}::Core IMPORTED_LOCATION)
endif()
get_filename_component(_qt_bin_dir "${_qt_core_location}" DIRECTORY)
get_filename_component(_qt_root_dir "${_qt_bin_dir}" DIRECTORY)

add_custom_command(TARGET demo_app POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E make_directory
          "$<TARGET_FILE_DIR:demo_app>/platforms"
  COMMAND ${CMAKE_COMMAND} -E copy_if_different
          "$<TARGET_FILE:Qt${QT_VERSION_MAJOR}::Core>"
          "$<TARGET_FILE_DIR:demo_app>"
  COMMAND ${CMAKE_COMMAND} -E copy_if_different
          "$<TARGET_FILE:Qt${QT_VERSION_MAJOR}::Gui>"
          "$<TARGET_FILE_DIR:demo_app>"
  COMMAND ${CMAKE_COMMAND} -E copy_if_different
          "$<TARGET_FILE:Qt${QT_VERSION_MAJOR}::Widgets>"
          "$<TARGET_FILE_DIR:demo_app>"
  COMMAND ${CMAKE_COMMAND} -E copy_if_different
          "$<IF:$<CONFIG:Debug>,${_qt_root_dir}/plugins/platforms/qwindowsd.dll,${_qt_root_dir}/plugins/platforms/qwindows.dll>"
          "$<TARGET_FILE_DIR:demo_app>/platforms/$<IF:$<CONFIG:Debug>,qwindowsd.dll,qwindows.dll>"
)
