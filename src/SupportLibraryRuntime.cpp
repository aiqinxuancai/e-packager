#include "SupportLibraryRuntime.h"

#include <algorithm>
#include <cstdint>

#define E_PACKAGER_MAPPED_PE_CODE(...) __VA_ARGS__
#include "MappedPeCompatibility.inl"
#undef E_PACKAGER_MAPPED_PE_CODE

namespace support_library_runtime {
namespace {


int FilterMetadataException(EXCEPTION_POINTERS* exception, DWORD& outExceptionCode)
{
#if defined(_MSC_VER) && defined(_M_IX86)
	__try {
		if (mapped_pe_compatibility::RestoreExecutablePage(*exception->ExceptionRecord)) {
			return EXCEPTION_CONTINUE_EXECUTION;
		}
	}
	__except (EXCEPTION_EXECUTE_HANDLER) {
		// 不信任第三方映像头；无效指针仍作为原始元数据异常返回。
	}
#endif
	outExceptionCode = exception->ExceptionRecord->ExceptionCode;
	return EXCEPTION_EXECUTE_HANDLER;
}

}  // namespace

const LIB_INFO* CallGetLibInfo(const PFN_GET_LIB_INFO getInfoProc, DWORD* exceptionCode)
{
	DWORD ignored = 0;
	DWORD& outExceptionCode = exceptionCode == nullptr ? ignored : *exceptionCode;
	outExceptionCode = 0;
#if defined(_MSC_VER)
	__try {
		return getInfoProc == nullptr ? nullptr : getInfoProc();
	}
	__except (FilterMetadataException(GetExceptionInformation(), outExceptionCode)) {
		return nullptr;
	}
#else
	return getInfoProc == nullptr ? nullptr : getInfoProc();
#endif
}

}  // namespace support_library_runtime
