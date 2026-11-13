#pragma once

#include <filesystem>
#include <string>

// 工作区辅助：为解包目录生成说明文件，并解析默认无参封包目标。
namespace workspace_support {

struct WorkspaceWriteOptions {
	std::string defaultPackOutputFileName;
	// 同时生成 AGENTS.md 与 README.md；依赖模块副本可关闭说明文档。
	bool writeAgentsMarkdown = true;
	// 默认复制当前工具到 tool/e-packager.exe，可在解包时关闭。
	bool copyToolExecutable = true;
};

bool WriteWorkspaceFiles(
	const std::filesystem::path& inputFile,
	const std::filesystem::path& outputDir,
	std::string& outError,
	const WorkspaceWriteOptions& options = {});

// 定位当前工作区或工具所属工作区；只读，不创建回包输出目录。
bool ResolveProjectRoot(
	const std::filesystem::path& currentDir,
	std::filesystem::path& outProjectRoot,
	std::string& outError);

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
