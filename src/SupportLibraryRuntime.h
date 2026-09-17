#pragma once

#include <Windows.h>
#include <lib2.h>

namespace support_library_runtime {

// 统一调用支持库元数据入口，隔离异常并兼容旧式内存 PE 加载器。
const LIB_INFO* CallGetLibInfo(PFN_GET_LIB_INFO procedure, DWORD* exceptionCode = nullptr);

}  // namespace support_library_runtime
