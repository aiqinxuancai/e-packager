#pragma once

#include <filesystem>
#include <string>

// 工作区辅助：为解包目录生成说明文件，并解析默认无参封包目标。
namespace workspace_support {

bool WriteWorkspaceFiles(
	const std::filesystem::path& inputFile,
	const std::filesystem::path& outputDir,
	std::string& outError);

bool ResolveDefaultPackOutput(
	const std::filesystem::path& currentDir,
	std::filesystem::path& outProjectRoot,
	std::filesystem::path& outOutputFile,
	std::string& outError);

}  // namespace workspace_support
