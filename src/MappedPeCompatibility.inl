// 同一份实现用于工具本身及生成程序；调用方宏选择编译代码或字符串化。
// 只恢复合法内存 PE 的可执行节权限，其它异常继续交给原有处理链。
E_PACKAGER_MAPPED_PE_CODE(
namespace mapped_pe_compatibility {
inline bool IsReadablePageProtection(const DWORD protect)
{
	if ((protect & PAGE_GUARD) != 0 || (protect & PAGE_NOACCESS) != 0) {
		return false;
	}

	switch (protect & 0xFFu) {
	case PAGE_READONLY:
	case PAGE_READWRITE:
	case PAGE_WRITECOPY:
	case PAGE_EXECUTE_READ:
	case PAGE_EXECUTE_READWRITE:
	case PAGE_EXECUTE_WRITECOPY:
		return true;
	default:
		return false;
	}
}

inline bool IsReadableMemoryRange(const void* address, size_t size)
{
	if (address == nullptr) {
		return false;
	}
	if (size == 0) {
		return true;
	}

	const auto* current = static_cast<const std::uint8_t*>(address);
	size_t remaining = size;
	while (remaining > 0) {
		MEMORY_BASIC_INFORMATION mbi = {};
		if (VirtualQuery(current, &mbi, sizeof(mbi)) != sizeof(mbi)) {
			return false;
		}
		if (mbi.State != MEM_COMMIT || !IsReadablePageProtection(mbi.Protect)) {
			return false;
		}

		const auto* regionBase = static_cast<const std::uint8_t*>(mbi.BaseAddress);
		const size_t offset = static_cast<size_t>(current - regionBase);
		if (offset >= mbi.RegionSize) {
			return false;
		}

		const size_t available = mbi.RegionSize - offset;
		if (available >= remaining) {
			return true;
		}

		current += available;
		remaining -= available;
	}

	return true;
}

// 部分旧式内存加载器漏设零原始数据代码节的执行权限（例如 UPX 解压区）。
// 仅在支持库入口的 SEH 范围内，按内存 PE 的节声明恢复代码节，不关闭进程 DEP。
inline bool RestoreExecutablePage(const EXCEPTION_RECORD& exception)
{
	if (exception.ExceptionCode != EXCEPTION_ACCESS_VIOLATION ||
		exception.NumberParameters < 2 || exception.ExceptionInformation[0] != 8 ||
		exception.ExceptionInformation[1] != reinterpret_cast<ULONG_PTR>(exception.ExceptionAddress)) {
		return false;
	}
	MEMORY_BASIC_INFORMATION memory {};
	if (VirtualQuery(exception.ExceptionAddress, &memory, sizeof(memory)) != sizeof(memory) ||
		memory.Type != MEM_PRIVATE || memory.State != MEM_COMMIT ||
		(memory.Protect != PAGE_READONLY && memory.Protect != PAGE_READWRITE)) {
		return false;
	}
	const auto* base = static_cast<const std::uint8_t*>(memory.AllocationBase);
	const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
	if (!IsReadableMemoryRange(dos, sizeof(*dos)) || dos->e_magic != IMAGE_DOS_SIGNATURE ||
		dos->e_lfanew < sizeof(*dos) || dos->e_lfanew > 0x100000) {
		return false;
	}
	const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS32*>(base + dos->e_lfanew);
	if (!IsReadableMemoryRange(nt, sizeof(*nt)) || nt->Signature != IMAGE_NT_SIGNATURE ||
		nt->FileHeader.Machine != IMAGE_FILE_MACHINE_I386 ||
		nt->OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR32_MAGIC ||
		nt->FileHeader.SizeOfOptionalHeader != sizeof(IMAGE_OPTIONAL_HEADER32) ||
		nt->FileHeader.NumberOfSections == 0 || nt->FileHeader.NumberOfSections > 96) {
		return false;
	}
	const auto rva = reinterpret_cast<ULONG_PTR>(exception.ExceptionAddress) -
		reinterpret_cast<ULONG_PTR>(base);
	if (rva < nt->OptionalHeader.SizeOfHeaders || rva >= nt->OptionalHeader.SizeOfImage) {
		return false;
	}
	const auto* sections = IMAGE_FIRST_SECTION(nt);
	if (!IsReadableMemoryRange(sections, sizeof(*sections) * nt->FileHeader.NumberOfSections)) {
		return false;
	}
	for (unsigned index = 0; index < nt->FileHeader.NumberOfSections; ++index) {
		const auto& section = sections[index];
		if ((section.Characteristics & IMAGE_SCN_MEM_EXECUTE) == 0 || rva < section.VirtualAddress) {
			continue;
		}
		// 旧加载器会复用 VirtualSize 存放映射地址，仍须以相邻节及映像边界约束范围。
		std::uint64_t end = std::min<std::uint64_t>(nt->OptionalHeader.SizeOfImage,
			static_cast<std::uint64_t>(section.VirtualAddress) +
			(std::max)(section.Misc.VirtualSize, section.SizeOfRawData));
		for (unsigned next = 0; next < nt->FileHeader.NumberOfSections; ++next) {
			if (sections[next].VirtualAddress > section.VirtualAddress) {
				end = std::min<std::uint64_t>(end, sections[next].VirtualAddress);
			}
		}
		if (rva < end) {
			// 初始化可能启动工作线程；整段代码必须在恢复执行前获得声明中的权限。
			// 按 VirtualQuery 区间保留各页读写属性，绝不越过节或分配边界。
			auto* cursor = const_cast<std::uint8_t*>(base) + section.VirtualAddress;
			auto* limit = const_cast<std::uint8_t*>(base) + end;
			while (cursor < limit) {
				MEMORY_BASIC_INFORMATION region {};
				if (VirtualQuery(cursor, &region, sizeof(region)) != sizeof(region) ||
					region.AllocationBase != base || region.State != MEM_COMMIT ||
					!IsReadablePageProtection(region.Protect)) return false;
				auto* next = (std::min)(limit, static_cast<std::uint8_t*>(region.BaseAddress) + region.RegionSize);
				DWORD protection = region.Protect;
				if (protection == PAGE_READONLY) protection = PAGE_EXECUTE_READ;
				else if (protection == PAGE_READWRITE) protection = PAGE_EXECUTE_READWRITE;
				else if (protection == PAGE_WRITECOPY) protection = PAGE_EXECUTE_WRITECOPY;
				DWORD previous = 0;
				if (protection != region.Protect && !VirtualProtect(cursor, next - cursor, protection, &previous)) return false;
				cursor = next;
			}
			return true;
		}
	}
	return false;
}

inline int FilterExecuteException(EXCEPTION_POINTERS* exception)
{
    __try {
        if (RestoreExecutablePage(*exception->ExceptionRecord)) return EXCEPTION_CONTINUE_EXECUTION;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {}
    return EXCEPTION_CONTINUE_SEARCH;
}
template<class Function, class... Arguments>
auto Call(Function function, Arguments... arguments) -> decltype(function(arguments...))
{
    __try { return function(arguments...); }
    __except (FilterExecuteException(GetExceptionInformation())) { __assume(0); }
}

}
)
