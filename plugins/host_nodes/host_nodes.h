#pragma once

#include <memory>

#include "jsapi/js_object.h"

namespace jsapi_demo {

// 静态注册示例：直接编译进宿主程序，不经过动态库。
std::shared_ptr<jsapi::JsObjectBase> CreateHostNode();

}  // namespace jsapi_demo
