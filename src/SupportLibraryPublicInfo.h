#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

#include "e2txt.h"

// 支持库公开信息导出辅助。
namespace support_library_public_info {

// 支持库参数的稳定 ABI 描述，所有字符串均已从 FNE 内存复制。
struct ArgumentMetadata {
	std::string name;
	std::string englishName;
	std::uint32_t dataType = 0;
	std::uint32_t state = 0;
	std::int32_t defaultValue = 0;
};

// 支持库命令的稳定 ABI 描述。
struct CommandMetadata {
	std::size_t index = 0;
	std::string name;
	std::string englishName;
	std::string executeSymbol;
	std::int16_t category = 0;
	std::uint16_t state = 0;
	std::uint32_t returnType = 0;
	std::vector<ArgumentMetadata> arguments;
};

// 支持库复合类型成员描述。
struct DataTypeElementMetadata {
	std::string name;
	std::string englishName;
	std::uint32_t dataType = 0;
	std::uint32_t state = 0;
	std::int32_t defaultValue = 0;
	bool isArray = false;
};

// 支持库类型及其成员命令关系。
struct DataTypeMetadata {
	std::size_t index = 0;
	std::string name;
	std::string englishName;
	std::uint32_t state = 0;
	std::vector<std::size_t> commandIndexes;
	std::vector<DataTypeElementMetadata> elements;
};

// 支持库常量值描述。
struct ConstantMetadata {
	std::size_t index = 0;
	std::string name;
	std::string englishName;
	std::int16_t type = 0;
	double numberValue = 0;
	std::string textValue;
};

// 单个 FNE 的完整静态编译元数据。
struct LibraryMetadata {
	std::filesystem::path filePath;
	std::string fileName;
	std::string name;
	std::string guid;
	std::uint32_t state = 0;
	int majorVersion = 0;
	int minorVersion = 0;
	int buildNumber = 0;
	std::string notifySymbol;
	std::vector<std::string> dependentLibraries;
	std::vector<CommandMetadata> commands;
	std::vector<DataTypeMetadata> dataTypes;
	std::vector<ConstantMetadata> constants;
};

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

// 直接从 FNE 读取静态编译元数据；仅 Win32 进程可加载 x86 支持库。
bool LoadSupportLibraryMetadata(
	const std::filesystem::path& inputPath,
	LibraryMetadata& outMetadata,
	std::string& outError);

// 根据输入的名称或路径解析支持库依赖。
bool TryBuildDependencyFromInput(
	const std::filesystem::path& sourcePath,
	const std::string& inputText,
	BuildDependencyResult& outResult,
	std::string& outError);

}  // namespace support_library_public_info
