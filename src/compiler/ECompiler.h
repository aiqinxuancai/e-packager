#pragma once

#include <filesystem>
#include <string>
#include <vector>

namespace ecompiler {

// 独立 Win32 编译选项。编译器和链接器均可显式覆盖。
struct Options {
	std::filesystem::path compilerPath;
	std::filesystem::path linkerPath;
	std::filesystem::path libraryPath;
	bool keepObject = true;
	bool buildDll = false;
	std::vector<std::string> conditionMacros;
};

// 独立编译结果，objectPath 便于调试和检查 COFF 输出。
struct Result {
	bool ok = false;
	std::filesystem::path sourcePath;
	std::filesystem::path objectPath;
	std::filesystem::path outputPath;
	std::string message;
};

// 将原生 .e 或拆包目录编译为 Win32 控制台 EXE，不启动易语言 IDE。
bool Compile(
	const std::filesystem::path& inputPath,
	const std::filesystem::path& outputPath,
	const Options& options,
	Result& result);

}  // namespace ecompiler
