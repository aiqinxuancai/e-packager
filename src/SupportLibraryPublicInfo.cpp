#include "SupportLibraryPublicInfo.h"

#include <Windows.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdint>
#include <fstream>
#include <optional>
#include <sstream>
#include <unordered_map>
#include <unordered_set>

#include <lib2.h>

#include "PathHelper.h"

namespace support_library_public_info {

namespace {

constexpr size_t kMaxSupportLibraryStringLength = 4096;
constexpr int kMaxSupportLibraryArrayCount = 16384;

std::string TrimAsciiCopy(std::string text)
{
	size_t begin = 0;
	while (begin < text.size() && static_cast<unsigned char>(text[begin]) <= 0x20) {
		++begin;
	}

	size_t end = text.size();
	while (end > begin && static_cast<unsigned char>(text[end - 1]) <= 0x20) {
		--end;
	}
	return text.substr(begin, end - begin);
}

std::string ToLowerAsciiCopy(std::string text)
{
	std::transform(text.begin(), text.end(), text.begin(), [](const unsigned char ch) {
		return static_cast<char>(std::tolower(ch));
	});
	return text;
}

std::string NormalizeCrLf(const std::string& text)
{
	std::string normalized;
	normalized.reserve(text.size() + 16);
	for (size_t index = 0; index < text.size(); ++index) {
		const char ch = text[index];
		if (ch == '\r') {
			normalized.append("\r\n");
			if (index + 1 < text.size() && text[index + 1] == '\n') {
				++index;
			}
		}
		else if (ch == '\n') {
			normalized.append("\r\n");
		}
		else {
			normalized.push_back(ch);
		}
	}
	return normalized;
}

std::string ConvertCodePage(
	const std::string& text,
	const UINT fromCodePage,
	const UINT toCodePage,
	const DWORD fromFlags)
{
	if (text.empty() || fromCodePage == toCodePage) {
		return text;
	}

	const int wideLen = MultiByteToWideChar(
		fromCodePage,
		fromFlags,
		text.data(),
		static_cast<int>(text.size()),
		nullptr,
		0);
	if (wideLen <= 0) {
		return text;
	}

	std::wstring wide(static_cast<size_t>(wideLen), L'\0');
	if (MultiByteToWideChar(
			fromCodePage,
			fromFlags,
			text.data(),
			static_cast<int>(text.size()),
			wide.data(),
			wideLen) <= 0) {
		return text;
	}

	const int outLen = WideCharToMultiByte(
		toCodePage,
		0,
		wide.data(),
		wideLen,
		nullptr,
		0,
		nullptr,
		nullptr);
	if (outLen <= 0) {
		return text;
	}

	std::string out(static_cast<size_t>(outLen), '\0');
	if (WideCharToMultiByte(
			toCodePage,
			0,
			wide.data(),
			wideLen,
			out.data(),
			outLen,
			nullptr,
			nullptr) <= 0) {
		return text;
	}

	return out;
}

std::string LocalToUtf8Text(const std::string& text)
{
	return ConvertCodePage(text, CP_ACP, CP_UTF8, 0);
}

bool WriteUtf8TextFileBom(const std::filesystem::path& path, const std::string& utf8Text)
{
	std::error_code ec;
	if (path.has_parent_path()) {
		std::filesystem::create_directories(path.parent_path(), ec);
		if (ec) {
			return false;
		}
	}

	std::ofstream out(path, std::ios::binary | std::ios::trunc);
	if (!out.is_open()) {
		return false;
	}

	static constexpr unsigned char kBom[] = {0xEF, 0xBB, 0xBF};
	out.write(reinterpret_cast<const char*>(kBom), sizeof(kBom));
	const std::string normalized = LocalToUtf8Text(NormalizeCrLf(utf8Text));
	if (!normalized.empty()) {
		out.write(normalized.data(), static_cast<std::streamsize>(normalized.size()));
	}
	return out.good();
}

std::string PathToGenericUtf8(const std::filesystem::path& path)
{
	return WideToUtf8Text(path.generic_wstring());
}

std::string SanitizeFileName(std::string name)
{
	for (char& ch : name) {
		const unsigned char byte = static_cast<unsigned char>(ch);
		if (byte < 0x20 ||
			ch == '<' || ch == '>' || ch == ':' || ch == '"' ||
			ch == '/' || ch == '\\' || ch == '|' || ch == '?' || ch == '*') {
			ch = '_';
		}
	}

	while (!name.empty() && (name.back() == ' ' || name.back() == '.')) {
		name.pop_back();
	}
	while (!name.empty() && (name.front() == ' ' || name.front() == '.')) {
		name.erase(name.begin());
	}
	return name.empty() ? std::string("support_library") : name;
}

void PushUniqueCandidate(std::vector<std::filesystem::path>& candidates, const std::filesystem::path& candidate)
{
	if (candidate.empty()) {
		return;
	}

	const auto normalized = candidate.lexically_normal();
	for (const auto& item : candidates) {
		if (item.lexically_normal() == normalized) {
			return;
		}
	}
	candidates.push_back(normalized);
}

std::vector<std::filesystem::path> BuildLibraryFileVariants(const std::string& libraryFileName)
{
	std::vector<std::filesystem::path> variants;
	if (libraryFileName.empty()) {
		return variants;
	}

	std::filesystem::path filePath(TrimAsciiCopy(libraryFileName));
	if (filePath.empty()) {
		return variants;
	}

	if (filePath.has_extension()) {
		variants.push_back(filePath);
		return variants;
	}

	variants.push_back(filePath);
	variants.push_back(filePath.string() + ".fne");
	variants.push_back(filePath.string() + ".fnr");
	variants.push_back(filePath.string() + ".dll");
	return variants;
}

std::vector<std::filesystem::path> BuildSupportLibraryCandidatePaths(
	const std::filesystem::path& sourcePath,
	const std::string& libraryFileName)
{
	std::vector<std::filesystem::path> candidates;
	const auto fileVariants = BuildLibraryFileVariants(libraryFileName);
	if (fileVariants.empty()) {
		return candidates;
	}

	const auto addBaseCandidates = [&](const std::filesystem::path& baseDir) {
		if (baseDir.empty()) {
			return;
		}

		for (const auto& variant : fileVariants) {
			PushUniqueCandidate(candidates, baseDir / variant);
			PushUniqueCandidate(candidates, baseDir / "lib" / variant);

			std::filesystem::path current = baseDir;
			while (!current.empty()) {
				PushUniqueCandidate(candidates, current / "lib" / variant);
				if (current == current.root_path()) {
					break;
				}
				current = current.parent_path();
			}
		}
	};

	for (const auto& variant : fileVariants) {
		if (variant.is_absolute()) {
			PushUniqueCandidate(candidates, variant);
		}
	}
	if (!candidates.empty()) {
		return candidates;
	}

	std::error_code ec;
	if (!sourcePath.empty()) {
		addBaseCandidates(sourcePath.parent_path());
	}
	addBaseCandidates(std::filesystem::current_path(ec));
	addBaseCandidates(std::filesystem::path(GetBasePath()));
	for (const auto& registeredBaseDir : GetRegisteredEplOpenCommandBaseDirs()) {
		addBaseCandidates(registeredBaseDir);
	}

	return candidates;
}

bool ResolveSupportLibraryPath(
	const std::filesystem::path& sourcePath,
	const std::string& libraryFileName,
	std::filesystem::path& outResolvedPath)
{
	outResolvedPath.clear();
	for (const auto& candidate : BuildSupportLibraryCandidatePaths(sourcePath, libraryFileName)) {
		std::error_code ec;
		if (!std::filesystem::exists(candidate, ec)) {
			continue;
		}
		outResolvedPath = candidate;
		return true;
	}
	return false;
}

bool IsReadablePageProtection(const DWORD protect)
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

bool IsReadableMemoryRange(const void* address, size_t size)
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

size_t GetSafeCStringLength(const char* text, const size_t maxLength)
{
	if (text == nullptr) {
		return 0;
	}

#if defined(_MSC_VER)
	size_t length = 0;
	__try {
		for (; length < maxLength; ++length) {
			if (text[length] == '\0') {
				break;
			}
		}
	}
	__except (EXCEPTION_EXECUTE_HANDLER) {
		return static_cast<size_t>(-1);
	}
	return length;
#else
	size_t index = 0;
	for (; index < maxLength; ++index) {
		if (text[index] == '\0') {
			return index;
		}
	}
	return index;
#endif
}

std::string ReadAnsiText(const char* text)
{
	const size_t length = GetSafeCStringLength(text, kMaxSupportLibraryStringLength);
	if (length == static_cast<size_t>(-1)) {
		return std::string();
	}
	return text == nullptr ? std::string() : std::string(text, length);
}

const LIB_INFO* CallGetLibInfoSafely(const PFN_GET_LIB_INFO getInfoProc)
{
#if defined(_MSC_VER)
	__try {
		return getInfoProc == nullptr ? nullptr : getInfoProc();
	}
	__except (EXCEPTION_EXECUTE_HANDLER) {
		return nullptr;
	}
#else
	return getInfoProc == nullptr ? nullptr : getInfoProc();
#endif
}

std::vector<std::string> BuildSupportTypeMemberNames(const LIB_DATA_TYPE_INFO& dataType)
{
	std::vector<std::string> memberNames;
	const bool isWinUnit =
		(dataType.m_dwState & LDT_WIN_UNIT) != 0 &&
		(dataType.m_dwState & LDT_ENUM) == 0;
	if (isWinUnit) {
		if (dataType.m_nPropertyCount > 0 &&
			dataType.m_nPropertyCount <= kMaxSupportLibraryArrayCount &&
			dataType.m_pPropertyBegin != nullptr &&
			IsReadableMemoryRange(
				dataType.m_pPropertyBegin,
				sizeof(UNIT_PROPERTY) * static_cast<size_t>(dataType.m_nPropertyCount))) {
			memberNames.reserve(static_cast<size_t>(dataType.m_nPropertyCount));
			for (int propertyIndex = 0; propertyIndex < dataType.m_nPropertyCount; ++propertyIndex) {
				memberNames.emplace_back(ReadAnsiText(dataType.m_pPropertyBegin[propertyIndex].m_szName));
			}
			return memberNames;
		}

		static constexpr std::array<const char*, FIXED_WIN_UNIT_PROPERTY_COUNT> kFixedWinUnitPropertyNames = {
			"左边",
			"顶边",
			"宽度",
			"高度",
			"标记",
			"可视",
			"禁止",
			"鼠标指针",
		};
		memberNames.reserve(kFixedWinUnitPropertyNames.size());
		for (const char* name : kFixedWinUnitPropertyNames) {
			memberNames.emplace_back(name == nullptr ? "" : name);
		}
		return memberNames;
	}

	if (dataType.m_nElementCount > 0 &&
		dataType.m_nElementCount <= kMaxSupportLibraryArrayCount &&
		dataType.m_pElementBegin != nullptr &&
		IsReadableMemoryRange(
			dataType.m_pElementBegin,
			sizeof(LIB_DATA_TYPE_ELEMENT) * static_cast<size_t>(dataType.m_nElementCount))) {
		memberNames.reserve(static_cast<size_t>(dataType.m_nElementCount));
		for (int memberIndex = 0; memberIndex < dataType.m_nElementCount; ++memberIndex) {
			memberNames.emplace_back(ReadAnsiText(dataType.m_pElementBegin[memberIndex].m_szName));
		}
	}
	return memberNames;
}

std::vector<std::string> BuildSupportTypeEventNames(const LIB_DATA_TYPE_INFO& dataType)
{
	std::vector<std::string> eventNames;
	if (dataType.m_nEventCount <= 0 ||
		dataType.m_nEventCount > kMaxSupportLibraryArrayCount ||
		dataType.m_pEventBegin == nullptr ||
		!IsReadableMemoryRange(
			dataType.m_pEventBegin,
			sizeof(EVENT_INFO2) * static_cast<size_t>(dataType.m_nEventCount))) {
		return eventNames;
	}

	eventNames.reserve(static_cast<size_t>(dataType.m_nEventCount));
	for (int eventIndex = 0; eventIndex < dataType.m_nEventCount; ++eventIndex) {
		eventNames.emplace_back(ReadAnsiText(dataType.m_pEventBegin[eventIndex].m_szName));
	}
	return eventNames;
}

std::string DecodeSupportLibraryDataType(const int typeValue)
{
	const DATA_TYPE type = static_cast<DATA_TYPE>(typeValue);
	const bool isArray = (type & DT_IS_ARY) != 0;
	const bool isVar = (type & DT_IS_VAR) != 0;
	const DATA_TYPE baseType = static_cast<DATA_TYPE>(type & ~DT_IS_ARY);

	std::string text;
	switch (baseType) {
	case _SDT_NULL: text = "空类型"; break;
	case _SDT_ALL: text = "通用型"; break;
	case SDT_BYTE: text = "字节型"; break;
	case SDT_SHORT: text = "短整数型"; break;
	case SDT_INT: text = "整数型"; break;
	case SDT_INT64: text = "长整数型"; break;
	case SDT_FLOAT: text = "小数型"; break;
	case SDT_DOUBLE: text = "双精度小数型"; break;
	case SDT_BOOL: text = "逻辑型"; break;
	case SDT_DATE_TIME: text = "日期时间型"; break;
	case SDT_TEXT: text = "文本型"; break;
	case SDT_BIN: text = "字节集"; break;
	case SDT_SUB_PTR: text = "子程序指针"; break;
	case SDT_STATMENT: text = "子语句"; break;
	default:
	{
		std::ostringstream stream;
		stream << "库类型(0x" << std::hex << std::uppercase << static_cast<unsigned int>(baseType) << ")";
		text = stream.str();
		break;
	}
	}

	if (isArray) {
		text += "[]";
	}
	if (isVar) {
		text += "&";
	}
	return text;
}

std::string DecodeSupportLibraryConstType(const int typeValue)
{
	switch (static_cast<SHORT>(typeValue)) {
	case CT_NULL: return "空";
	case CT_NUM: return "数值";
	case CT_BOOL: return "逻辑";
	case CT_TEXT: return "文本";
	default:
		return "未知(" + std::to_string(typeValue) + ")";
	}
}

struct LoadedSupportLibraryDump {
	std::string filePath;
	std::string fileName;
	std::string name;
	std::string guid;
	std::string author;
	std::string explain;
	int majorVersion = 0;
	int minorVersion = 0;
	int buildNumber = 0;
	std::vector<std::string> lines;
};

bool TryLoadSupportLibraryDump(
	const std::filesystem::path& filePath,
	LoadedSupportLibraryDump& outDump,
	std::string& outError)
{
	outDump = {};
	outError.clear();

#if !defined(_M_IX86)
	(void)filePath;
	outError = "support_library_dump_requires_win32";
	return false;
#else
	HMODULE module = nullptr;
	const LIB_INFO* libInfo = nullptr;

	const auto closeModule = [&]() {
		if (module != nullptr) {
			FreeLibrary(module);
			module = nullptr;
		}
	};

	auto tryLoad = [&](const DWORD flags, std::string& outAttemptError) -> bool {
		module = LoadLibraryExA(filePath.string().c_str(), nullptr, flags);
		if (module == nullptr) {
			outAttemptError = "LoadLibraryEx failed";
			return false;
		}

		auto* getInfoProc = reinterpret_cast<PFN_GET_LIB_INFO>(GetProcAddress(module, FUNCNAME_GET_LIB_INFO));
		if (getInfoProc == nullptr) {
			outAttemptError = "GetNewInf not found";
			closeModule();
			return false;
		}

		libInfo = CallGetLibInfoSafely(getInfoProc);
		if (libInfo == nullptr || !IsReadableMemoryRange(libInfo, sizeof(LIB_INFO))) {
			outAttemptError = "GetNewInf returned invalid LIB_INFO";
			libInfo = nullptr;
			closeModule();
			return false;
		}

		return true;
	};

	std::string attemptError;
	if (!tryLoad(DONT_RESOLVE_DLL_REFERENCES, attemptError) && !tryLoad(0, attemptError)) {
		outError = attemptError;
		return false;
	}

	outDump.filePath = filePath.string();
	outDump.fileName = filePath.filename().string();
	outDump.name = ReadAnsiText(libInfo->m_szName);
	outDump.guid = ReadAnsiText(libInfo->m_szGuid);
	outDump.author = ReadAnsiText(libInfo->m_szAuthor);
	outDump.explain = ReadAnsiText(libInfo->m_szExplain);
	outDump.majorVersion = libInfo->m_nMajorVersion;
	outDump.minorVersion = libInfo->m_nMinorVersion;
	outDump.buildNumber = libInfo->m_nBuildNumber;

	std::vector<std::string>& lines = outDump.lines;
	if (!outDump.name.empty()) {
		lines.push_back("支持库名称：" + outDump.name);
	}
	lines.push_back(
		"版本：" +
		std::to_string(outDump.majorVersion) + "." +
		std::to_string(outDump.minorVersion) + "." +
		std::to_string(outDump.buildNumber));
	if (!outDump.author.empty()) {
		lines.push_back("作者：" + outDump.author);
	}
	lines.push_back("文件路径：" + outDump.filePath);
	if (!outDump.explain.empty()) {
		lines.push_back("说明：" + outDump.explain);
	}

	if (libInfo->m_nCmdCount > 0 &&
		libInfo->m_nCmdCount <= kMaxSupportLibraryArrayCount &&
		libInfo->m_pBeginCmdInfo != nullptr &&
		IsReadableMemoryRange(
			libInfo->m_pBeginCmdInfo,
			sizeof(CMD_INFO) * static_cast<size_t>(libInfo->m_nCmdCount))) {
		lines.push_back("");
		lines.push_back("[命令]");
		for (int i = 0; i < libInfo->m_nCmdCount; ++i) {
			const CMD_INFO& cmd = libInfo->m_pBeginCmdInfo[i];
			const std::string cmdName = ReadAnsiText(cmd.m_szName);
			std::ostringstream header;
			header << ".命令 "
				<< (cmdName.empty() ? std::string("<未命名>") : cmdName)
				<< ", " << DecodeSupportLibraryDataType(cmd.m_dtRetValType)
				<< ", 分类=" << cmd.m_shtCategory;
			lines.push_back(header.str());

			const std::string cmdExplain = ReadAnsiText(cmd.m_szExplain);
			if (!cmdExplain.empty()) {
				lines.push_back("  说明：" + cmdExplain);
			}

			if (cmd.m_nArgCount > 0 &&
				cmd.m_nArgCount <= kMaxSupportLibraryArrayCount &&
				cmd.m_pBeginArgInfo != nullptr &&
				IsReadableMemoryRange(
					cmd.m_pBeginArgInfo,
					sizeof(ARG_INFO) * static_cast<size_t>(cmd.m_nArgCount))) {
				for (int argIndex = 0; argIndex < cmd.m_nArgCount; ++argIndex) {
					const ARG_INFO& arg = cmd.m_pBeginArgInfo[argIndex];
					const std::string argName = ReadAnsiText(arg.m_szName);
					const std::string argExplain = ReadAnsiText(arg.m_szExplain);
					std::string argLine =
						"  .参数 " +
						std::string(argName.empty() ? "<未命名>" : argName) +
						", " + DecodeSupportLibraryDataType(arg.m_dtType);
					if (!argExplain.empty()) {
						argLine += ", " + argExplain;
					}
					lines.push_back(std::move(argLine));
				}
			}
		}
	}

	if (libInfo->m_nDataTypeCount > 0 &&
		libInfo->m_nDataTypeCount <= kMaxSupportLibraryArrayCount &&
		libInfo->m_pDataType != nullptr &&
		IsReadableMemoryRange(
			libInfo->m_pDataType,
			sizeof(LIB_DATA_TYPE_INFO) * static_cast<size_t>(libInfo->m_nDataTypeCount))) {
		lines.push_back("");
		lines.push_back("[数据类型]");
		for (int i = 0; i < libInfo->m_nDataTypeCount; ++i) {
			const LIB_DATA_TYPE_INFO& dataType = libInfo->m_pDataType[i];
			const std::string typeName = ReadAnsiText(dataType.m_szName);
			lines.push_back(".数据类型 " + std::string(typeName.empty() ? "<未命名>" : typeName));

			const std::string typeExplain = ReadAnsiText(dataType.m_szExplain);
			if (!typeExplain.empty()) {
				lines.push_back("  说明：" + typeExplain);
			}

			for (const auto& memberName : BuildSupportTypeMemberNames(dataType)) {
				lines.push_back("  .成员 " + (memberName.empty() ? std::string("<未命名>") : memberName));
			}
			for (const auto& eventName : BuildSupportTypeEventNames(dataType)) {
				lines.push_back("  .事件 " + (eventName.empty() ? std::string("<未命名>") : eventName));
			}
			if (dataType.m_nCmdCount > 0 &&
				dataType.m_nCmdCount <= kMaxSupportLibraryArrayCount &&
				dataType.m_pnCmdsIndex != nullptr &&
				IsReadableMemoryRange(
					dataType.m_pnCmdsIndex,
					sizeof(int) * static_cast<size_t>(dataType.m_nCmdCount)) &&
				libInfo->m_pBeginCmdInfo != nullptr &&
				IsReadableMemoryRange(
					libInfo->m_pBeginCmdInfo,
					sizeof(CMD_INFO) * static_cast<size_t>(libInfo->m_nCmdCount))) {
				for (int cmdIndex = 0; cmdIndex < dataType.m_nCmdCount; ++cmdIndex) {
					const int globalCmdIndex = dataType.m_pnCmdsIndex[cmdIndex];
					if (globalCmdIndex < 0 || globalCmdIndex >= libInfo->m_nCmdCount) {
						continue;
					}
					const std::string memberCommandName = ReadAnsiText(libInfo->m_pBeginCmdInfo[globalCmdIndex].m_szName);
					lines.push_back("  .成员命令 " + (memberCommandName.empty() ? std::string("<未命名>") : memberCommandName));
				}
			}
		}
	}

	if (libInfo->m_nLibConstCount > 0 &&
		libInfo->m_nLibConstCount <= kMaxSupportLibraryArrayCount &&
		libInfo->m_pLibConst != nullptr &&
		IsReadableMemoryRange(
			libInfo->m_pLibConst,
			sizeof(LIB_CONST_INFO) * static_cast<size_t>(libInfo->m_nLibConstCount))) {
		lines.push_back("");
		lines.push_back("[常量]");
		for (int i = 0; i < libInfo->m_nLibConstCount; ++i) {
			const LIB_CONST_INFO& item = libInfo->m_pLibConst[i];
			const std::string name = ReadAnsiText(item.m_szName);
			const std::string explain = ReadAnsiText(item.m_szExplain);
			const std::string textValue = ReadAnsiText(item.m_szText);
			std::ostringstream line;
			line << ".常量 "
				<< (name.empty() ? std::string("<未命名>") : name)
				<< ", " << DecodeSupportLibraryConstType(item.m_shtType);
			if (!textValue.empty()) {
				line << ", " << textValue;
			}
			else {
				line << ", " << item.m_dbValue;
			}
			if (!explain.empty()) {
				line << ", " << explain;
			}
			lines.push_back(line.str());
		}
	}

	closeModule();
	return true;
#endif
}

std::string JoinLines(const std::vector<std::string>& lines)
{
	std::ostringstream stream;
	for (size_t i = 0; i < lines.size(); ++i) {
		if (i != 0) {
			stream << "\r\n";
		}
		stream << lines[i];
	}
	return stream.str();
}

std::string BuildVersionTextMajorMinor(const LoadedSupportLibraryDump& dump)
{
	return std::to_string(dump.majorVersion) + "." + std::to_string(dump.minorVersion);
}

std::string ResolveDependencyLibraryName(const e2txt::Dependency& dependency)
{
	if (!TrimAsciiCopy(dependency.fileName).empty()) {
		return TrimAsciiCopy(dependency.fileName);
	}
	if (!TrimAsciiCopy(dependency.name).empty()) {
		return TrimAsciiCopy(dependency.name);
	}
	return std::string();
}

bool IsEquivalentDependency(const e2txt::Dependency& left, const e2txt::Dependency& right)
{
	if (left.kind != right.kind) {
		return false;
	}

	if (left.kind == e2txt::DependencyKind::ECom) {
		return ToLowerAsciiCopy(TrimAsciiCopy(left.path)) == ToLowerAsciiCopy(TrimAsciiCopy(right.path));
	}

	const std::string leftFile = ToLowerAsciiCopy(TrimAsciiCopy(left.fileName));
	const std::string rightFile = ToLowerAsciiCopy(TrimAsciiCopy(right.fileName));
	const std::string leftGuid = ToLowerAsciiCopy(TrimAsciiCopy(left.guid));
	const std::string rightGuid = ToLowerAsciiCopy(TrimAsciiCopy(right.guid));
	if (!leftGuid.empty() || !rightGuid.empty()) {
		return leftFile == rightFile && leftGuid == rightGuid;
	}
	return leftFile == rightFile;
}

}  // namespace

ExportResult ExportDependencies(
	const std::filesystem::path& sourcePath,
	const std::filesystem::path& outputDir,
	const std::vector<e2txt::Dependency>& dependencies)
{
	ExportResult result;
	std::unordered_map<std::string, std::string> localWorkspacesByResolvedPath;
	std::unordered_map<std::string, int> exportedFileNames;

	bool warnedAboutX64 = false;
	for (size_t dependencyIndex = 0; dependencyIndex < dependencies.size(); ++dependencyIndex) {
		const auto& dependency = dependencies[dependencyIndex];
		if (dependency.kind != e2txt::DependencyKind::ELib) {
			continue;
		}

		const std::string libraryName = ResolveDependencyLibraryName(dependency);
		if (libraryName.empty()) {
			e2txt::AddRuntimeWarning(Utf8Literal(u8"支持库依赖缺少 fileName/name，已跳过导出。"));
			continue;
		}

		std::filesystem::path resolvedPath;
		if (!ResolveSupportLibraryPath(sourcePath, libraryName, resolvedPath)) {
			e2txt::AddRuntimeWarning(
				Utf8Literal(u8"未找到支持库依赖：") + libraryName);
			continue;
		}

		std::error_code ec;
		std::filesystem::path resolvedAbsolutePath = std::filesystem::absolute(resolvedPath, ec);
		if (ec) {
			resolvedAbsolutePath = resolvedPath;
		}
		const std::string resolvedKey = PathToUtf8(resolvedAbsolutePath.lexically_normal());
		if (const auto it = localWorkspacesByResolvedPath.find(resolvedKey);
			it != localWorkspacesByResolvedPath.end()) {
			result.annotations.push_back(DependencyAnnotation {
				.dependencyIndex = dependencyIndex,
				.resolvedPath = resolvedKey,
				.localWorkspace = it->second,
			});
			continue;
		}

#if !defined(_M_IX86)
		result.annotations.push_back(DependencyAnnotation {
			.dependencyIndex = dependencyIndex,
			.resolvedPath = resolvedKey,
		});
		if (!warnedAboutX64) {
			e2txt::AddRuntimeWarning(
				Utf8Literal(u8"当前为 x64 版本，已跳过 elib 公开信息导出；如需生成 elib/*.txt，请使用 Win32 版 e-packager。"));
			warnedAboutX64 = true;
		}
#else
		LoadedSupportLibraryDump dump;
		std::string loadError;
		if (!TryLoadSupportLibraryDump(resolvedPath, dump, loadError)) {
			e2txt::AddRuntimeWarning(
				Utf8Literal(u8"支持库公开信息导出失败：") + PathToUtf8(resolvedPath) +
				" => " + loadError);
			result.annotations.push_back(DependencyAnnotation {
				.dependencyIndex = dependencyIndex,
				.resolvedPath = resolvedKey,
			});
			continue;
		}

		std::string baseFileName = dump.name.empty() ? dependency.name : dump.name;
		if (TrimAsciiCopy(baseFileName).empty()) {
			baseFileName = resolvedPath.stem().string();
		}
		baseFileName = SanitizeFileName(baseFileName);
		const std::string normalizedBaseFileName = ToLowerAsciiCopy(baseFileName);
		const int duplicateIndex = ++exportedFileNames[normalizedBaseFileName];
		const std::string actualFileName =
			duplicateIndex <= 1 ? baseFileName + ".txt" : baseFileName + "_" + std::to_string(duplicateIndex) + ".txt";

		const std::filesystem::path outputFilePath = outputDir / "elib" / std::filesystem::path(actualFileName);
		if (!WriteUtf8TextFileBom(outputFilePath, JoinLines(dump.lines))) {
			e2txt::AddRuntimeWarning(
				Utf8Literal(u8"写入支持库公开信息失败：") + PathToUtf8(outputFilePath));
			result.annotations.push_back(DependencyAnnotation {
				.dependencyIndex = dependencyIndex,
				.resolvedPath = resolvedKey,
			});
			continue;
		}

		const std::string localWorkspace = PathToGenericUtf8(outputFilePath.lexically_relative(outputDir));
		localWorkspacesByResolvedPath[resolvedKey] = localWorkspace;
		result.annotations.push_back(DependencyAnnotation {
			.dependencyIndex = dependencyIndex,
			.resolvedPath = resolvedKey,
			.localWorkspace = localWorkspace,
		});
		++result.exportedCount;
#endif
	}

	return result;
}

bool TryBuildDependencyFromInput(
	const std::filesystem::path& sourcePath,
	const std::string& inputText,
	BuildDependencyResult& outResult,
	std::string& outError)
{
	outResult = {};
	outError.clear();

#if !defined(_M_IX86)
	(void)sourcePath;
	(void)inputText;
	outError = "add_elib_requires_win32";
	return false;
#else
	const std::string trimmedInput = TrimAsciiCopy(inputText);
	if (trimmedInput.empty()) {
		outError = "empty_support_library_input";
		return false;
	}

	std::filesystem::path resolvedPath;
	const std::filesystem::path directPath(trimmedInput);
	std::error_code ec;
	if ((directPath.is_absolute() || trimmedInput.find('\\') != std::string::npos || trimmedInput.find('/') != std::string::npos) &&
		std::filesystem::exists(directPath, ec)) {
		resolvedPath = std::filesystem::absolute(directPath, ec);
		if (ec) {
			resolvedPath = directPath;
		}
	}
	else if (!ResolveSupportLibraryPath(sourcePath, trimmedInput, resolvedPath)) {
		outError = "support_library_not_found: " + trimmedInput;
		return false;
	}

	LoadedSupportLibraryDump dump;
	if (!TryLoadSupportLibraryDump(resolvedPath, dump, outError)) {
		outError = "support_library_load_failed: " + PathToUtf8(resolvedPath) + " => " + outError;
		return false;
	}

	e2txt::Dependency dependency;
	dependency.kind = e2txt::DependencyKind::ELib;
	dependency.fileName = resolvedPath.stem().string();
	dependency.guid = dump.guid;
	dependency.versionText = BuildVersionTextMajorMinor(dump);
	dependency.name = dump.name.empty() ? dependency.fileName : dump.name;

	outResult.dependency = std::move(dependency);
	outResult.resolvedPath = PathToUtf8(resolvedPath);
	return true;
#endif
}

}  // namespace support_library_public_info
