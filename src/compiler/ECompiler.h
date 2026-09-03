#pragma once

#include <filesystem>
#include <string>
#include <vector>

#include "CompilerTarget.h"

namespace ecompiler {

// 编译路线：语义模型直接编译，或保留的传统黑月易代码转换。
enum class CompileMode {
	// `.e`/目录 -> 语义模型 -> C++ -> 现代 MSVC。
	Semantic,
	// IDE -> 易代码 PE -> BlackMoon.obj -> 传统入口对象（仅 Win32）。
	LegacyBlackMoon,
	// 旧命令行 `blackmoon` 的兼容分派：Win32 走传统路线，x64 走语义路线。
	BlackMoonCompatibility,
	// 保留旧 C++ 调用方的枚举名。
	NativeCpp = Semantic,
	BlackMoon = BlackMoonCompatibility,
};

enum class ExecutableSubsystem {
	Auto,
	Console,
	WindowsGui,
};

// 兼容旧的 C++ 调用方；新代码应使用 CompileMode。
using Backend = CompileMode;

// 黑月链接入口模式，产物体积通常从小到大为汇编、C/C++、MFC。
enum class BlackMoonMode {
	Assembly,
	Cpp,
	Mfc,
};

// 独立源码编译选项。编译器和链接器均可显式覆盖；semantic 支持 x86/x64。
struct Options {
	// MSVC 版本根目录（包含 bin/include/lib）；留空时自动发现。
	std::filesystem::path vcToolsDirectory;
	// Windows Kits 根目录（包含 Include/Lib/bin）；留空时自动发现。
	std::filesystem::path windowsSdkDirectory;
	std::filesystem::path compilerPath;
	std::filesystem::path linkerPath;
	// 易语言安装目录；留空时从 IDE 环境变量和注册表自动发现。
	std::filesystem::path eDirectory;
	// 语义编译核心库根目录可按优先级叠加。目录中可包含 x86/x64 adapter 清单。
	std::vector<std::filesystem::path> blackMoonCoreDirectories;
	std::vector<std::filesystem::path> blackMoonX86Directories;
	std::vector<std::filesystem::path> blackMoonX64Directories;
	// 编译原生 .e 时用于按官方 x86 命令表解码字节码的 Win32 工具。
	std::filesystem::path x86DecoderPath;
	TargetArchitecture targetArchitecture = TargetArchitecture::Host;
	CompileMode compileMode = CompileMode::Semantic;
	BlackMoonMode blackMoonMode = BlackMoonMode::Assembly;
	// 黑月目录，包含 bin/ 与 lib/；留空时根据 e.exe 自动推导。
	std::filesystem::path blackMoonDirectory;
	// 传统黑月专用链接器；留空时使用黑月目录中的 bin/LINK.EXE。
	std::filesystem::path legacyBlackMoonLinkerPath;
	// 黑月编译方式用于生成原生易代码 PE 的易语言 IDE。
	std::filesystem::path eIdePath;
	unsigned int blackMoonTimeoutSeconds = 120;
	bool keepObject = true;
	bool buildDll = false;
	ExecutableSubsystem subsystem = ExecutableSubsystem::Auto;
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

// 将原生 .e 或拆包目录编译为目标架构的 EXE 或 DLL；semantic 不启动 IDE，
// legacy-blackmoon 仅使用 IDE 生成黑月所需的中间易代码 PE。
bool Compile(
	const std::filesystem::path& inputPath,
	const std::filesystem::path& outputPath,
	const Options& options,
	Result& result);

}  // namespace ecompiler
