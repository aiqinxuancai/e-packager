#pragma once

#include <Windows.h>
#include <lib2.h>

namespace support_library_runtime {

// SDK 中窗口和菜单均使用属性表；菜单不一定带有 LDT_WIN_UNIT 标志。
// 这里只判定成员表种类，属性表的长度和内存可读性由调用方校验。
inline bool UsesPropertyTable(const LIB_DATA_TYPE_INFO& dataType)
{
    return (dataType.m_dwState & LDT_ENUM) == 0 &&
        ((dataType.m_dwState & LDT_WIN_UNIT) != 0 ||
         (dataType.m_nElementCount == 0 && dataType.m_pElementBegin == nullptr &&
          dataType.m_nPropertyCount > 0 && dataType.m_pPropertyBegin != nullptr));
}

// 统一调用支持库元数据入口，隔离异常并兼容旧式内存 PE 加载器。
const LIB_INFO* CallGetLibInfo(PFN_GET_LIB_INFO procedure, DWORD* exceptionCode = nullptr);

}  // namespace support_library_runtime
