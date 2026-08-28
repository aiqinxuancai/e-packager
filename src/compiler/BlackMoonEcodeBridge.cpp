// 黑月易代码转换桥接，基于 BlackMoonNG（MIT）中的 EcodeToObjFile。
#include "BlackMoonEcodeBridge.h"

#if !defined(_M_IX86)

namespace ecompiler::blackmoon {

bool ConvertEcodePeToObject(
	const std::filesystem::path&,
	const std::filesystem::path&,
	const std::filesystem::path&,
	const std::filesystem::path&,
	const std::vector<std::filesystem::path>&,
	ConversionResult& outResult,
	std::string& outError)
{
	outResult = {};
	outError = "blackmoon_ecode_conversion_requires_win32";
	return false;
}

}  // namespace ecompiler::blackmoon

#else

#include "blackmoon/bm_ecode_to_obj.h"
#include "blackmoon/bm_globals.h"

#include "../PathHelper.h"
#include "../SupportLibraryPublicInfo.h"

#include <charconv>
#include <cctype>
#include <system_error>

namespace ecompiler::blackmoon {
namespace {

std::string DirectoryText(const std::filesystem::path& path)
{
	std::error_code error;
	const std::filesystem::path absolute = std::filesystem::absolute(path, error);
	std::string result = PathToUtf8(error ? path : absolute);
	if (!result.empty() && result.back() != '\\' && result.back() != '/') {
		result.push_back('\\');
	}
	return result;
}

std::string AnsiToUtf8(const std::string& text)
{
	if (text.empty()) return {};
	const int wideLength = MultiByteToWideChar(CP_ACP, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
	if (wideLength <= 0) return text;
	std::wstring wide(static_cast<std::size_t>(wideLength), L'\0');
	if (MultiByteToWideChar(CP_ACP, 0, text.data(), static_cast<int>(text.size()), wide.data(), wideLength) <= 0) return text;
	const int utf8Length = WideCharToMultiByte(CP_UTF8, 0, wide.data(), wideLength, nullptr, 0, nullptr, nullptr);
	if (utf8Length <= 0) return text;
	std::string result(static_cast<std::size_t>(utf8Length), '\0');
	if (WideCharToMultiByte(CP_UTF8, 0, wide.data(), wideLength, result.data(), utf8Length, nullptr, nullptr) <= 0) return text;
	return result;
}

std::string DescribeCoreCommandError(const std::string& rawError, const std::filesystem::path& eideDirectory)
{
	std::string result = AnsiToUtf8(rawError);
	if (rawError.find("核心库") == std::string::npos) return result;
	std::size_t begin = 0;
	while (begin < rawError.size() && std::isdigit(static_cast<unsigned char>(rawError[begin])) == 0) ++begin;
	if (begin == rawError.size()) return result;
	std::size_t end = begin;
	while (end < rawError.size() && std::isdigit(static_cast<unsigned char>(rawError[end])) != 0) ++end;
	std::size_t commandIndex = 0;
	const auto parsed = std::from_chars(rawError.data() + begin, rawError.data() + end, commandIndex);
	if (parsed.ec != std::errc() || parsed.ptr != rawError.data() + end) return result;

	support_library_public_info::LibraryMetadata metadata;
	std::string metadataError;
	if (!support_library_public_info::LoadSupportLibraryMetadata(eideDirectory / L"lib" / L"krnln.fne", metadata, metadataError)) {
		return result;
	}
	for (const auto& command : metadata.commands) {
		if (command.index != commandIndex) continue;
		const std::string name = !command.englishName.empty() ? command.englishName : command.name;
		if (!name.empty()) result += " [core_command=" + name + "]";
		break;
	}
	return result;
}

void PopulateCoreFunctionMetadata(
	const std::filesystem::path& eideDirectory,
	bm::EcodeToObjFile& converter)
{
	support_library_public_info::LibraryMetadata metadata;
	std::string metadataError;
	if (!support_library_public_info::LoadSupportLibraryMetadata(
			eideDirectory / L"lib" / L"krnln.fne", metadata, metadataError)) {
		return;
	}
	converter.coreFunctionNames.resize(metadata.commands.size());
	for (const auto& command : metadata.commands) {
		if (command.index >= converter.coreFunctionNames.size() ||
			command.executeSymbol.empty()) {
			continue;
		}
		converter.coreFunctionNames[command.index] = command.executeSymbol;
		if (converter.coreFunctionNames[command.index].front() != '_') {
			converter.coreFunctionNames[command.index].insert(
				converter.coreFunctionNames[command.index].begin(), '_');
		}
	}

	converter.coreComFunctionFlags.assign(metadata.commands.size(), 0);
	for (const auto& type : metadata.dataTypes) {
		std::string english = type.englishName;
		std::string chinese = type.name;
		for (char& character : english) {
			if (character >= 'A' && character <= 'Z') {
				character = static_cast<char>(character - 'A' + 'a');
			}
		}
		const bool isComType = english == "comobject" || english == "variant" ||
			chinese.find("COM") != std::string::npos ||
			chinese.find("变体") != std::string::npos;
		if (!isComType) continue;
		for (const std::size_t commandIndex : type.commandIndexes) {
			if (commandIndex < converter.coreComFunctionFlags.size()) {
				converter.coreComFunctionFlags[commandIndex] = 1;
			}
		}
	}
}

}  // namespace

bool ConvertEcodePeToObject(
	const std::filesystem::path& inputPe,
	const std::filesystem::path& outputObject,
	const std::filesystem::path& eideDirectory,
	const std::filesystem::path& blackMoonLibraryDirectory,
	const std::vector<std::filesystem::path>& librarySearchDirectories,
	ConversionResult& outResult,
	std::string& outError)
{
	outResult = {};
	outError.clear();
	bm::g_path.eidePath = DirectoryText(eideDirectory);
	bm::EcodeToObjFile converter;
	converter.bmLibPath = PathToUtf8(blackMoonLibraryDirectory);
	const std::filesystem::path installedCore = blackMoonLibraryDirectory / L"krnln.lib";
	const std::filesystem::path rootCore = blackMoonLibraryDirectory.parent_path() / L"krnln.lib";
	if (std::filesystem::is_regular_file(installedCore)) {
		converter.coreSymbolFiles.push_back(PathToUtf8(installedCore));
	}
	else if (std::filesystem::is_regular_file(rootCore)) {
		converter.coreSymbolFiles.push_back(PathToUtf8(rootCore));
	}
	PopulateCoreFunctionMetadata(eideDirectory, converter);
	converter.paths.reserve(librarySearchDirectories.size());
	for (const std::filesystem::path& directory : librarySearchDirectories) {
		if (!directory.empty()) converter.paths.push_back(PathToUtf8(directory));
	}

	LPBYTE rawData = nullptr;
	DWORD rawSize = 0;
	if (!converter.loadEProgram(PathToUtf8(inputPe), rawData, rawSize)) {
		outError = DescribeCoreCommandError(converter.m_error, eideDirectory);
		return false;
	}
	const bool parsed = converter.parseECode(reinterpret_cast<bm::PAPP_HEADER_INFO>(rawData), rawSize);
	delete[] rawData;
	if (!parsed) {
		outError = DescribeCoreCommandError(converter.m_error, eideDirectory);
		return false;
	}
	if (!converter.saveObjFile(PathToUtf8(outputObject))) {
		outError = "blackmoon_object_write_failed:" + PathToUtf8(outputObject);
		return false;
	}

	outResult.isConsole = converter.bIsConsole;
	outResult.isDll = converter.bIsDLL;
	outResult.usesCom = converter.bUseCom;
	outResult.hasDllMain = converter.dwDllMainOffset != 0;
	outResult.userLibraries = converter.useLibList;
	outResult.exportNames = converter.exportFuncName;
	for (const bm::LibInfoEntry* library : converter.elibInfoList) {
		if (library == nullptr || !library->m_bIsUse) continue;
		SupportLibrary item;
		item.name = library->m_sLibName;
		item.dependentLibraries = library->m_DependentLibList;
		outResult.supportLibraries.push_back(std::move(item));
	}
	return true;
}

}  // namespace ecompiler::blackmoon

#endif  // defined(_M_IX86)
