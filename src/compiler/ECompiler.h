#pragma once

#include <filesystem>
#include <string>
#include <vector>

namespace ecompiler {

// 独立编译后端类型。
enum class Backend {
	NativeCpp,
	BlackMoon,
};

// 黑月链接入口模式，产物体积通常从小到大为汇编、C/C++、MFC。
enum class BlackMoonMode {
	Assembly,
	Cpp,
	Mfc,
};

// 独立 Win32 编译选项。编译器和链接器均可显式覆盖。
struct Options {
	std::filesystem::path compilerPath;
	std::filesystem::path linkerPath;
	std::filesystem::path libraryPath;
	Backend backend = Backend::NativeCpp;
	BlackMoonMode blackMoonMode = BlackMoonMode::Assembly;
	// 黑月目录，包含 bin/ 与 lib/；留空时根据 e.exe 自动推导。
	std::filesystem::path blackMoonDirectory;
	// 黑月后端用于生成原生易代码 PE 的无头 IDE 工具。
	std::filesystem::path eIdePath;
	std::filesystem::path autoLinkerTestPath;
	unsigned int blackMoonTimeoutSeconds = 120;
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
