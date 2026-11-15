#include "jsapi/js_table.h"

namespace jsapi {

const JsMember* JsTable::FindByName(const std::string& name) const {
  auto it = name_to_index_.find(name);
  if (it == name_to_index_.end()) {
    return nullptr;
  }
  return &members_[static_cast<std::size_t>(it->second)];
}

const JsMember* JsTable::FindByIndex(int index) const {
  if (index < 0 || static_cast<std::size_t>(index) >= members_.size()) {
    return nullptr;
  }
  return &members_[static_cast<std::size_t>(index)];
}

JsMember& JsTableBuilder::AddMember(const std::string& name) {
  for (auto& member : table_.members_) {
    if (member.name == name) {
      // 同名重复声明视为覆盖，索引保持不变，保证前后端拿到的 idx 稳定。
      return member;
    }
  }
  JsMember member;
  member.name = name;
  member.index = static_cast<int>(table_.members_.size());
  table_.members_.push_back(std::move(member));
  return table_.members_.back();
}

JsTableBuilder& JsTableBuilder::Function(const std::string& name,
                                         JsType return_type,
                                         std::vector<Param> params,
                                         JsFunction function,
                                         bool async,
                                         ThreadAffinity affinity) {
  JsMember& member = AddMember(name);
  member.is_property = false;
  member.type = return_type;
  member.params = std::move(params);
  member.async = async;
  member.affinity = affinity;
  member.function = std::move(function);
  member.getter = nullptr;
  member.setter = nullptr;
  member.writable = false;
  return *this;
}

JsTableBuilder& JsTableBuilder::AsyncFunction(const std::string& name,
                                              JsType return_type,
                                              std::vector<Param> params,
                                              JsFunction function,
                                              ThreadAffinity affinity) {
  return Function(name, return_type, std::move(params), std::move(function),
                  /*async=*/true, affinity);
}

JsTableBuilder& JsTableBuilder::Property(const std::string& name,
                                         JsType type,
                                         JsGetter getter,
                                         JsSetter setter,
                                         ThreadAffinity affinity) {
  JsMember& member = AddMember(name);
  member.is_property = true;
  member.type = type;
  member.params.clear();
  member.async = false;
  member.affinity = affinity;
  member.function = nullptr;
  member.getter = std::move(getter);
  member.setter = std::move(setter);
  member.writable = static_cast<bool>(member.setter);
  return *this;
}

std::shared_ptr<const JsTable> JsTableBuilder::Build() {
  table_.name_to_index_.clear();
  for (const auto& member : table_.members_) {
    table_.name_to_index_[member.name] = member.index;
  }
  return std::make_shared<const JsTable>(std::move(table_));
}

}  // namespace jsapi
