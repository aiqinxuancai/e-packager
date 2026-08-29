#pragma once

#include <filesystem>
#include <string>

namespace autolinker_compile_check {

// AutoLinker 无头编译检查选项。
struct Options {
	std::filesystem::path eIdePath;
	std::filesystem::path launcherPath;
	std::string target = "auto";
	bool staticCompile = false;
	unsigned int timeoutSeconds = 120;
};

// 已完成路径发现和参数校验的编译检查选项。
struct PreparedOptions : Options {
};

// 无头编译检查结果。
struct Result {
	bool ok = false;
	std::string summary;
	std::string error;
};

// 解析显式参数、环境变量和注册表中的工具路径。
bool Prepare(const Options& options, PreparedOptions& outOptions, std::string& outError);

// 调用 AutoLinkerTest 对指定易语言工程执行真实编译。
Result Run(const std::filesystem::path& sourcePath, const PreparedOptions& options);

// 调用 AutoLinkerTest 并保留指定输出文件，供后续编译方式消费原生 PE。
Result CompileToOutput(
	const std::filesystem::path& sourcePath,
	const std::filesystem::path& outputPath,
	const PreparedOptions& options);

}  // namespace autolinker_compile_check
