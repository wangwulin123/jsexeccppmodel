#pragma once

// jsapi_qt.dll 的导出宏。构建时由 CMake 定义 JSAPI_QT_BUILD_DLL。
#if defined(_WIN32)
#  if defined(JSAPI_STATIC_LINK)
#    define JSAPI_QT_API
#  elif defined(JSAPI_QT_BUILD_DLL)
#    define JSAPI_QT_API __declspec(dllexport)
#  else
#    define JSAPI_QT_API __declspec(dllimport)
#  endif
#else
#  define JSAPI_QT_API
#endif
