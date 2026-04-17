#pragma once

#include <filesystem>
#include <string>

// 工作区辅助：为解包目录生成说明文件，并解析默认无参封包目标。
namespace workspace_support {

struct WorkspaceWriteOptions {
	std::string defaultPackOutputFileName;
};

bool WriteWorkspaceFiles(
	const std::filesystem::path& inputFile,
	const std::filesystem::path& outputDir,
	std::string& outError,
	const WorkspaceWriteOptions& options = {});

bool ResolveDefaultPackOutput(
	const std::filesystem::path& currentDir,
	std::filesystem::path& outProjectRoot,
	std::filesystem::path& outOutputFile,
	std::string& outError);

bool ResolvePackOutputPath(
	const std::filesystem::path& projectRoot,
	const std::filesystem::path& requestedOutputPath,
	std::filesystem::path& outOutputPath,
	std::string& outError);

bool ValidateInfoJsonVersion(
	const std::filesystem::path& projectRoot,
	std::string& outError);

}  // namespace workspace_support
