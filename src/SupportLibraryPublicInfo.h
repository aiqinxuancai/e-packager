#pragma once

#include <cstddef>
#include <filesystem>
#include <string>
#include <vector>

#include "e2txt.h"

// 支持库公开信息导出辅助。
namespace support_library_public_info {

// 依赖导出后写回 .module.json 的辅助定位信息。
struct DependencyAnnotation {
	size_t dependencyIndex = 0;
	std::string resolvedPath;
	std::string localWorkspace;
};

// 支持库批量导出结果。
struct ExportResult {
	size_t exportedCount = 0;
	std::vector<DependencyAnnotation> annotations;
};

// 由命令行输入构建出的支持库依赖信息。
struct BuildDependencyResult {
	e2txt::Dependency dependency;
	std::string resolvedPath;
};

// 将依赖中的支持库公开信息导出到 elib/*.txt。
ExportResult ExportDependencies(
	const std::filesystem::path& sourcePath,
	const std::filesystem::path& outputDir,
	const std::vector<e2txt::Dependency>& dependencies,
	size_t workerCount = e2txt::kDefaultDependencyExportThreadCount);

// 将单个支持库的公开接口导出为文本文件，仅 Win32 版可实际加载 x86 支持库。
bool DumpSupportLibraryPublicInfoToFile(
	const std::filesystem::path& inputPath,
	const std::filesystem::path& outputPath,
	std::string& outSummary,
	std::string& outError);

// 根据输入的名称或路径解析支持库依赖。
bool TryBuildDependencyFromInput(
	const std::filesystem::path& sourcePath,
	const std::string& inputText,
	BuildDependencyResult& outResult,
	std::string& outError);

}  // namespace support_library_public_info
