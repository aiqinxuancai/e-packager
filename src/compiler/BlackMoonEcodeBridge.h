// 黑月易代码 PE 到 COFF OBJ 的独立转换桥接。
#pragma once

#include <filesystem>
#include <string>
#include <vector>

namespace ecompiler::blackmoon {

struct SupportLibrary {
	std::string name;
	std::vector<std::string> dependentLibraries;
};

struct ConversionResult {
	bool isConsole = false;
	bool isDll = false;
	bool usesCom = false;
	bool hasDllMain = false;
	std::vector<SupportLibrary> supportLibraries;
	std::vector<std::string> userLibraries;
	std::vector<std::string> exportNames;
};

// 将易语言动态编译 PE 中的原生易代码转换为黑月兼容 COFF OBJ。
bool ConvertEcodePeToObject(
	const std::filesystem::path& inputPe,
	const std::filesystem::path& outputObject,
	const std::filesystem::path& eideDirectory,
	const std::filesystem::path& blackMoonLibraryDirectory,
	const std::vector<std::filesystem::path>& librarySearchDirectories,
	ConversionResult& outResult,
	std::string& outError);

}  // namespace ecompiler::blackmoon
