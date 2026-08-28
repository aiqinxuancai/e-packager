// BlackMoonNG MIT 适配层：文件编译没有 IDE 内存中的导出名称覆盖信息。
#include "bm_mem_scan.h"

namespace bm {

EDllExportInfo g_eDllExportInfo;

EDllExport* EDllExportInfo::getByOrgName(const char*)
{
	return nullptr;
}

}  // namespace bm
