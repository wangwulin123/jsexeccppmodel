#pragma once

// jsapi_cef.dll 的导出宏。
// 构建该 DLL 时由 CMake 定义 JSAPI_CEF_BUILD_DLL；静态链接场景定义 JSAPI_STATIC_LINK。
#if defined(_WIN32)
#  if defined(JSAPI_STATIC_LINK)
#    define JSAPI_CEF_API
#  elif defined(JSAPI_CEF_BUILD_DLL)
#    define JSAPI_CEF_API __declspec(dllexport)
#  else
#    define JSAPI_CEF_API __declspec(dllimport)
#  endif
#else
#  define JSAPI_CEF_API
#endif
