#pragma once

#include "ProjectSystemInfo.h"
#include "../thirdparty/json.hpp"

namespace e2txt {

// 系统信息以可读字段持久化，读取时严格检查整数范围、数组长度和扩展字节。
nlohmann::json ProjectSystemInfoToJson(const ProjectSystemInfo& info);
bool ProjectSystemInfoFromJson(const nlohmann::json& json, ProjectSystemInfo& info, std::string* error);

} // namespace e2txt
