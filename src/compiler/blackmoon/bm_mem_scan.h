// BlackMoonNG MIT 适配层：独立转换不读取 IDE 内存中的 DLL 导出修饰信息。
#pragma once

#include <windows.h>

namespace bm {

struct EDllExport {
	char Name[256] = {};
	int Cdecl = 0;
	DWORD ParamCount = 0;
};

class EDllExportInfo {
public:
	EDllExport* getByOrgName(const char* name);
};

extern EDllExportInfo g_eDllExportInfo;

}  // namespace bm
