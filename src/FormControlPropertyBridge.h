#pragma once

#include "FormControlPropertyCodec.h"
#include "../thirdparty/json.hpp"

namespace e2txt {

// 属性接口只能在支持库匹配的进程位数中调用；桥接仅传递值，不传递指针。
bool InvokeFormControlWorker(const nlohmann::json& request, nlohmann::json& response, std::string* error);
// 内部 Win32 属性工作进程入口。
int RunFormControlWorker(const std::filesystem::path& request, const std::filesystem::path& response);

NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(FormControlSupportLibrary, fileName, resolvedPath)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(FormControlEventParameter, name, type, byReference)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(FormControlEventDefinition, index, name, returnType, parameters)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(FormControlPropertyDefinition, name, englishName, xmlName, dataType, state, metadataIndex, callbackIndex)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(FormControlPropertyValue, definition, kind, integerValue, doubleValue, booleanValue, textValue, binaryValue)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(FormControlPropertyCollection, definition, kind, textValues, integerValues)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(FormControlPropertyXmlNode, name, attributes, children)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(FormControlPropertySemanticData, collections, structured)

} // namespace e2txt
