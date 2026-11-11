#include "jsapi_rpc/jsapi_rpc.h"

namespace jsapi {

bool CoerceValue(const JsValue& input,
                 JsType type,
                 JsValue* output,
                 std::string* error) {
  if (type == JsType::Any) {
    *output = input;
    return true;
  }

  auto fail = [&](const char* expected) {
    if (error) {
      *error = std::string("expected ") + expected + ", got " +
               TypeName(input.type());
    }
    return false;
  };

  switch (type) {
    case JsType::Bool:
      if (input.type() == JsType::Bool || input.type() == JsType::Int ||
          input.type() == JsType::Double) {
        *output = JsValue::Bool(input.AsBool());
        return true;
      }
      return fail("bool");
    case JsType::Int:
      if (input.type() == JsType::Int || input.type() == JsType::Double ||
          input.type() == JsType::Bool) {
        *output = JsValue::Int(input.AsInt());
        return true;
      }
      return fail("int");
    case JsType::Double:
      if (input.type() == JsType::Int || input.type() == JsType::Double) {
        *output = JsValue::Double(input.AsDouble());
        return true;
      }
      return fail("double");
    case JsType::String:
      if (input.type() == JsType::String) {
        *output = input;
        return true;
      }
      return fail("string");
    case JsType::Array:
      if (input.type() == JsType::Array) {
        *output = input;
        return true;
      }
      return fail("array");
    case JsType::Object:
      if (input.type() == JsType::Object) {
        *output = input;
        return true;
      }
      return fail("object");
    case JsType::ObjectHandle:
      if (input.type() == JsType::ObjectHandle) {
        *output = input;
        return true;
      }
      return fail("objhandle");
    case JsType::CallbackHandle:
      if (input.type() == JsType::CallbackHandle) {
        *output = input;
        return true;
      }
      return fail("cbhandle");
    case JsType::Null:
      *output = JsValue::Null();
      return true;
    default:
      return fail("value");
  }
}

bool RpcHost::ParseRequest(const std::string& json,
                           RpcRequest* request,
                           std::string* error) {
  JsValue root;
  if (!ParseJson(json, &root, error)) {
    return false;
  }
  return ParseRequest(root, request, error);
}

bool RpcHost::ParseRequest(const JsValue& root,
                           RpcRequest* request,
                           std::string* error) {
  if (root.type() != JsType::Object) {
    if (error) *error = "request must be an object";
    return false;
  }

  RpcRequest out;
  if (const JsValue* value = root.Find("seq"); value) {
    out.seq = static_cast<int>(value->AsInt());
  }
  if (const JsValue* value = root.Find("op"); value) {
    out.op = value->AsString();
  }
  if (const JsValue* value = root.Find("handle"); value) {
    out.handle = static_cast<int>(value->AsInt(-1));
  }
  if (const JsValue* value = root.Find("index"); value) {
    out.index = static_cast<int>(value->AsInt(-1));
  }
  if (const JsValue* value = root.Find("async"); value) {
    out.async = value->AsBool();
  }
  if (const JsValue* value = root.Find("args"); value) {
    out.args = value->AsArray();
  }
  if (const JsValue* value = root.Find("env"); value) {
    if (value->type() == JsType::Object) {
      if (const JsValue* browser = value->Find("browser"); browser) {
        out.env.browser_id = static_cast<int>(browser->AsInt(-1));
      }
      if (const JsValue* frame = value->Find("frame"); frame) {
        out.env.frame_id = frame->AsString();
      }
    }
  }

  if (out.op.empty()) {
    if (error) *error = "request missing op";
    return false;
  }
  if (error) error->clear();
  *request = std::move(out);
  return true;
}

std::string RpcHost::EncodeRequest(
    const RpcRequest& request,
    const std::map<std::string, JsValue>& extras) {
  JsValue root = JsValue::Object();
  root.Set("seq", JsValue::Int(request.seq));
  root.Set("op", JsValue::String(request.op));
  root.Set("handle", JsValue::Int(request.handle));
  root.Set("index", JsValue::Int(request.index));
  root.Set("async", JsValue::Bool(request.async));
  root.Set("args", JsValue::Array(request.args));

  JsValue env = JsValue::Object();
  env.Set("browser", JsValue::Int(request.env.browser_id));
  env.Set("frame", JsValue::String(request.env.frame_id));
  root.Set("env", std::move(env));

  for (const auto& extra : extras) {
    root.Set(extra.first, extra.second);
  }
  return root.ToJson();
}

std::string RpcHost::EncodeResponse(const RpcResponse& response) {
  JsValue root = JsValue::Object();
  root.Set("seq", JsValue::Int(response.seq));
  root.Set("ok", JsValue::Bool(response.ok));
  if (response.ok) {
    root.Set("value", response.value);
  } else {
    root.Set("error", JsValue::String(response.error));
  }
  return root.ToJson();
}

RpcResponse RpcHost::Process(const RpcRequest& request) {
  RpcResponse response;
  response.seq = request.seq;

  auto object = registry_ ? registry_->GetObjectByHandle(request.handle) : nullptr;
  if (!object) {
    response.ok = false;
    response.error = "invalid object handle: " + std::to_string(request.handle);
    return response;
  }

  if (request.op == "describe") {
    response.value = registry_->Describe(request.handle);
    return response;
  }

  const JsMember* member = object->GetTable().FindByIndex(request.index);
  if (!member) {
    response.ok = false;
    response.error = "invalid member index: " + std::to_string(request.index);
    return response;
  }

  if (request.op == "get") {
    if (!member->is_property) {
      response.ok = false;
      response.error = "'" + member->name + "' is not a property";
      return response;
    }
    if (!member->getter) {
      response.ok = false;
      response.error = "property '" + member->name + "' has no getter";
      return response;
    }
    response.value = member->getter(object.get());
    return response;
  }

  if (request.op == "set") {
    if (!member->is_property) {
      response.ok = false;
      response.error = "'" + member->name + "' is not a property";
      return response;
    }
    if (!member->setter) {
      response.ok = false;
      response.error = "property '" + member->name + "' is read-only";
      return response;
    }
    if (request.args.empty()) {
      response.ok = false;
      response.error = "set requires one value argument";
      return response;
    }
    JsValue value;
    std::string error;
    if (!CoerceValue(request.args[0], member->type, &value, &error)) {
      response.ok = false;
      response.error = "property '" + member->name + "': " + error;
      return response;
    }
    std::string set_error;
    if (!member->setter(object.get(), value, &set_error)) {
      response.ok = false;
      response.error = set_error.empty()
                           ? "failed to set property '" + member->name + "'"
                           : set_error;
      return response;
    }
    response.value = JsValue::Bool(true);
    return response;
  }

  if (request.op == "execute") {
    if (member->is_property) {
      response.ok = false;
      response.error = "'" + member->name + "' is not a function";
      return response;
    }
    if (!member->function) {
      response.ok = false;
      response.error = "function '" + member->name + "' is not implemented";
      return response;
    }

    std::size_t required = 0;
    for (const auto& param : member->params) {
      if (!param.optional) {
        required++;
      }
    }
    if (request.args.size() < required ||
        request.args.size() > member->params.size()) {
      response.ok = false;
      response.error = "function '" + member->name + "' expects " +
                       std::to_string(required) + ".." +
                       std::to_string(member->params.size()) + " args, got " +
                       std::to_string(request.args.size());
      return response;
    }

    std::vector<JsValue> args;
    args.reserve(request.args.size());
    for (std::size_t i = 0; i < request.args.size(); ++i) {
      JsValue coerced;
      std::string error;
      if (!CoerceValue(request.args[i], member->params[i].type, &coerced,
                       &error)) {
        response.ok = false;
        response.error = "function '" + member->name + "' arg '" +
                         member->params[i].name + "': " + error;
        return response;
      }
      args.push_back(std::move(coerced));
    }

    JsCallContext ctx(object.get(), std::move(args), registry_, request.env);
    member->function(object.get(), ctx);
    if (ctx.has_error()) {
      response.ok = false;
      response.error = ctx.error();
    } else {
      response.value = ctx.has_value() ? ctx.value() : JsValue::Null();
    }
    return response;
  }

  response.ok = false;
  response.error = "unknown op: " + request.op;
  return response;
}

}  // namespace jsapi
