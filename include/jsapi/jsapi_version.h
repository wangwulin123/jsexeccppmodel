#pragma once

// JSAPI SDK 的 ABI 版本。
//
// 头文件与动态库必须来自同一次构建：ABI 不一致时二进制布局可能变化，
// 表现为句柄错乱或直接崩溃。宿主/插件在启动日志里打印该值便于对账。
//
// 层与层的依赖方向固定为（箭头指向被依赖方）：
//   jsapi_qt.dll  ->  jsapi_cef.dll  ->  jsapi.dll
//
// 只有 jsapi.dll 的头文件允许出现在业务插件与业务 exe 里；
// jsapi_cef / jsapi_qt 的头文件仅面向宿主集成方。
#define JSAPI_ABI_VERSION 1
