#include <cstdlib>
#include <Windows.h>

#include <algorithm>
#include <cctype>
#include <charconv>
#include <chrono>
#include <cwctype>
#include <exception>
#include <filesystem>
#include <fstream>
#include <future>
#include <iostream>
#include <map>
#include <regex>
#include <sstream>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "..\thirdparty\json.hpp"
#include "AutoLinkerCompileCheck.h"
#include "DependencyDownloader.h"
#include "compiler/ECompiler.h"
#include "EFolderCodec.h"
#include "PathHelper.h"
#include "SelfUpdater.h"
#include "SourcePreflightValidator.h"
#include "SupportLibraryPublicInfo.h"
#include "UpdateCheck.h"
#include "WorkspaceProjectSupport.h"
#include "e2txt.h"
#include "version.h"

namespace {

using json = nlohmann::json;

bool IsVersionCommand(const std::string& command)
{
	return command == "version" ||
		command == "--version" ||
		command == "-v" ||
		command == "/version";
}

bool IsVersionInvocation(int argc, char* argv[])
{
	return argc >= 2 && argv[1] != nullptr && IsVersionCommand(argv[1]);
}

void PrintVersion()
{
	std::cout << "e-packager " << APP_VERSION << std::endl;
}

int PrintStringResult(const char* label, int result, const char* text)
{
	if (result >= 0) {
		std::cout << label << ": " << text << std::endl;
		return EXIT_SUCCESS;
	}

	if (text != nullptr && text[0] != '\0') {
		std::cerr << label << " failed: " << text << std::endl;
	}
	else if (result == -2) {
		std::cerr << label << " failed: buffer too small" << std::endl;
	}
	else {
		std::cerr << label << " failed: invalid argument" << std::endl;
	}
	return EXIT_FAILURE;
}

std::string PathToUtf8(const std::filesystem::path& path)
{
	return WideToUtf8Text(path.wstring());
}

std::filesystem::path ResolveAbsolutePath(const std::filesystem::path& path)
{
	std::error_code ec;
	const std::filesystem::path absolutePath = std::filesystem::absolute(path, ec);
	return ec ? path : absolutePath;
}

bool IsSupportLibraryFileExtension(const std::filesystem::path& path)
{
	std::string extension = path.extension().string();
	std::transform(extension.begin(), extension.end(), extension.begin(), [](unsigned char ch) {
		return static_cast<char>(std::tolower(ch));
	});
	return extension == ".fne" || extension == ".fnr" || extension == ".dll";
}

std::filesystem::path BuildDefaultSupportLibraryDumpOutputPath(const std::filesystem::path& inputPath)
{
	std::filesystem::path outputPath = inputPath;
	outputPath.replace_extension(L".txt");
	return outputPath;
}

std::filesystem::path ResolveSupportLibraryDumpOutputPath(
	const std::filesystem::path& inputPath,
	const std::filesystem::path& requestedOutputPath)
{
	if (requestedOutputPath.empty()) {
		return BuildDefaultSupportLibraryDumpOutputPath(inputPath);
	}

	std::error_code ec;
	if (std::filesystem::exists(requestedOutputPath, ec) && std::filesystem::is_directory(requestedOutputPath, ec)) {
		return requestedOutputPath / (inputPath.stem().wstring() + L".txt");
	}
	return requestedOutputPath;
}

void ClearNativeReuseState(e2txt::ProjectBundle& bundle)
{
	bundle.nativeBundleDigest.clear();
	bundle.nativeSourceBytes.clear();
	bundle.nativeSourceSnapshots.clear();
	bundle.nativeProgramHeader.reset();
	bundle.nativeGlobalSnapshots.clear();
	bundle.nativeStructSnapshots.clear();
	bundle.nativeDllSnapshots.clear();
	bundle.nativeConstantSnapshots.clear();
}

void ClearNativeByteReuseState(e2txt::ProjectBundle& bundle)
{
	bundle.nativeBundleDigest.clear();
	bundle.nativeSourceBytes.clear();
	bundle.nativeSourceSnapshots.clear();
}

void ConfigureConsoleForUtf8()
{
	DWORD mode = 0;
	const HANDLE stdoutHandle = GetStdHandle(STD_OUTPUT_HANDLE);
	const HANDLE stderrHandle = GetStdHandle(STD_ERROR_HANDLE);
	if ((stdoutHandle != nullptr && stdoutHandle != INVALID_HANDLE_VALUE && GetConsoleMode(stdoutHandle, &mode)) ||
		(stderrHandle != nullptr && stderrHandle != INVALID_HANDLE_VALUE && GetConsoleMode(stderrHandle, &mode))) {
		SetConsoleOutputCP(CP_UTF8);
		SetConsoleCP(CP_UTF8);
	}
}

struct UnpackOptions {
	bool writeAgentsMarkdown = true;
	bool writeDependencyArtifacts = true;
	bool unpackDependencyModules = true;
	size_t dependencyExportThreadCount = e2txt::kDefaultDependencyExportThreadCount;
	e2txt::ReadOptions readOptions;
};

struct PackCommandOptions {
	e2txt::WriteOptions writeOptions;
	bool compileCheck = false;
	autolinker_compile_check::Options compileOptions;
};

struct CompileCommandOptions {
	ecompiler::Options compilerOptions;
};

enum class DiagnosticOutputFormat {
	Text,
	Json,
};

bool ParseDiagnosticOutputFormat(const std::string& value, DiagnosticOutputFormat& outFormat)
{
	std::string normalized = value;
	std::transform(normalized.begin(), normalized.end(), normalized.begin(), [](const unsigned char character) {
		return static_cast<char>(std::tolower(character));
	});
	if (normalized == "text" || normalized == "plain") {
		outFormat = DiagnosticOutputFormat::Text;
		return true;
	}
	if (normalized == "json") {
		outFormat = DiagnosticOutputFormat::Json;
		return true;
	}
	return false;
}

struct DependencyModuleAnnotation {
	size_t dependencyIndex = 0;
	std::string resolvedPath;
	std::string localWorkspace;
};

struct DependencyModuleExportResult {
	size_t exportedCount = 0;
	std::vector<DependencyModuleAnnotation> annotations;
};

struct PendingDependencyModuleExport {
	size_t dependencyIndex = 0;
	std::filesystem::path resolvedPath;
	std::string resolvedKey;
	std::filesystem::path outputDir;
	std::string localWorkspace;
};

struct DependencyModuleTaskResult {
	bool exported = false;
	DependencyModuleAnnotation annotation;
	std::string warning;
};

struct DependencyModuleAnnotationEntry {
	size_t dependencyIndex = 0;
	std::string resolvedKey;
	size_t pendingExportIndex = static_cast<size_t>(-1);
};

std::string TrimAsciiCopy(std::string text)
{
	const auto isSpace = [](unsigned char ch) {
		return std::isspace(ch) != 0;
	};
	text.erase(
		text.begin(),
		std::find_if(text.begin(), text.end(), [&](const unsigned char ch) {
			return !isSpace(ch);
		}));
	text.erase(
		std::find_if(text.rbegin(), text.rend(), [&](const unsigned char ch) {
			return !isSpace(ch);
		}).base(),
		text.end());
	return text;
}

bool ReadFileBytes(const std::filesystem::path& path, std::vector<std::uint8_t>& outBytes, std::string& outError);

std::string ToLowerAsciiCopy(std::string text)
{
	std::transform(text.begin(), text.end(), text.begin(), [](unsigned char ch) {
		return static_cast<char>(std::tolower(ch));
	});
	return text;
}

std::vector<std::filesystem::path> BuildDependencyModuleCandidatePaths(
	const std::filesystem::path& sourcePath,
	const std::string& modulePathText)
{
	return BuildModuleFileLookupCandidates(sourcePath, modulePathText);
}

bool ResolveDependencyModulePath(
	const std::filesystem::path& sourcePath,
	const std::string& modulePathText,
	std::filesystem::path& outResolvedPath)
{
	outResolvedPath.clear();
	for (const auto& candidate : BuildDependencyModuleCandidatePaths(sourcePath, modulePathText)) {
		std::error_code ec;
		if (!std::filesystem::exists(candidate, ec)) {
			continue;
		}
		outResolvedPath = candidate;
		return true;
	}
	return false;
}

std::string SanitizeDirectoryName(std::string name)
{
	for (char& ch : name) {
		const unsigned char byte = static_cast<unsigned char>(ch);
		if (byte < 0x20 ||
			ch == '<' || ch == '>' || ch == ':' || ch == '"' ||
			ch == '/' || ch == '\\' || ch == '|' || ch == '?' || ch == '*') {
			ch = '_';
		}
	}

	while (!name.empty() && (name.back() == ' ' || name.back() == '.')) {
		name.pop_back();
	}
	size_t first = 0;
	while (first < name.size() && (name[first] == ' ' || name[first] == '.')) {
		++first;
	}
	name.erase(0, first);
	return name.empty() ? std::string("module") : name;
}

std::string BuildDependencyDirectoryName(
	const e2txt::Dependency& dependency,
	const std::filesystem::path& resolvedPath)
{
	std::string name = TrimAsciiCopy(dependency.name);
	if (name.empty() && !resolvedPath.empty()) {
		name = resolvedPath.stem().string();
	}
	if (name.empty()) {
		std::string modulePathText = TrimAsciiCopy(dependency.path);
		if (modulePathText.size() >= 2 &&
			modulePathText.front() == '"' &&
			modulePathText.back() == '"') {
			modulePathText = modulePathText.substr(1, modulePathText.size() - 2);
		}
		if (!modulePathText.empty() && modulePathText.front() == '$') {
			modulePathText.erase(modulePathText.begin());
		}
		name = std::filesystem::path(modulePathText).stem().string();
	}
	return SanitizeDirectoryName(name);
}

std::string PathToGenericUtf8(const std::filesystem::path& path)
{
	return WideToUtf8Text(path.generic_wstring());
}

std::string NormalizeCrLfForWrite(const std::string& text)
{
	std::string normalized;
	normalized.reserve(text.size() + 16);
	for (size_t index = 0; index < text.size(); ++index) {
		const char ch = text[index];
		if (ch == '\r') {
			normalized.append("\r\n");
			if (index + 1 < text.size() && text[index + 1] == '\n') {
				++index;
			}
		}
		else if (ch == '\n') {
			normalized.append("\r\n");
		}
		else {
			normalized.push_back(ch);
		}
	}
	return normalized;
}

bool ReadUtf8JsonFile(const std::filesystem::path& path, json& outJson, std::string& outError)
{
	outJson = json();
	outError.clear();

	std::ifstream in(path, std::ios::binary);
	if (!in.is_open()) {
		outError = "open_json_failed: " + PathToUtf8(path);
		return false;
	}

	in.seekg(0, std::ios::end);
	const std::streamoff size = in.tellg();
	if (size < 0) {
		outError = "tellg_json_failed: " + PathToUtf8(path);
		return false;
	}
	in.seekg(0, std::ios::beg);

	std::string bytes(static_cast<size_t>(size), '\0');
	if (size > 0) {
		in.read(bytes.data(), size);
		if (!in.good() && static_cast<size_t>(in.gcount()) != bytes.size()) {
			outError = "read_json_failed: " + PathToUtf8(path);
			return false;
		}
	}
	if (bytes.size() >= 3 &&
		static_cast<unsigned char>(bytes[0]) == 0xEF &&
		static_cast<unsigned char>(bytes[1]) == 0xBB &&
		static_cast<unsigned char>(bytes[2]) == 0xBF) {
		bytes.erase(0, 3);
	}

	try {
		outJson = json::parse(bytes);
		return true;
	}
	catch (const std::exception& ex) {
		outError = std::string("parse_json_failed: ") + ex.what();
		return false;
	}
}

bool WriteUtf8JsonFileBom(const std::filesystem::path& path, const json& value, std::string& outError)
{
	outError.clear();

	std::error_code ec;
	if (path.has_parent_path()) {
		std::filesystem::create_directories(path.parent_path(), ec);
		if (ec) {
			outError = "create_json_dir_failed: " + PathToUtf8(path.parent_path());
			return false;
		}
	}

	std::ofstream out(path, std::ios::binary | std::ios::trunc);
	if (!out.is_open()) {
		outError = "write_json_open_failed: " + PathToUtf8(path);
		return false;
	}

	static constexpr unsigned char kUtf8Bom[] = {0xEF, 0xBB, 0xBF};
	out.write(reinterpret_cast<const char*>(kUtf8Bom), sizeof(kUtf8Bom));
	const std::string text = NormalizeCrLfForWrite(value.dump(2, ' ', false, json::error_handler_t::replace));
	out.write(text.data(), static_cast<std::streamsize>(text.size()));
	if (!out.good()) {
		outError = "write_json_failed: " + PathToUtf8(path);
		return false;
	}
	return true;
}

bool WriteDependencyModuleAnnotations(
	const std::filesystem::path& outputDir,
	const std::vector<DependencyModuleAnnotation>& annotations,
	std::string& outError)
{
	outError.clear();
	if (annotations.empty()) {
		return true;
	}

	const std::filesystem::path moduleJsonPath = outputDir / L"project" / L".module.json";
	json moduleJson;
	if (!ReadUtf8JsonFile(moduleJsonPath, moduleJson, outError)) {
		return false;
	}

	auto dependenciesIt = moduleJson.find("dependencies");
	if (dependenciesIt == moduleJson.end() || !dependenciesIt->is_array()) {
		outError = "dependencies_json_not_array: " + PathToUtf8(moduleJsonPath);
		return false;
	}

	for (const auto& annotation : annotations) {
		if (annotation.dependencyIndex >= dependenciesIt->size()) {
			continue;
		}

		auto& dependencyJson = (*dependenciesIt)[annotation.dependencyIndex];
		if (!dependencyJson.is_object()) {
			continue;
		}

		if (!annotation.resolvedPath.empty()) {
			dependencyJson["resolvedPath"] = annotation.resolvedPath;
		}
		if (!annotation.localWorkspace.empty()) {
			dependencyJson["localWorkspace"] = annotation.localWorkspace;
		}
	}

	return WriteUtf8JsonFileBom(moduleJsonPath, moduleJson, outError);
}

struct DependencyRefreshResult {
	size_t exportedEComModules = 0;
	size_t exportedELibFiles = 0;
	std::vector<DependencyModuleAnnotation> annotations;
};

bool DoUnpackInternal(
	const std::filesystem::path& inputPath,
	const std::filesystem::path& outputDir,
	std::string& outSummary,
	std::string& outError,
	const UnpackOptions& options);

DependencyModuleExportResult ExportDependencyModules(
	const std::filesystem::path& sourcePath,
	const std::filesystem::path& outputDir,
	const e2txt::ProjectBundle& bundle,
	const e2txt::ReadOptions& readOptions,
	const size_t workerCount)
{
	DependencyModuleExportResult result;
	std::filesystem::path ecomRoot = outputDir / "ecom";
	std::unordered_set<std::string> exportedPaths;
	std::unordered_map<std::string, size_t> pendingExportIndexByResolvedPath;
	std::unordered_map<std::string, int> exportedDirNames;
	std::vector<PendingDependencyModuleExport> pendingExports;
	std::vector<DependencyModuleAnnotationEntry> annotationEntries;

	for (size_t dependencyIndex = 0; dependencyIndex < bundle.dependencies.size(); ++dependencyIndex) {
		const auto& dependency = bundle.dependencies[dependencyIndex];
		if (dependency.kind != e2txt::DependencyKind::ECom) {
			continue;
		}

		std::filesystem::path resolvedPath;
		if (!ResolveDependencyModulePath(sourcePath, dependency.path, resolvedPath)) {
			e2txt::AddRuntimeWarning(
				Utf8Literal(u8"未找到易模块依赖：") + dependency.name +
				" path=" + dependency.path);
			continue;
		}

		std::error_code ec;
		std::filesystem::path resolvedAbsolutePath = std::filesystem::absolute(resolvedPath, ec);
		if (ec) {
			resolvedAbsolutePath = resolvedPath;
		}
		resolvedAbsolutePath = resolvedAbsolutePath.lexically_normal();
		const std::string resolvedKey = PathToUtf8(resolvedAbsolutePath);
		if (const auto pendingExportIt = pendingExportIndexByResolvedPath.find(resolvedKey);
			pendingExportIt != pendingExportIndexByResolvedPath.end()) {
			annotationEntries.push_back(DependencyModuleAnnotationEntry {
				.dependencyIndex = dependencyIndex,
				.resolvedKey = resolvedKey,
				.pendingExportIndex = pendingExportIt->second,
			});
			continue;
		}

		if (!exportedPaths.insert(resolvedKey).second) {
			result.annotations.push_back(DependencyModuleAnnotation {
				.dependencyIndex = dependencyIndex,
				.resolvedPath = resolvedKey,
			});
			continue;
		}

		const std::string baseDirName = BuildDependencyDirectoryName(dependency, resolvedPath);
		const std::string normalizedBaseDirName = ToLowerAsciiCopy(baseDirName);
		const int duplicateIndex = ++exportedDirNames[normalizedBaseDirName];
		const std::string actualDirName =
			duplicateIndex <= 1 ? baseDirName : (baseDirName + "_" + std::to_string(duplicateIndex));
		const std::filesystem::path moduleOutputDir = ecomRoot / std::filesystem::path(actualDirName);
		const std::string localWorkspace = PathToGenericUtf8(moduleOutputDir.lexically_relative(outputDir));

		const size_t pendingExportIndex = pendingExports.size();
		pendingExportIndexByResolvedPath[resolvedKey] = pendingExportIndex;
		annotationEntries.push_back(DependencyModuleAnnotationEntry {
			.dependencyIndex = dependencyIndex,
			.resolvedKey = resolvedKey,
			.pendingExportIndex = pendingExportIndex,
		});
		pendingExports.push_back(PendingDependencyModuleExport {
			.dependencyIndex = dependencyIndex,
			.resolvedPath = resolvedPath,
			.resolvedKey = resolvedKey,
			.outputDir = moduleOutputDir,
			.localWorkspace = localWorkspace,
		});
	}

	std::vector<DependencyModuleTaskResult> taskResults(pendingExports.size());
	e2txt::RunFixedThreadTasks(
		pendingExports.size(),
		workerCount,
		[&](const size_t taskIndex) {
		const auto& task = pendingExports[taskIndex];
		std::string childSummary;
		std::string childError;
		const UnpackOptions childOptions {
			.writeAgentsMarkdown = false,
			.writeDependencyArtifacts = true,
			.unpackDependencyModules = false,
			.dependencyExportThreadCount = 1,
			.readOptions = readOptions,
		};
		DependencyModuleTaskResult taskResult;
		taskResult.annotation = DependencyModuleAnnotation {
			.dependencyIndex = task.dependencyIndex,
			.resolvedPath = task.resolvedKey,
		};
		if (!DoUnpackInternal(task.resolvedPath, task.outputDir, childSummary, childError, childOptions)) {
			taskResult.warning =
				Utf8Literal(u8"易模块依赖解包失败：") + PathToUtf8(task.resolvedPath) +
				" => " + childError;
			taskResults[taskIndex] = std::move(taskResult);
			return;
		}

		taskResult.exported = true;
		taskResult.annotation.localWorkspace = task.localWorkspace;
		taskResults[taskIndex] = std::move(taskResult);
	});

	for (const auto& taskResult : taskResults) {
		if (!taskResult.warning.empty()) {
			e2txt::AddRuntimeWarning(taskResult.warning);
		}
		if (taskResult.exported) {
			++result.exportedCount;
		}
	}
	for (const auto& entry : annotationEntries) {
		DependencyModuleAnnotation annotation {
			.dependencyIndex = entry.dependencyIndex,
			.resolvedPath = entry.resolvedKey,
		};
		if (entry.pendingExportIndex < taskResults.size() && taskResults[entry.pendingExportIndex].exported) {
			annotation.localWorkspace = pendingExports[entry.pendingExportIndex].localWorkspace;
		}
		result.annotations.push_back(std::move(annotation));
	}

	return result;
}

void AppendSupportLibraryAnnotations(
	const std::vector<support_library_public_info::DependencyAnnotation>& inputAnnotations,
	std::vector<DependencyModuleAnnotation>& outAnnotations)
{
	for (const auto& item : inputAnnotations) {
		outAnnotations.push_back(DependencyModuleAnnotation {
			.dependencyIndex = item.dependencyIndex,
			.resolvedPath = item.resolvedPath,
			.localWorkspace = item.localWorkspace,
		});
	}
}

bool RemoveGeneratedDependencyArtifacts(const std::filesystem::path& outputDir, std::string& outError)
{
	outError.clear();

	for (const auto& childDirName : { L"ecom", L"elib" }) {
		std::error_code ec;
		std::filesystem::remove_all(outputDir / childDirName, ec);
		if (ec) {
			outError = "remove_generated_dependency_artifacts_failed: " + PathToUtf8(outputDir / childDirName);
			return false;
		}
	}

	return true;
}

bool RefreshDependencyArtifacts(
	const std::filesystem::path& sourcePath,
	const std::filesystem::path& outputDir,
	const e2txt::ProjectBundle& bundle,
	const bool exportEComModules,
	const e2txt::ReadOptions& readOptions,
	const size_t workerCount,
	DependencyRefreshResult& outResult,
	std::string& outError)
{
	outResult = {};
	outError.clear();

	if (!RemoveGeneratedDependencyArtifacts(outputDir, outError)) {
		return false;
	}

	if (exportEComModules) {
		const DependencyModuleExportResult ecomResult = ExportDependencyModules(
			sourcePath,
			outputDir,
			bundle,
			readOptions,
			workerCount);
		outResult.exportedEComModules = ecomResult.exportedCount;
		outResult.annotations.insert(
			outResult.annotations.end(),
			ecomResult.annotations.begin(),
			ecomResult.annotations.end());
	}

	const auto elibResult = support_library_public_info::ExportDependencies(
		sourcePath,
		outputDir,
		bundle.dependencies,
		workerCount);
	outResult.exportedELibFiles = elibResult.exportedCount;
	AppendSupportLibraryAnnotations(elibResult.annotations, outResult.annotations);

	if (!WriteDependencyModuleAnnotations(outputDir, outResult.annotations, outError)) {
		return false;
	}

	return true;
}

bool DoUnpackInternal(
	const std::filesystem::path& inputPath,
	const std::filesystem::path& outputDir,
	std::string& outSummary,
	std::string& outError,
	const UnpackOptions& options)
{
	const std::filesystem::path effectiveInputPath = ResolveAbsolutePath(inputPath);
	const std::filesystem::path effectiveOutputDir = ResolveAbsolutePath(outputDir);

	e2txt::Generator generator;
	e2txt::ProjectBundle bundle;
	workspace_support::WorkspaceWriteOptions workspaceOptions;
	std::string extension = effectiveInputPath.extension().string();
	std::transform(extension.begin(), extension.end(), extension.begin(), [](unsigned char ch) {
		return static_cast<char>(std::tolower(ch));
	});
	if (extension == ".ec") {
		e2txt::ProjectBundle ecBundle;
		if (!generator.GenerateBundle(PathToUtf8(effectiveInputPath), ecBundle, &outError, options.readOptions)) {
			return false;
		}

		e2txt::ProjectBundle bridgeSourceBundle = ecBundle;
		bridgeSourceBundle.nativeSourceBytes.clear();
		bridgeSourceBundle.nativeBundleDigest.clear();

		e2txt::Restorer restorer;
		std::vector<std::uint8_t> eBytes;
		if (!restorer.RestoreBundleToBytesForEcUnpackBridge(bridgeSourceBundle, eBytes, &outError)) {
			return false;
		}

		std::filesystem::path bridgeSourcePath = effectiveInputPath;
		bridgeSourcePath += L".e";
		if (!generator.GenerateBundleFromBytes(eBytes, PathToUtf8(bridgeSourcePath), bundle, &outError)) {
			return false;
		}
		bundle.sourcePath = PathToUtf8(effectiveInputPath);
		bundle.sourceFileKind = e2txt::SourceFileKind::EC;
		bundle.publicHeaderText = ecBundle.publicHeaderText;
		workspaceOptions.defaultPackOutputFileName = PathToUtf8(effectiveInputPath.filename()) + ".e";
	}
	else {
		if (!generator.GenerateBundle(PathToUtf8(effectiveInputPath), bundle, &outError, options.readOptions)) {
			return false;
		}
	}

	e2txt::BundleDirectoryCodec codec;
	if (!codec.WriteBundle(bundle, PathToUtf8(effectiveOutputDir), &outError)) {
		return false;
	}
	workspaceOptions.writeAgentsMarkdown = options.writeAgentsMarkdown;
	if (!workspace_support::WriteWorkspaceFiles(effectiveInputPath, effectiveOutputDir, outError, workspaceOptions)) {
		return false;
	}

	DependencyRefreshResult dependencyRefreshResult;
	if (options.writeDependencyArtifacts) {
		if (!RefreshDependencyArtifacts(
				effectiveInputPath,
				effectiveOutputDir,
				bundle,
				options.unpackDependencyModules && extension != ".ec",
				options.readOptions,
				options.dependencyExportThreadCount,
				dependencyRefreshResult,
				outError)) {
			return false;
		}
	}

	outSummary =
		"source_files=" + std::to_string(bundle.sourceFiles.size()) +
		", form_files=" + std::to_string(bundle.formFiles.size()) +
		", resources=" + std::to_string(bundle.resources.size()) +
		", ecom_modules=" + std::to_string(dependencyRefreshResult.exportedEComModules) +
		", elib_files=" + std::to_string(dependencyRefreshResult.exportedELibFiles) +
		", output=" + PathToUtf8(effectiveOutputDir);
	return true;
}

bool DoUnpack(
	const std::filesystem::path& inputPath,
	const std::filesystem::path& outputDir,
	std::string& outSummary,
	std::string& outError,
	const e2txt::ReadOptions& readOptions = {})
{
	UnpackOptions options;
	options.readOptions = readOptions;
	return DoUnpackInternal(inputPath, outputDir, outSummary, outError, options);
}

bool DoPack(
	const std::filesystem::path& inputDir,
	const std::filesystem::path& outputPath,
	std::string& outSummary,
	std::string& outError,
	std::filesystem::path* outWrittenOutputPath = nullptr,
	const e2txt::WriteOptions& writeOptions = {})
{
	const std::filesystem::path effectiveInputDir = ResolveAbsolutePath(inputDir);
	const std::filesystem::path requestedOutputPath = ResolveAbsolutePath(outputPath);

	if (!workspace_support::ValidateInfoJsonVersion(effectiveInputDir, outError)) {
		return false;
	}

	std::filesystem::path effectiveOutputPath;
	if (!workspace_support::ResolvePackOutputPath(
			effectiveInputDir,
			requestedOutputPath,
			effectiveOutputPath,
			outError)) {
		return false;
	}

	e2txt::BundleDirectoryCodec codec;
	e2txt::ProjectBundle bundle;
	if (!codec.ReadBundle(PathToUtf8(effectiveInputDir), bundle, &outError)) {
		return false;
	}

	e2txt::Restorer restorer;
	std::vector<std::uint8_t> plainBytes;
	if (bundle.sourceFileKind == e2txt::SourceFileKind::EC) {
		if (!restorer.RestoreBundleToBytesForEcBridge(bundle, plainBytes, &outError)) {
			return false;
		}
	}
	else {
		if (!restorer.RestoreBundleToBytes(bundle, plainBytes, &outError)) {
			return false;
		}
	}

	std::vector<std::uint8_t> outputBytes;
	if (!e2txt::EncodeSourceBytesForWrite(plainBytes, writeOptions, outputBytes, &outError)) {
		return false;
	}

	std::ofstream out(effectiveOutputPath, std::ios::binary);
	if (!out.is_open()) {
		outError = "open_output_failed";
		return false;
	}
	out.write(reinterpret_cast<const char*>(outputBytes.data()), static_cast<std::streamsize>(outputBytes.size()));
	if (!out.good()) {
		outError = "write_output_failed";
		return false;
	}

	outSummary = "bytes=" + std::to_string(outputBytes.size());
	if (!writeOptions.password.empty()) {
		outSummary += ", encrypted=true";
	}
	outSummary += ", output=" + PathToUtf8(effectiveOutputPath);
	if (outWrittenOutputPath != nullptr) {
		*outWrittenOutputPath = effectiveOutputPath;
	}
	return true;
}

bool IsEquivalentDependency(const e2txt::Dependency& left, const e2txt::Dependency& right)
{
	if (left.kind != right.kind) {
		return false;
	}

	if (left.kind == e2txt::DependencyKind::ECom) {
		return ToLowerAsciiCopy(TrimAsciiCopy(left.path)) == ToLowerAsciiCopy(TrimAsciiCopy(right.path));
	}

	const std::string leftFile = ToLowerAsciiCopy(TrimAsciiCopy(left.fileName));
	const std::string rightFile = ToLowerAsciiCopy(TrimAsciiCopy(right.fileName));
	const std::string leftGuid = ToLowerAsciiCopy(TrimAsciiCopy(left.guid));
	const std::string rightGuid = ToLowerAsciiCopy(TrimAsciiCopy(right.guid));
	if (!leftGuid.empty() || !rightGuid.empty()) {
		return leftFile == rightFile && leftGuid == rightGuid;
	}
	return leftFile == rightFile;
}

bool HasEquivalentDependency(
	const std::vector<e2txt::Dependency>& dependencies,
	const e2txt::Dependency& candidate)
{
	return std::any_of(dependencies.begin(), dependencies.end(), [&](const e2txt::Dependency& item) {
		return IsEquivalentDependency(item, candidate);
	});
}

std::filesystem::path ResolveWorkspaceSourcePath(
	const e2txt::ProjectBundle& bundle,
	const std::filesystem::path& projectRoot)
{
	if (!bundle.sourcePath.empty()) {
		return ResolveAbsolutePath(Utf8PathToPath(bundle.sourcePath));
	}
	return projectRoot;
}

bool BuildEComDependencyFromInput(
	const std::filesystem::path& sourcePath,
	const std::string& inputText,
	e2txt::Dependency& outDependency,
	std::string& outResolvedPath,
	std::string& outError)
{
	outDependency = {};
	outResolvedPath.clear();
	outError.clear();

	const std::string trimmedInput = TrimAsciiCopy(inputText);
	if (trimmedInput.empty()) {
		outError = "empty_ecom_input";
		return false;
	}

	std::filesystem::path resolvedPath;
	const std::filesystem::path directPath(trimmedInput);
	std::error_code ec;
	if ((directPath.is_absolute() || trimmedInput.find('\\') != std::string::npos || trimmedInput.find('/') != std::string::npos) &&
		std::filesystem::exists(directPath, ec)) {
		resolvedPath = std::filesystem::absolute(directPath, ec);
		if (ec) {
			resolvedPath = directPath;
		}
	}
	else if (!ResolveDependencyModulePath(sourcePath, trimmedInput, resolvedPath)) {
		outError = "ecom_not_found: " + trimmedInput;
		return false;
	}

	e2txt::Generator generator;
	e2txt::ProjectBundle moduleBundle;
	if (!generator.GenerateBundle(PathToUtf8(resolvedPath), moduleBundle, &outError)) {
		outError = "ecom_parse_failed: " + PathToUtf8(resolvedPath) + " => " + outError;
		return false;
	}

	outDependency.kind = e2txt::DependencyKind::ECom;
	outDependency.name = TrimAsciiCopy(moduleBundle.projectName);
	if (outDependency.name.empty()) {
		outDependency.name = resolvedPath.stem().string();
	}
	outDependency.path = "$" + resolvedPath.filename().string();
	outDependency.reExport = false;
	outResolvedPath = PathToUtf8(resolvedPath);
	return true;
}

std::string StripWrappingQuotes(std::string text)
{
	text = TrimAsciiCopy(std::move(text));
	if (text.size() >= 2 &&
		((text.front() == '"' && text.back() == '"') ||
			(text.front() == '\'' && text.back() == '\''))) {
		return text.substr(1, text.size() - 2);
	}
	return text;
}

std::string NormalizeResourceLogicalName(std::string name)
{
	name = StripWrappingQuotes(std::move(name));
	if (!name.empty() && name.front() == '#') {
		name.erase(name.begin());
	}
	return TrimAsciiCopy(std::move(name));
}

bool IsValidResourceLogicalName(const std::string& name)
{
	if (name.empty()) {
		return false;
	}
	return name.find_first_of("\\/:*?\"<>|") == std::string::npos;
}

std::string ResourceKindKeyPrefix(const e2txt::BundleResourceKind kind)
{
	return kind == e2txt::BundleResourceKind::Image ? "image" : "sound";
}

std::string ResourceKindDirectoryName(const e2txt::BundleResourceKind kind)
{
	return kind == e2txt::BundleResourceKind::Image ? "image" : "audio";
}

bool HasResourceLogicalNameConflict(
	const std::vector<e2txt::BundleBinaryResource>& resources,
	const e2txt::BundleResourceKind kind,
	const std::string& logicalName,
	bool& outSameKindExists)
{
	outSameKindExists = false;
	const std::string normalizedName = ToLowerAsciiCopy(TrimAsciiCopy(logicalName));
	for (const auto& resource : resources) {
		if (ToLowerAsciiCopy(TrimAsciiCopy(resource.logicalName)) != normalizedName) {
			continue;
		}
		if (resource.kind == kind) {
			outSameKindExists = true;
		}
		return true;
	}
	return false;
}

std::string MakeUniqueResourceKey(
	const std::vector<e2txt::BundleBinaryResource>& resources,
	const e2txt::BundleResourceKind kind,
	const std::string& logicalName)
{
	const std::string baseKey = ResourceKindKeyPrefix(kind) + ":" + logicalName;
	std::string candidate = baseKey;
	for (int counter = 2;; ++counter) {
		const bool exists = std::any_of(resources.begin(), resources.end(), [&](const e2txt::BundleBinaryResource& resource) {
			return resource.key == candidate;
		});
		if (!exists) {
			return candidate;
		}
		candidate = baseKey + "#" + std::to_string(counter);
	}
}

bool AppendUniqueRootChildKey(std::vector<std::string>& rootChildKeys, const std::string& key)
{
	if (key.empty() ||
		std::find(rootChildKeys.begin(), rootChildKeys.end(), key) != rootChildKeys.end()) {
		return false;
	}
	rootChildKeys.push_back(key);
	return true;
}

bool BuildBinaryResourceFromInput(
	const std::vector<e2txt::BundleBinaryResource>& existingResources,
	const e2txt::BundleResourceKind kind,
	const std::string& inputText,
	e2txt::BundleBinaryResource& outResource,
	bool& outAlreadyExists,
	std::string& outError)
{
	outResource = {};
	outAlreadyExists = false;
	outError.clear();

	std::string input = StripWrappingQuotes(inputText);
	if (input.empty()) {
		outError = kind == e2txt::BundleResourceKind::Image ? "empty_image_input" : "empty_audio_input";
		return false;
	}

	std::string logicalName;
	std::string fileNameText = input;
	const size_t assignPos = input.find('=');
	if (assignPos != std::string::npos) {
		const std::string namePart = TrimAsciiCopy(input.substr(0, assignPos));
		const std::string pathPart = TrimAsciiCopy(input.substr(assignPos + 1));
		if (!namePart.empty() &&
			!pathPart.empty() &&
			namePart.find_first_of("\\/:") == std::string::npos) {
			logicalName = NormalizeResourceLogicalName(namePart);
			fileNameText = StripWrappingQuotes(pathPart);
		}
	}

	std::filesystem::path sourcePath(fileNameText);
	if (logicalName.empty()) {
		logicalName = NormalizeResourceLogicalName(sourcePath.stem().string());
	}
	if (!IsValidResourceLogicalName(logicalName)) {
		outError = "invalid_resource_name: " + logicalName;
		return false;
	}

	std::error_code ec;
	if (!std::filesystem::exists(sourcePath, ec) || !std::filesystem::is_regular_file(sourcePath, ec)) {
		outError = "resource_file_not_found: " + fileNameText;
		return false;
	}
	sourcePath = std::filesystem::absolute(sourcePath, ec);
	if (ec) {
		sourcePath = std::filesystem::path(fileNameText);
	}

	bool sameKindExists = false;
	if (HasResourceLogicalNameConflict(existingResources, kind, logicalName, sameKindExists)) {
		if (sameKindExists) {
			outAlreadyExists = true;
			return true;
		}
		outError = "resource_name_conflict: #" + logicalName;
		return false;
	}

	std::vector<std::uint8_t> data;
	if (!ReadFileBytes(sourcePath, data, outError)) {
		return false;
	}

	outResource.kind = kind;
	outResource.key = MakeUniqueResourceKey(existingResources, kind, logicalName);
	outResource.logicalName = logicalName;
	outResource.relativePath = ResourceKindDirectoryName(kind) + "/" + logicalName + ".bin";
	outResource.comment.clear();
	outResource.isPublic = false;
	outResource.data = std::move(data);
	return true;
}

int RunUpdate(
	const std::filesystem::path& inputDir,
	const std::vector<std::string>& addEcomInputs,
	const std::vector<std::string>& addElibInputs,
	const std::vector<std::string>& addImageInputs,
	const std::vector<std::string>& addAudioInputs)
{
	const std::filesystem::path effectiveInputDir = ResolveAbsolutePath(inputDir);

	std::string error;
	if (!workspace_support::ValidateInfoJsonVersion(effectiveInputDir, error)) {
		return PrintStringResult("update", -1, error.c_str());
	}

	e2txt::BundleDirectoryCodec codec;
	e2txt::ProjectBundle bundle;
	if (!codec.ReadBundle(PathToUtf8(effectiveInputDir), bundle, &error)) {
		return PrintStringResult("update", -1, error.c_str());
	}

	const std::filesystem::path workspaceSourcePath = ResolveWorkspaceSourcePath(bundle, effectiveInputDir);
	size_t addedEcomCount = 0;
	size_t addedElibCount = 0;
	size_t addedImageCount = 0;
	size_t addedAudioCount = 0;

	for (const auto& input : addEcomInputs) {
		e2txt::Dependency dependency;
		std::string resolvedPath;
		if (!BuildEComDependencyFromInput(workspaceSourcePath, input, dependency, resolvedPath, error)) {
			return PrintStringResult("update", -1, error.c_str());
		}
		if (!HasEquivalentDependency(bundle.dependencies, dependency)) {
			bundle.dependencies.push_back(std::move(dependency));
			++addedEcomCount;
		}
	}

	for (const auto& input : addElibInputs) {
		support_library_public_info::BuildDependencyResult buildResult;
		if (!support_library_public_info::TryBuildDependencyFromInput(workspaceSourcePath, input, buildResult, error)) {
			return PrintStringResult("update", -1, error.c_str());
		}
		if (!HasEquivalentDependency(bundle.dependencies, buildResult.dependency)) {
			bundle.dependencies.push_back(std::move(buildResult.dependency));
			++addedElibCount;
		}
	}

	for (const auto& input : addImageInputs) {
		e2txt::BundleBinaryResource resource;
		bool alreadyExists = false;
		if (!BuildBinaryResourceFromInput(
				bundle.resources,
				e2txt::BundleResourceKind::Image,
				input,
				resource,
				alreadyExists,
				error)) {
			return PrintStringResult("update", -1, error.c_str());
		}
		if (!alreadyExists) {
			AppendUniqueRootChildKey(bundle.rootChildKeys, resource.key);
			bundle.resources.push_back(std::move(resource));
			++addedImageCount;
		}
	}

	for (const auto& input : addAudioInputs) {
		e2txt::BundleBinaryResource resource;
		bool alreadyExists = false;
		if (!BuildBinaryResourceFromInput(
				bundle.resources,
				e2txt::BundleResourceKind::Sound,
				input,
				resource,
				alreadyExists,
				error)) {
			return PrintStringResult("update", -1, error.c_str());
		}
		if (!alreadyExists) {
			AppendUniqueRootChildKey(bundle.rootChildKeys, resource.key);
			bundle.resources.push_back(std::move(resource));
			++addedAudioCount;
		}
	}

	if (addedEcomCount + addedElibCount + addedImageCount + addedAudioCount > 0) {
		ClearNativeByteReuseState(bundle);
	}

	if (!codec.WriteBundle(bundle, PathToUtf8(effectiveInputDir), &error)) {
		return PrintStringResult("update", -1, error.c_str());
	}

	DependencyRefreshResult refreshResult;
	if (!RefreshDependencyArtifacts(
			workspaceSourcePath,
			effectiveInputDir,
			bundle,
			bundle.sourceFileKind != e2txt::SourceFileKind::EC,
			e2txt::ReadOptions {},
			e2txt::kDefaultDependencyExportThreadCount,
			refreshResult,
			error)) {
		return PrintStringResult("update", -1, error.c_str());
	}

	const std::string summary =
		"dependencies_added=" + std::to_string(addedEcomCount + addedElibCount) +
		", resources_added=" + std::to_string(addedImageCount + addedAudioCount) +
		", add_ecom=" + std::to_string(addedEcomCount) +
		", add_elib=" + std::to_string(addedElibCount) +
		", add_image=" + std::to_string(addedImageCount) +
		", add_audio=" + std::to_string(addedAudioCount) +
		", ecom_modules=" + std::to_string(refreshResult.exportedEComModules) +
		", elib_files=" + std::to_string(refreshResult.exportedELibFiles) +
		", output=" + PathToUtf8(effectiveInputDir);
	return PrintStringResult("update", 0, summary.c_str());
}

std::string ResourceDataDigest(const e2txt::BundleBinaryResource& resource)
{
	return e2txt::ComputeTextDigest(std::string(resource.data.begin(), resource.data.end()));
}

void NormalizeBundleForDigestCompare(e2txt::ProjectBundle& bundle)
{
	if (!bundle.projectNameStored) {
		bundle.projectName.clear();
	}
}

std::string BuildBundleDigestCompareText(const e2txt::ProjectBundle& fromE, const e2txt::ProjectBundle& fromDir)
{
	e2txt::ProjectBundle normalizedFromE = fromE;
	e2txt::ProjectBundle normalizedFromDir = fromDir;
	NormalizeBundleForDigestCompare(normalizedFromE);
	NormalizeBundleForDigestCompare(normalizedFromDir);

	std::ostringstream stream;
	const std::string digestFromE = e2txt::ComputeBundleDigest(normalizedFromE);
	const std::string digestFromDir = e2txt::ComputeBundleDigest(normalizedFromDir);
	stream
		<< "digest_from_e=" << digestFromE << "\n"
		<< "digest_from_dir=" << digestFromDir << "\n"
		<< "match=" << (digestFromE == digestFromDir ? "true" : "false") << "\n";
	if (digestFromE == digestFromDir) {
		return stream.str();
	}

	const auto appendValueMismatch =
		[&stream](const char* label, const std::string& left, const std::string& right) {
			stream << "mismatch=" << label << "\n"
				<< "left=" << left << "\n"
				<< "right=" << right << "\n";
		};

	if (normalizedFromE.projectName != normalizedFromDir.projectName) {
		appendValueMismatch("projectName", normalizedFromE.projectName, normalizedFromDir.projectName);
		return stream.str();
	}
	if (normalizedFromE.versionText != normalizedFromDir.versionText) {
		appendValueMismatch("versionText", normalizedFromE.versionText, normalizedFromDir.versionText);
		return stream.str();
	}
	if (normalizedFromE.dependencies.size() != normalizedFromDir.dependencies.size()) {
		stream << "mismatch=dependencies.size\nleft=" << normalizedFromE.dependencies.size()
			<< "\nright=" << normalizedFromDir.dependencies.size() << "\n";
		return stream.str();
	}
	for (size_t index = 0; index < normalizedFromE.dependencies.size(); ++index) {
		const auto& left = normalizedFromE.dependencies[index];
		const auto& right = normalizedFromDir.dependencies[index];
		if (left.kind != right.kind ||
			left.name != right.name ||
			left.fileName != right.fileName ||
			left.guid != right.guid ||
			left.versionText != right.versionText ||
			left.path != right.path ||
			left.reExport != right.reExport) {
			stream << "mismatch=dependencies[" << index << "]\n"
				<< "left_name=" << left.name << "\n"
				<< "right_name=" << right.name << "\n"
				<< "left_file=" << left.fileName << "\n"
				<< "right_file=" << right.fileName << "\n"
				<< "left_guid=" << left.guid << "\n"
				<< "right_guid=" << right.guid << "\n"
				<< "left_version=" << left.versionText << "\n"
				<< "right_version=" << right.versionText << "\n"
				<< "left_path=" << left.path << "\n"
				<< "right_path=" << right.path << "\n"
				<< "left_reExport=" << (left.reExport ? 1 : 0) << "\n"
				<< "right_reExport=" << (right.reExport ? 1 : 0) << "\n";
			return stream.str();
		}
	}
	if (normalizedFromE.sourceFiles.size() != normalizedFromDir.sourceFiles.size()) {
		stream << "mismatch=sourceFiles.size\nleft=" << normalizedFromE.sourceFiles.size()
			<< "\nright=" << normalizedFromDir.sourceFiles.size() << "\n";
		return stream.str();
	}
	for (size_t index = 0; index < normalizedFromE.sourceFiles.size(); ++index) {
		const auto& left = normalizedFromE.sourceFiles[index];
		const auto& right = normalizedFromDir.sourceFiles[index];
		if (left.key != right.key ||
			left.logicalName != right.logicalName ||
			left.relativePath != right.relativePath ||
			left.content != right.content) {
			stream << "mismatch=sourceFiles[" << index << "]\n"
				<< "left_key=" << left.key << "\n"
				<< "right_key=" << right.key << "\n"
				<< "left_name=" << left.logicalName << "\n"
				<< "right_name=" << right.logicalName << "\n"
				<< "left_relative=" << left.relativePath << "\n"
				<< "right_relative=" << right.relativePath << "\n"
				<< "left_digest=" << e2txt::ComputeTextDigest(left.content) << "\n"
				<< "right_digest=" << e2txt::ComputeTextDigest(right.content) << "\n";
			return stream.str();
		}
	}
	if (normalizedFromE.formFiles.size() != normalizedFromDir.formFiles.size()) {
		stream << "mismatch=formFiles.size\nleft=" << normalizedFromE.formFiles.size()
			<< "\nright=" << normalizedFromDir.formFiles.size() << "\n";
		return stream.str();
	}
	for (size_t index = 0; index < normalizedFromE.formFiles.size(); ++index) {
		const auto& left = normalizedFromE.formFiles[index];
		const auto& right = normalizedFromDir.formFiles[index];
		if (left.key != right.key ||
			left.logicalName != right.logicalName ||
			left.relativePath != right.relativePath ||
			left.xmlText != right.xmlText) {
			stream << "mismatch=formFiles[" << index << "]\n"
				<< "left_key=" << left.key << "\n"
				<< "right_key=" << right.key << "\n"
				<< "left_name=" << left.logicalName << "\n"
				<< "right_name=" << right.logicalName << "\n"
				<< "left_relative=" << left.relativePath << "\n"
				<< "right_relative=" << right.relativePath << "\n"
				<< "left_digest=" << e2txt::ComputeTextDigest(left.xmlText) << "\n"
				<< "right_digest=" << e2txt::ComputeTextDigest(right.xmlText) << "\n";
			return stream.str();
		}
	}
	if (normalizedFromE.dataTypeText != normalizedFromDir.dataTypeText) {
		appendValueMismatch("dataTypeText.digest", e2txt::ComputeTextDigest(normalizedFromE.dataTypeText), e2txt::ComputeTextDigest(normalizedFromDir.dataTypeText));
		return stream.str();
	}
	if (normalizedFromE.dllDeclareText != normalizedFromDir.dllDeclareText) {
		appendValueMismatch("dllDeclareText.digest", e2txt::ComputeTextDigest(normalizedFromE.dllDeclareText), e2txt::ComputeTextDigest(normalizedFromDir.dllDeclareText));
		return stream.str();
	}
	if (normalizedFromE.constantText != normalizedFromDir.constantText) {
		appendValueMismatch("constantText.digest", e2txt::ComputeTextDigest(normalizedFromE.constantText), e2txt::ComputeTextDigest(normalizedFromDir.constantText));
		return stream.str();
	}
	if (normalizedFromE.globalText != normalizedFromDir.globalText) {
		appendValueMismatch("globalText.digest", e2txt::ComputeTextDigest(normalizedFromE.globalText), e2txt::ComputeTextDigest(normalizedFromDir.globalText));
		return stream.str();
	}
	if (normalizedFromE.resources.size() != normalizedFromDir.resources.size()) {
		stream << "mismatch=resources.size\nleft=" << normalizedFromE.resources.size()
			<< "\nright=" << normalizedFromDir.resources.size() << "\n";
		return stream.str();
	}
	for (size_t index = 0; index < normalizedFromE.resources.size(); ++index) {
		const auto& left = normalizedFromE.resources[index];
		const auto& right = normalizedFromDir.resources[index];
		if (left.kind != right.kind ||
			left.key != right.key ||
			left.logicalName != right.logicalName ||
			left.relativePath != right.relativePath ||
			left.comment != right.comment ||
			left.isPublic != right.isPublic ||
			left.data != right.data) {
			stream << "mismatch=resources[" << index << "]\n"
				<< "left_kind=" << static_cast<int>(left.kind) << "\n"
				<< "right_kind=" << static_cast<int>(right.kind) << "\n"
				<< "left_key=" << left.key << "\n"
				<< "right_key=" << right.key << "\n"
				<< "left_name=" << left.logicalName << "\n"
				<< "right_name=" << right.logicalName << "\n"
				<< "left_relative=" << left.relativePath << "\n"
				<< "right_relative=" << right.relativePath << "\n"
				<< "left_size=" << left.data.size() << "\n"
				<< "right_size=" << right.data.size() << "\n"
				<< "left_digest=" << ResourceDataDigest(left) << "\n"
				<< "right_digest=" << ResourceDataDigest(right) << "\n";
			return stream.str();
		}
	}
	if (normalizedFromE.folderAllocatedKey != normalizedFromDir.folderAllocatedKey) {
		stream << "mismatch=folderAllocatedKey\nleft=" << normalizedFromE.folderAllocatedKey
			<< "\nright=" << normalizedFromDir.folderAllocatedKey << "\n";
		return stream.str();
	}
	if (normalizedFromE.rootChildKeys != normalizedFromDir.rootChildKeys) {
		stream << "mismatch=rootChildKeys\nleft_count=" << normalizedFromE.rootChildKeys.size()
			<< "\nright_count=" << normalizedFromDir.rootChildKeys.size() << "\n";
		for (size_t index = 0; index < (std::min)(normalizedFromE.rootChildKeys.size(), normalizedFromDir.rootChildKeys.size()); ++index) {
			if (normalizedFromE.rootChildKeys[index] != normalizedFromDir.rootChildKeys[index]) {
				stream << "first_diff_index=" << index << "\n"
					<< "left=" << normalizedFromE.rootChildKeys[index] << "\n"
					<< "right=" << normalizedFromDir.rootChildKeys[index] << "\n";
				return stream.str();
			}
		}
		return stream.str();
	}
	if (normalizedFromE.folders.size() != normalizedFromDir.folders.size()) {
		stream << "mismatch=folders.size\nleft=" << normalizedFromE.folders.size()
			<< "\nright=" << normalizedFromDir.folders.size() << "\n";
		return stream.str();
	}
	for (size_t index = 0; index < normalizedFromE.folders.size(); ++index) {
		const auto& left = normalizedFromE.folders[index];
		const auto& right = normalizedFromDir.folders[index];
		if (left.key != right.key ||
			left.parentKey != right.parentKey ||
			left.expand != right.expand ||
			left.name != right.name ||
			left.childKeys != right.childKeys) {
			stream << "mismatch=folders[" << index << "]\n"
				<< "left_key=" << left.key << "\n"
				<< "right_key=" << right.key << "\n"
				<< "left_parent=" << left.parentKey << "\n"
				<< "right_parent=" << right.parentKey << "\n"
				<< "left_expand=" << (left.expand ? 1 : 0) << "\n"
				<< "right_expand=" << (right.expand ? 1 : 0) << "\n"
				<< "left_name=" << left.name << "\n"
				<< "right_name=" << right.name << "\n";
			return stream.str();
		}
	}
	if (normalizedFromE.windowBindings.size() != normalizedFromDir.windowBindings.size()) {
		stream << "mismatch=windowBindings.size\nleft=" << normalizedFromE.windowBindings.size()
			<< "\nright=" << normalizedFromDir.windowBindings.size() << "\n";
		return stream.str();
	}
	for (size_t index = 0; index < normalizedFromE.windowBindings.size(); ++index) {
		const auto& left = normalizedFromE.windowBindings[index];
		const auto& right = normalizedFromDir.windowBindings[index];
		if (left.formName != right.formName || left.className != right.className) {
			stream << "mismatch=windowBindings[" << index << "]\n"
				<< "left_form=" << left.formName << "\n"
				<< "right_form=" << right.formName << "\n"
				<< "left_class=" << left.className << "\n"
				<< "right_class=" << right.className << "\n";
			return stream.str();
		}
	}

	stream << "mismatch=unknown\n";
	return stream.str();
}

bool ParseReadOptions(
	const int argc,
	char* argv[],
	const int startIndex,
	e2txt::ReadOptions& outReadOptions)
{
	outReadOptions = {};
	for (int index = startIndex; index < argc; ++index) {
		const std::string option = argv[index];
		if (option == "--password") {
			if (index + 1 >= argc) {
				return false;
			}
			outReadOptions.password = argv[++index];
			continue;
		}
		if (option.rfind("--password=", 0) == 0) {
			outReadOptions.password = option.substr(std::string("--password=").size());
			continue;
		}
		return false;
	}
	return true;
}

bool ParseUnpackOptions(
	const int argc,
	char* argv[],
	const int startIndex,
	UnpackOptions& outOptions)
{
	outOptions = {};
	for (int index = startIndex; index < argc; ++index) {
		const std::string option = argv[index];
		if (option == "--password") {
			if (index + 1 >= argc) {
				return false;
			}
			outOptions.readOptions.password = argv[++index];
			continue;
		}
		if (option.rfind("--password=", 0) == 0) {
			outOptions.readOptions.password = option.substr(std::string("--password=").size());
			continue;
		}
		if (option == "--main-only") {
			outOptions.writeDependencyArtifacts = false;
			outOptions.unpackDependencyModules = false;
			continue;
		}
		return false;
	}
	return true;
}

bool TryParseUnsignedInt(const std::string& text, unsigned int& outValue)
{
	if (text.empty()) {
		return false;
	}
	unsigned int value = 0;
	const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
	if (error != std::errc() || end != text.data() + text.size()) {
		return false;
	}
	outValue = value;
	return true;
}

std::string ToLowerAscii(std::string value)
{
	std::transform(value.begin(), value.end(), value.begin(), [](const unsigned char character) {
		return static_cast<char>(std::tolower(character));
	});
	return value;
}

bool ParseCompileMode(const std::string& text, ecompiler::CompileMode& outMode)
{
	const std::string normalized = ToLowerAscii(text);
	if (normalized == "semantic" || normalized == "direct" || normalized == "native" || normalized == "cpp") {
		outMode = ecompiler::CompileMode::Semantic;
		return true;
	}
	if (normalized == "legacy-blackmoon" || normalized == "legacy_bm" ||
		normalized == "legacy" || normalized == "ecode") {
		outMode = ecompiler::CompileMode::LegacyBlackMoon;
		return true;
	}
	// Keep existing scripts working: the historical `blackmoon` name selected
	// the E-code bridge on x86 and the direct compiler on x64.
	if (normalized == "blackmoon" || normalized == "bm") {
		outMode = ecompiler::CompileMode::BlackMoonCompatibility;
		return true;
	}
	return false;
}

bool ParseTargetArchitecture(const std::string& text, ecompiler::TargetArchitecture& outArchitecture)
{
	const std::string normalized = ToLowerAscii(text);
	if (normalized == "host" || normalized == "auto") {
		outArchitecture = ecompiler::TargetArchitecture::Host;
		return true;
	}
	if (normalized == "x86" || normalized == "win32" || normalized == "i386") {
		outArchitecture = ecompiler::TargetArchitecture::X86;
		return true;
	}
	if (normalized == "x64" || normalized == "amd64") {
		outArchitecture = ecompiler::TargetArchitecture::X64;
		return true;
	}
	return false;
}

bool ParseExecutableSubsystem(const std::string& text, ecompiler::ExecutableSubsystem& outSubsystem)
{
	const std::string normalized = ToLowerAscii(text);
	if (normalized == "auto") {
		outSubsystem = ecompiler::ExecutableSubsystem::Auto;
		return true;
	}
	if (normalized == "console" || normalized == "cui") {
		outSubsystem = ecompiler::ExecutableSubsystem::Console;
		return true;
	}
	if (normalized == "windows" || normalized == "windowsgui" || normalized == "gui") {
		outSubsystem = ecompiler::ExecutableSubsystem::WindowsGui;
		return true;
	}
	return false;
}

bool ParseBlackMoonMode(const std::string& text, ecompiler::BlackMoonMode& outMode)
{
	const std::string normalized = ToLowerAscii(text);
	if (normalized == "asm" || normalized == "assembly") {
		outMode = ecompiler::BlackMoonMode::Assembly;
		return true;
	}
	if (normalized == "cpp" || normalized == "c++" || normalized == "c") {
		outMode = ecompiler::BlackMoonMode::Cpp;
		return true;
	}
	if (normalized == "mfc" || normalized == "vc++" || normalized == "vc") {
		outMode = ecompiler::BlackMoonMode::Mfc;
		return true;
	}
	return false;
}

bool ParseCompileCheckOption(
	const int argc,
	char* argv[],
	int& index,
	autolinker_compile_check::Options& outOptions,
	bool& outRecognized)
{
	outRecognized = true;
	const std::string option = argv[index];
	const auto readValue = [&](std::string& outValue) -> bool {
		if (index + 1 >= argc) {
			return false;
		}
		outValue = argv[++index];
		return !outValue.empty();
	};

	std::string value;
	if (option == "--eide") {
		if (!readValue(value)) return false;
		outOptions.eIdePath = std::filesystem::path(value);
		return true;
	}
	if (option.rfind("--eide=", 0) == 0) {
		outOptions.eIdePath = std::filesystem::path(option.substr(std::string("--eide=").size()));
		return !outOptions.eIdePath.empty();
	}
	if (option == "--autolinker-test") {
		if (!readValue(value)) return false;
		outOptions.launcherPath = std::filesystem::path(value);
		return true;
	}
	if (option.rfind("--autolinker-test=", 0) == 0) {
		outOptions.launcherPath = std::filesystem::path(option.substr(std::string("--autolinker-test=").size()));
		return !outOptions.launcherPath.empty();
	}
	if (option == "--compile-target") {
		if (!readValue(value)) return false;
		outOptions.target = value;
		return true;
	}
	if (option.rfind("--compile-target=", 0) == 0) {
		outOptions.target = option.substr(std::string("--compile-target=").size());
		return !outOptions.target.empty();
	}
	if (option == "--compile-static") {
		outOptions.staticCompile = true;
		return true;
	}
	if (option == "--compile-timeout") {
		if (!readValue(value)) return false;
		return TryParseUnsignedInt(value, outOptions.timeoutSeconds);
	}
	if (option.rfind("--compile-timeout=", 0) == 0) {
		return TryParseUnsignedInt(
			option.substr(std::string("--compile-timeout=").size()),
			outOptions.timeoutSeconds);
	}
	outRecognized = false;
	return true;
}

bool ParsePackCommandOptions(
	const int argc,
	char* argv[],
	const int startIndex,
	PackCommandOptions& outOptions)
{
	outOptions = {};
	for (int index = startIndex; index < argc; ++index) {
		const std::string option = argv[index];
		if (option == "--password") {
			if (index + 1 >= argc) return false;
			outOptions.writeOptions.password = argv[++index];
			continue;
		}
		if (option.rfind("--password=", 0) == 0) {
			outOptions.writeOptions.password = option.substr(std::string("--password=").size());
			continue;
		}
		if (option == "--compile-check") {
			outOptions.compileCheck = true;
			continue;
		}
		bool recognized = false;
		if (!ParseCompileCheckOption(argc, argv, index, outOptions.compileOptions, recognized)) {
			return false;
		}
		if (!recognized) {
			return false;
		}
		outOptions.compileCheck = true;
	}
	return true;
}

bool ParseStandaloneCompileCheckOptions(
	const int argc,
	char* argv[],
	const int startIndex,
	autolinker_compile_check::Options& outOptions)
{
	outOptions = {};
	for (int index = startIndex; index < argc; ++index) {
		bool recognized = false;
		if (!ParseCompileCheckOption(argc, argv, index, outOptions, recognized) || !recognized) {
			return false;
		}
	}
	return true;
}

int RunUnpack(const char* inputPath, const char* outputDir, const UnpackOptions& unpackOptions = {})
{
	std::string summary;
	std::string error;
	if (!DoUnpackInternal(
			std::filesystem::path(inputPath),
			std::filesystem::path(outputDir),
			summary,
			error,
			unpackOptions)) {
		return PrintStringResult("unpack", -1, error.c_str());
	}
	return PrintStringResult("unpack", 0, summary.c_str());
}

bool CreateStagedPackOutputPath(
	const std::filesystem::path& finalOutputPath,
	std::filesystem::path& outStagedPath,
	std::string& outError)
{
	outStagedPath.clear();
	const std::filesystem::path parent = finalOutputPath.parent_path();
	const std::wstring stem = finalOutputPath.stem().wstring().empty()
		? std::wstring(L"project")
		: finalOutputPath.stem().wstring();
	const std::wstring extension = finalOutputPath.extension().wstring();
	const std::uint64_t seed = static_cast<std::uint64_t>(GetTickCount64());
	for (unsigned int attempt = 0; attempt < 100; ++attempt) {
		const std::wstring name = L"." + stem + L".e-packager-compile-check-" +
			std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(seed) + L"-" +
			std::to_wstring(attempt) + extension;
		const std::filesystem::path candidate = parent / name;
		const HANDLE file = CreateFileW(
			candidate.c_str(),
			GENERIC_WRITE,
			0,
			nullptr,
			CREATE_NEW,
			FILE_ATTRIBUTE_TEMPORARY,
			nullptr);
		if (file != INVALID_HANDLE_VALUE) {
			CloseHandle(file);
			outStagedPath = candidate;
			return true;
		}
		if (GetLastError() != ERROR_FILE_EXISTS && GetLastError() != ERROR_ALREADY_EXISTS) {
			outError = "compile_check_staging_create_failed: " + PathToUtf8(candidate) +
				", win32_error=" + std::to_string(GetLastError());
			return false;
		}
	}
	outError = "compile_check_staging_path_collision";
	return false;
}

bool RemoveStagedPackOutput(const std::filesystem::path& path, std::string* outCleanupError = nullptr)
{
	std::error_code ec;
	const bool removed = std::filesystem::remove(path, ec);
	if (!ec && (removed || !std::filesystem::exists(path, ec))) {
		return true;
	}
	if (outCleanupError != nullptr) {
		*outCleanupError = "compile_check_staging_cleanup_failed: " + PathToUtf8(path);
	}
	return false;
}

bool CommitStagedPackOutput(
	const std::filesystem::path& stagedPath,
	const std::filesystem::path& finalPath,
	std::string& outError)
{
	if (MoveFileExW(
			stagedPath.c_str(),
			finalPath.c_str(),
			MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != FALSE) {
		return true;
	}
	outError = "compile_check_output_commit_failed: " + PathToUtf8(finalPath) +
		", win32_error=" + std::to_string(GetLastError()) +
		", staged_output=" + PathToUtf8(stagedPath);
	return false;
}

void ReplacePackSummaryOutput(std::string& summary, const std::filesystem::path& outputPath)
{
	const std::string marker = ", output=";
	const std::size_t position = summary.find(marker);
	if (position != std::string::npos) {
		summary.resize(position);
	}
	if (!summary.empty()) {
		summary += ", ";
	}
	summary += "output=" + PathToUtf8(outputPath);
}

int RunPack(const char* inputDir, const char* outputPath, const PackCommandOptions& options = {})
{
	if (!options.compileCheck) {
		std::string summary;
		std::string error;
		if (!DoPack(
				std::filesystem::path(inputDir),
				std::filesystem::path(outputPath),
				summary,
				error,
				nullptr,
				options.writeOptions)) {
			return PrintStringResult("pack", -1, error.c_str());
		}
		return PrintStringResult("pack", 0, summary.c_str());
	}

	if (!options.writeOptions.password.empty()) {
		return PrintStringResult(
			"pack",
			-1,
			"compile_check_encrypted_source_unsupported: AutoLinker cannot receive the pack password");
	}

	autolinker_compile_check::PreparedOptions preparedCompileOptions;
	std::string error;
	if (!autolinker_compile_check::Prepare(
			options.compileOptions,
			preparedCompileOptions,
			error)) {
		return PrintStringResult("pack", -1, error.c_str());
	}

	const std::filesystem::path effectiveInputDir = ResolveAbsolutePath(std::filesystem::path(inputDir));
	const std::filesystem::path requestedOutputPath = ResolveAbsolutePath(std::filesystem::path(outputPath));
	std::filesystem::path finalOutputPath;
	if (!workspace_support::ResolvePackOutputPath(
			effectiveInputDir,
			requestedOutputPath,
			finalOutputPath,
			error)) {
		return PrintStringResult("pack", -1, error.c_str());
	}
	std::string extension = finalOutputPath.extension().string();
	std::transform(extension.begin(), extension.end(), extension.begin(), [](const unsigned char ch) {
		return static_cast<char>(std::tolower(ch));
	});
	if (extension != ".e") {
		return PrintStringResult(
			"pack",
			-1,
			"compile_check_requires_e_output: choose an output path ending in .e");
	}

	std::filesystem::path stagedOutputPath;
	if (!CreateStagedPackOutputPath(finalOutputPath, stagedOutputPath, error)) {
		return PrintStringResult("pack", -1, error.c_str());
	}

	std::string summary;
	std::filesystem::path writtenOutputPath;
	if (!DoPack(
			effectiveInputDir,
			stagedOutputPath,
			summary,
			error,
			&writtenOutputPath,
			options.writeOptions)) {
		RemoveStagedPackOutput(stagedOutputPath);
		return PrintStringResult("pack", -1, error.c_str());
	}

	const autolinker_compile_check::Result compileResult =
		autolinker_compile_check::Run(writtenOutputPath, preparedCompileOptions);
	if (!compileResult.ok) {
		std::string cleanupError;
		const bool removed = RemoveStagedPackOutput(writtenOutputPath, &cleanupError);
		error = compileResult.error + "\npack_output_not_committed: " + PathToUtf8(finalOutputPath);
		if (!removed) {
			error += "\n" + cleanupError;
		}
		return PrintStringResult("pack", -1, error.c_str());
	}

	if (!CommitStagedPackOutput(writtenOutputPath, finalOutputPath, error)) {
		return PrintStringResult("pack", -1, error.c_str());
	}
	ReplacePackSummaryOutput(summary, finalOutputPath);
	summary += ", " + compileResult.summary;
	return PrintStringResult("pack", 0, summary.c_str());
}

int RunCompileCheck(
	const char* sourcePath,
	const autolinker_compile_check::Options& options)
{
	autolinker_compile_check::PreparedOptions preparedOptions;
	std::string error;
	if (!autolinker_compile_check::Prepare(options, preparedOptions, error)) {
		return PrintStringResult("compile-check", -1, error.c_str());
	}
	const autolinker_compile_check::Result result =
		autolinker_compile_check::Run(std::filesystem::path(sourcePath), preparedOptions);
	return PrintStringResult(
		"compile-check",
		result.ok ? 0 : -1,
		result.ok ? result.summary.c_str() : result.error.c_str());
}

struct ParsedCompileDiagnostic {
	std::string phase = "compile";
	std::string file;
	std::size_t line = 0;
	std::size_t column = 0;
	std::string code = "compile_failed";
	std::string message;
	std::string rawOutput;
};

std::string NormalizeDiagnosticUtf8(const std::string& value)
{
	if (value.empty()) return {};
	const int utf8Length = MultiByteToWideChar(
		CP_UTF8, MB_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()), nullptr, 0);
	if (utf8Length > 0) return value;
	const int localLength = MultiByteToWideChar(
		CP_ACP, 0, value.data(), static_cast<int>(value.size()), nullptr, 0);
	if (localLength <= 0) return value;
	std::wstring wide(static_cast<std::size_t>(localLength), L'\0');
	if (MultiByteToWideChar(
		CP_ACP, 0, value.data(), static_cast<int>(value.size()), wide.data(), localLength) <= 0) {
		return value;
	}
	return WideToUtf8Text(wide);
}

bool TryParseSourceLocation(
	const std::string& value,
	std::string& outFile,
	std::size_t& outLine,
	std::string& outDetail)
{
	static const std::regex pattern(R"(^(.+):([0-9]+): (.*)$)");
	std::smatch match;
	if (!std::regex_match(value, match, pattern) || match.size() != 4) return false;
	try {
		outLine = static_cast<std::size_t>(std::stoull(match[2].str()));
	}
	catch (...) {
		return false;
	}
	outFile = match[1].str();
	outDetail = match[3].str();
	return true;
}

std::string DiagnosticCodeFromDetail(const std::string& detail)
{
	const std::size_t separator = detail.find(':');
	const std::string code = detail.substr(0, separator);
	return code.empty() ? "compile_failed" : code;
}

ParsedCompileDiagnostic ParseCompileDiagnostic(const std::string& message)
{
	ParsedCompileDiagnostic diagnostic;
	const std::string normalizedMessage = NormalizeDiagnosticUtf8(message);
	diagnostic.message = normalizedMessage;
	diagnostic.rawOutput = normalizedMessage;
	std::string detail = normalizedMessage;
	for (;;) {
		const std::size_t separator = detail.find(':');
		if (separator == std::string::npos) break;
		const std::string prefix = detail.substr(0, separator);
		if (prefix == "compiler_model_failed") {
			diagnostic.phase = "parse";
			detail = detail.substr(separator + 1);
			continue;
		}
		if (prefix == "source_generation_failed") {
			diagnostic.phase = "generate";
			detail = detail.substr(separator + 1);
			continue;
		}
		if (prefix == "semantic_core_library_not_found" || prefix == "core_static_archive_not_found" ||
			prefix == "mfc_runtime_file_not_found" || prefix == "vc6_runtime_library_not_found" ||
			prefix == "core_runtime_dependency_not_found" || prefix == "support_library_static_archive_not_found") {
			diagnostic.phase = "dependency";
			diagnostic.code = prefix;
			break;
		}
		if (prefix == "process_failed" || prefix == "start_process_failed") {
			diagnostic.phase = "cpp_compile";
			diagnostic.code = prefix;
			break;
		}
		break;
	}
	std::string sourceFile;
	std::size_t sourceLine = 0;
	std::string sourceDetail;
	if (TryParseSourceLocation(detail, sourceFile, sourceLine, sourceDetail)) {
		diagnostic.file = sourceFile;
		diagnostic.line = sourceLine;
		diagnostic.message = sourceDetail;
		diagnostic.code = DiagnosticCodeFromDetail(sourceDetail);
		if (diagnostic.phase == "parse" && diagnostic.code.rfind("unknown_", 0) == 0) {
			diagnostic.phase = "semantic";
		}
	}
	else {
		diagnostic.message = detail;
		if (diagnostic.code == "compile_failed") diagnostic.code = DiagnosticCodeFromDetail(detail);
	}
	return diagnostic;
}

std::string ReadDiagnosticSourceLine(
	const std::filesystem::path& inputPath,
	const ParsedCompileDiagnostic& diagnostic)
{
	if (diagnostic.file.empty() || diagnostic.line == 0 || !std::filesystem::is_directory(inputPath)) return {};
	std::string relative = diagnostic.file;
	std::replace(relative.begin(), relative.end(), '\\', '/');
	if (relative.rfind("src/", 0) == 0) relative.erase(0, 4);
	const std::filesystem::path sourcePath = inputPath / "src" / Utf8PathToPath(relative);
	std::ifstream input(sourcePath, std::ios::binary);
	if (!input) return {};
	std::ostringstream buffer;
	buffer << input.rdbuf();
	const std::string content = buffer.str();
	size_t currentLine = 1;
	size_t start = 0;
	for (size_t index = 0; index <= content.size(); ++index) {
		if (index != content.size() && content[index] != '\r' && content[index] != '\n') continue;
		if (currentLine == diagnostic.line) return NormalizeDiagnosticUtf8(content.substr(start, index - start));
		if (index < content.size() && content[index] == '\r' && index + 1 < content.size() && content[index + 1] == '\n') ++index;
		start = index + 1;
		++currentLine;
	}
	return {};
}

std::string FormatCompileResultJson(
	const bool ok,
	const std::string& message,
	const std::filesystem::path& inputPath)
{
	const std::string normalizedMessage = NormalizeDiagnosticUtf8(message);
	json output;
	output["ok"] = ok;
	output["command"] = "compile";
	output["diagnostics"] = json::array();
	if (!normalizedMessage.empty()) {
		if (ok) {
			output["diagnostics"].push_back({
				{ "severity", "info" }, { "phase", "compile" }, { "file", "" }, { "line", 0 },
				{ "column", 0 }, { "code", "compile_succeeded" }, { "message", normalizedMessage },
				{ "sourceLine", "" }, { "suggestion", "" }, { "rawOutput", "" },
			});
		}
		else {
			std::istringstream lines(normalizedMessage);
			std::string line;
			while (std::getline(lines, line)) {
				if (!line.empty() && line.back() == '\r') line.pop_back();
				if (line.empty()) continue;
				const ParsedCompileDiagnostic diagnostic = ParseCompileDiagnostic(line);
				const std::string sourceLine = ReadDiagnosticSourceLine(inputPath, diagnostic);
				output["diagnostics"].push_back({
					{ "severity", "error" }, { "phase", diagnostic.phase }, { "file", diagnostic.file },
					{ "line", diagnostic.line }, { "column", diagnostic.column }, { "code", diagnostic.code },
					{ "message", diagnostic.message }, { "sourceLine", sourceLine }, { "suggestion", "" },
					{ "rawOutput", diagnostic.rawOutput },
				});
			}
		}
	}
	return output.dump();
}

int PrintCompileResult(
	const bool ok,
	const std::string& message,
	const std::filesystem::path& inputPath,
	const DiagnosticOutputFormat format)
{
	if (format == DiagnosticOutputFormat::Json) {
		const std::string output = FormatCompileResultJson(ok, message, inputPath);
		std::cout << output << std::endl;
		return ok ? EXIT_SUCCESS : EXIT_FAILURE;
	}
	const std::string normalizedMessage = NormalizeDiagnosticUtf8(message);
	return PrintStringResult("compile", ok ? 0 : -1, normalizedMessage.c_str());
}

int RunValidate(const char* inputDir, const DiagnosticOutputFormat format = DiagnosticOutputFormat::Text)
{
	const std::filesystem::path effectiveInputDir = ResolveAbsolutePath(std::filesystem::path(inputDir));
	std::string error;
	if (!workspace_support::ValidateInfoJsonVersion(effectiveInputDir, error)) {
		if (format == DiagnosticOutputFormat::Json) {
			json output = {
				{ "ok", false }, { "command", "validate" }, { "diagnostics", json::array({
					{{ "severity", "error" }, { "phase", "preflight" }, { "file", "" }, { "line", 0 },
					 { "column", 0 }, { "code", "workspace_metadata_invalid" }, { "message", error },
					 { "sourceLine", "" }, { "suggestion", "" }, { "rawOutput", error }}
				}) }
			};
			std::cout << output.dump() << std::endl;
			return EXIT_FAILURE;
		}
		return PrintStringResult("validate", -1, error.c_str());
	}

	e2txt::BundleDirectoryCodec codec;
	e2txt::ProjectBundle bundle;
	if (!codec.ReadBundle(PathToUtf8(effectiveInputDir), bundle, &error)) {
		if (format == DiagnosticOutputFormat::Json) {
			json output = {
				{ "ok", false }, { "command", "validate" }, { "diagnostics", json::array({
					{{ "severity", "error" }, { "phase", "preflight" }, { "file", "" }, { "line", 0 },
					 { "column", 0 }, { "code", "read_project_failed" }, { "message", error },
					 { "sourceLine", "" }, { "suggestion", "" }, { "rawOutput", error }}
				}) }
			};
			std::cout << output.dump() << std::endl;
			return EXIT_FAILURE;
		}
		return PrintStringResult("validate", -1, error.c_str());
	}

	const e2txt::SourcePreflightReport report = e2txt::ValidateProjectBundleSource(bundle);
	if (format == DiagnosticOutputFormat::Json) {
		std::cout << e2txt::FormatSourcePreflightReportJson(report, &bundle) << std::endl;
		return report.IsValid() ? EXIT_SUCCESS : EXIT_FAILURE;
	}
	const std::string summary = e2txt::FormatSourcePreflightReport(report);
	return PrintStringResult("validate", report.IsValid() ? 0 : -1, summary.c_str());
}

bool IsDirectECompileInput(const std::filesystem::path& inputPath)
{
	std::string extension = inputPath.extension().string();
	std::transform(extension.begin(), extension.end(), extension.begin(), [](unsigned char ch) {
		return static_cast<char>(std::tolower(ch));
	});
	return extension == ".e" && std::filesystem::is_regular_file(inputPath);
}

bool ParseMissingCompileDependency(
	const std::string& message,
	std::string& outDependency)
{
	outDependency.clear();
	const auto after = [&message](const std::string& prefix) -> std::string {
		if (message.rfind(prefix, 0) != 0) return {};
		return message.substr(prefix.size());
	};
	if (message.rfind("semantic_core_library_not_found:", 0) == 0 ||
		message.rfind("core_static_archive_not_found:", 0) == 0 ||
		message.rfind("mfc_runtime_file_not_found:", 0) == 0 ||
		message.rfind("vc6_runtime_library_not_found:", 0) == 0 ||
		message.rfind("core_runtime_dependency_not_found:", 0) == 0) {
		outDependency = "krnln";
		return true;
	}
	if (message.rfind("support_library_static_archive_not_found:", 0) == 0) {
		const std::string value = after("support_library_static_archive_not_found:");
		const std::size_t separator = value.find(':');
		outDependency = separator == std::string::npos ? value : value.substr(0, separator);
		return !outDependency.empty();
	}
	if (message.rfind("support_library_target_implementation_not_available:", 0) == 0) {
		outDependency = after("support_library_target_implementation_not_available:");
		return !outDependency.empty();
	}
	return false;
}

ecompiler::TargetArchitecture HostCompileArchitecture()
{
#if defined(_M_X64)
	return ecompiler::TargetArchitecture::X64;
#else
	return ecompiler::TargetArchitecture::X86;
#endif
}

bool AskToDownloadDependency(const std::string& dependency, const ecompiler::TargetArchitecture architecture)
{
	const char* architectureName = architecture == ecompiler::TargetArchitecture::X64 ? "x64" : "x86";
	std::cerr << Utf8Literal(u8"缺少直接编译依赖：") << dependency
		<< " (" << architectureName << ")\n"
		<< Utf8Literal(u8"是否自动下载并重试？ [Y/n] ") << std::flush;
	std::string answer;
	if (!std::getline(std::cin, answer)) {
		std::cerr << Utf8Literal(u8"\n未读取到确认，已取消自动下载。") << std::endl;
		return false;
	}
	std::cerr << std::endl;
	answer = TrimAsciiCopy(std::move(answer));
	return answer.empty() || answer[0] == 'y' || answer[0] == 'Y';
}

void AppendDownloadedDependencyRoot(
	ecompiler::Options& options,
	const ecompiler::TargetArchitecture architecture,
	const std::filesystem::path& root)
{
	if (architecture == ecompiler::TargetArchitecture::X64) {
		if (options.blackMoonX64Directory.empty()) options.blackMoonX64Directory = root;
		options.blackMoonX64Directories.push_back(root);
	}
	else {
		if (options.blackMoonCoreDirectory.empty()) options.blackMoonCoreDirectory = root;
		options.blackMoonCoreDirectories.push_back(root);
	}
}

int RunCompile(
	const char* inputPath,
	const char* outputPath,
	ecompiler::Options options = {},
	const DiagnosticOutputFormat format = DiagnosticOutputFormat::Text)
{
	const std::filesystem::path effectiveInputPath = ResolveAbsolutePath(std::filesystem::path(inputPath));
	const std::filesystem::path effectiveOutputPath = ResolveAbsolutePath(std::filesystem::path(outputPath));
	ecompiler::Result result;
	if (ecompiler::Compile(effectiveInputPath, effectiveOutputPath, options, result)) {
		return PrintCompileResult(true, result.message, effectiveInputPath, format);
	}

	std::string missingDependency;
	const bool canOfferDownload = IsDirectECompileInput(effectiveInputPath) &&
		ParseMissingCompileDependency(result.message, missingDependency);
	if (!canOfferDownload) {
		return PrintCompileResult(false, result.message, effectiveInputPath, format);
	}
	const ecompiler::TargetArchitecture architecture = options.targetArchitecture == ecompiler::TargetArchitecture::Host
		? HostCompileArchitecture() : options.targetArchitecture;
	if (!AskToDownloadDependency(missingDependency, architecture)) {
		const std::string message = result.message + "\nauto_download_declined:" + missingDependency;
		return PrintCompileResult(false, message, effectiveInputPath, format);
	}
	std::filesystem::path downloadedRoot;
	std::string downloadError;
	if (!dependency_download::EnsureDependency(
			missingDependency, architecture, downloadedRoot, downloadError)) {
		const std::string message = result.message + "\nauto_download_failed:" + downloadError;
		return PrintCompileResult(false, message, effectiveInputPath, format);
	}
	AppendDownloadedDependencyRoot(options, architecture, downloadedRoot);
	result = {};
	if (!ecompiler::Compile(effectiveInputPath, effectiveOutputPath, options, result)) {
		const std::string message = result.message + "\nauto_download_retry_failed:" + PathToUtf8(downloadedRoot);
		return PrintCompileResult(false, message, effectiveInputPath, format);
	}
	return PrintCompileResult(true, result.message, effectiveInputPath, format);
}

int RunDefaultPack()
{
	std::filesystem::path projectRoot;
	std::filesystem::path outputPath;
	std::string error;
	if (!workspace_support::ResolveDefaultPackOutput(std::filesystem::current_path(), projectRoot, outputPath, error)) {
		return PrintStringResult("pack", -1, error.c_str());
	}

	std::string summary;
	if (!DoPack(projectRoot, outputPath, summary, error)) {
		return PrintStringResult("pack", -1, error.c_str());
	}
	if (summary.find("output=") == std::string::npos) {
		if (!summary.empty()) {
			summary += ", ";
		}
		summary += "output=" + PathToUtf8(outputPath);
	}
	return PrintStringResult("pack", 0, summary.c_str());
}

int RunCompareBundle(const char* inputPath, const char* inputDir, const e2txt::ReadOptions& readOptions = {})
{
	e2txt::Generator generator;
	e2txt::Restorer restorer;
	e2txt::BundleDirectoryCodec codec;
	e2txt::ProjectBundle bundleFromE;
	e2txt::ProjectBundle bundleFromDir;
	e2txt::ProjectBundle rebuiltBundleFromDir;
	std::string error;
	const std::filesystem::path effectiveInputPath = ResolveAbsolutePath(std::filesystem::path(inputPath));
	const std::filesystem::path effectiveInputDir = ResolveAbsolutePath(std::filesystem::path(inputDir));
	std::string inputExtension = effectiveInputPath.extension().string();
	std::transform(inputExtension.begin(), inputExtension.end(), inputExtension.begin(), [](unsigned char ch) {
		return static_cast<char>(std::tolower(ch));
	});
	if (inputExtension == ".ec") {
		// .ec 拆包使用内部 .e 桥接源码；比较时采用同一语义入口，
		// 避免把原生 .ec 公开接口与桥接目录误判为源码不一致。
		e2txt::ProjectBundle ecBundle;
		if (!generator.GenerateBundle(PathToUtf8(effectiveInputPath), ecBundle, &error, readOptions)) {
			return PrintStringResult("compare-bundle", -1, error.c_str());
		}
		ecBundle.nativeSourceBytes.clear();
		ecBundle.nativeBundleDigest.clear();
		std::vector<std::uint8_t> bridgeBytes;
		if (!restorer.RestoreBundleToBytesForEcUnpackBridge(ecBundle, bridgeBytes, &error) ||
			!generator.GenerateBundleFromBytes(bridgeBytes, PathToUtf8(effectiveInputPath), bundleFromE, &error)) {
			return PrintStringResult("compare-bundle", -1, error.c_str());
		}
		bundleFromE.sourcePath = PathToUtf8(effectiveInputPath);
		bundleFromE.sourceFileKind = e2txt::SourceFileKind::EC;
	}
	else if (!generator.GenerateBundle(PathToUtf8(effectiveInputPath), bundleFromE, &error, readOptions)) {
		return PrintStringResult("compare-bundle", -1, error.c_str());
	}
	if (!codec.ReadBundle(PathToUtf8(effectiveInputDir), bundleFromDir, &error)) {
		return PrintStringResult("compare-bundle", -1, error.c_str());
	}

	std::vector<std::uint8_t> rebuiltBytes;
	const bool restored = bundleFromDir.sourceFileKind == e2txt::SourceFileKind::EC
		? restorer.RestoreBundleToBytesForEcBridge(bundleFromDir, rebuiltBytes, &error)
		: restorer.RestoreBundleToBytes(bundleFromDir, rebuiltBytes, &error);
	if (!restored) {
		const std::string restoreError = "restore_directory_bundle_failed: " + error;
		return PrintStringResult("compare-bundle", -1, restoreError.c_str());
	}
	if (!generator.GenerateBundleFromBytes(
			rebuiltBytes,
			bundleFromDir.sourcePath,
			rebuiltBundleFromDir,
			&error)) {
		const std::string generateError = "parse_rebuilt_directory_bundle_failed: " + error;
		return PrintStringResult("compare-bundle", -1, generateError.c_str());
	}

	const std::string summary = BuildBundleDigestCompareText(bundleFromE, rebuiltBundleFromDir);
	return PrintStringResult("compare-bundle", 0, summary.c_str());
}

int RunRoundTrip(
	const char* inputPath,
	const char* workDir,
	const char* outputPath,
	const e2txt::ReadOptions& readOptions = {})
{
	const std::filesystem::path root = ResolveAbsolutePath(std::filesystem::path(workDir));
	const std::filesystem::path effectiveInputPath = ResolveAbsolutePath(std::filesystem::path(inputPath));
	const std::filesystem::path requestedOutputPath = ResolveAbsolutePath(std::filesystem::path(outputPath));
	const std::filesystem::path unpackDir = root / "unpacked";
	std::error_code ec;
	std::filesystem::remove_all(unpackDir, ec);
	std::filesystem::create_directories(unpackDir, ec);

	std::string summary;
	std::string error;
	if (!DoUnpack(effectiveInputPath, unpackDir, summary, error, readOptions)) {
		return PrintStringResult("roundtrip", -1, error.c_str());
	}
	if (!DoPack(unpackDir, requestedOutputPath, summary, error)) {
		return PrintStringResult("roundtrip", -1, error.c_str());
	}
	return PrintStringResult("roundtrip", 0, summary.c_str());
}

bool ReadFileBytes(const std::filesystem::path& path, std::vector<std::uint8_t>& outBytes, std::string& outError)
{
	outBytes.clear();

	std::ifstream in(path, std::ios::binary);
	if (!in.is_open()) {
		outError = "open_failed: " + PathToUtf8(path);
		return false;
	}

	in.seekg(0, std::ios::end);
	const std::streamoff size = in.tellg();
	if (size < 0) {
		outError = "tellg_failed: " + PathToUtf8(path);
		return false;
	}
	in.seekg(0, std::ios::beg);

	outBytes.resize(static_cast<size_t>(size));
	if (size > 0) {
		in.read(reinterpret_cast<char*>(outBytes.data()), size);
		if (!in.good() && static_cast<size_t>(in.gcount()) != outBytes.size()) {
			outError = "read_failed: " + PathToUtf8(path);
			return false;
		}
	}

	return true;
}

std::string StripUtf8Bom(const std::string& text)
{
	if (text.size() >= 3 &&
		static_cast<unsigned char>(text[0]) == 0xEF &&
		static_cast<unsigned char>(text[1]) == 0xBB &&
		static_cast<unsigned char>(text[2]) == 0xBF) {
		return text.substr(3);
	}
	return text;
}

std::string NormalizeTextForCompare(const std::string& text)
{
	const std::string withoutBom = StripUtf8Bom(text);
	std::string normalized;
	normalized.reserve(withoutBom.size());

	size_t lineStart = 0;
	while (lineStart <= withoutBom.size()) {
		size_t lineEnd = withoutBom.find_first_of("\r\n", lineStart);
		if (lineEnd == std::string::npos) {
			lineEnd = withoutBom.size();
		}

		size_t contentStart = lineStart;
		while (contentStart < lineEnd &&
			(withoutBom[contentStart] == ' ' || withoutBom[contentStart] == '\t')) {
			++contentStart;
		}

		size_t contentEnd = lineEnd;
		while (contentEnd > contentStart &&
			(withoutBom[contentEnd - 1] == ' ' || withoutBom[contentEnd - 1] == '\t')) {
			--contentEnd;
		}

		if (contentEnd > contentStart) {
			normalized.append(withoutBom, contentStart, contentEnd - contentStart);
			normalized.push_back('\n');
		}

		if (lineEnd == withoutBom.size()) {
			break;
		}
		lineStart = lineEnd + 1;
		if (lineStart < withoutBom.size() &&
			withoutBom[lineEnd] == '\r' &&
			withoutBom[lineStart] == '\n') {
			++lineStart;
		}
	}

	while (!normalized.empty() && normalized.back() == '\n') {
		normalized.pop_back();
	}
	return normalized;
}

bool CompareNormalizedTextFile(
	const std::filesystem::path& leftPath,
	const std::filesystem::path& rightPath,
	std::string& outSummary)
{
	std::vector<std::uint8_t> leftBytes;
	std::vector<std::uint8_t> rightBytes;
	std::string error;
	if (!ReadFileBytes(leftPath, leftBytes, error)) {
		outSummary = error;
		return false;
	}
	if (!ReadFileBytes(rightPath, rightBytes, error)) {
		outSummary = error;
		return false;
	}

	const std::string leftText = NormalizeTextForCompare(std::string(leftBytes.begin(), leftBytes.end()));
	const std::string rightText = NormalizeTextForCompare(std::string(rightBytes.begin(), rightBytes.end()));
	if (leftText == rightText) {
		return true;
	}

	outSummary = "text_mismatch: " + PathToUtf8(leftPath);
	return false;
}

void NormalizeJsonForCompare(json& value)
{
	if (!value.is_object()) {
		return;
	}

	value.erase("sourcePath");
	value.erase("sourceFileName");
	value.erase("sourceModifiedTimeUtc");
	value.erase("nativeBundleDigest");
	value.erase("projectName");
	value.erase("projectNameStored");

	auto it = value.find("rootChildKeys");
	if (it != value.end() && it->is_array()) {
		std::vector<std::string> keys;
		for (const auto& item : *it) {
			if (item.is_string()) {
				keys.push_back(item.get<std::string>());
			}
		}
		std::sort(keys.begin(), keys.end());
		*it = json::array();
		for (const auto& key : keys) {
			it->push_back(key);
		}
	}
}

bool ShouldIgnorePathForRoundTripCompare(const std::string& relativePath)
{
	return relativePath == "AGENTS.md" ||
		relativePath.starts_with("src/.native_");
}

bool CompareJsonFile(
	const std::filesystem::path& leftPath,
	const std::filesystem::path& rightPath,
	std::string& outSummary)
{
	std::vector<std::uint8_t> leftBytes;
	std::vector<std::uint8_t> rightBytes;
	std::string error;
	if (!ReadFileBytes(leftPath, leftBytes, error)) {
		outSummary = error;
		return false;
	}
	if (!ReadFileBytes(rightPath, rightBytes, error)) {
		outSummary = error;
		return false;
	}

	try {
		auto leftJson = json::parse(StripUtf8Bom(std::string(leftBytes.begin(), leftBytes.end())));
		auto rightJson = json::parse(StripUtf8Bom(std::string(rightBytes.begin(), rightBytes.end())));
		NormalizeJsonForCompare(leftJson);
		NormalizeJsonForCompare(rightJson);
		if (leftJson == rightJson) {
			return true;
		}

		outSummary = "json_mismatch: " + PathToUtf8(leftPath);
		return false;
	}
	catch (const std::exception& ex) {
		outSummary = std::string("json_parse_failed: ") + ex.what();
		return false;
	}
}

bool BuildFileMap(
	const std::filesystem::path& root,
	std::map<std::string, std::filesystem::path>& outFiles,
	std::string& outError)
{
	outFiles.clear();

	std::error_code ec;
	if (!std::filesystem::exists(root, ec)) {
		outError = "path_not_found: " + PathToUtf8(root);
		return false;
	}

	for (std::filesystem::recursive_directory_iterator it(root, ec), end; it != end; it.increment(ec)) {
		if (ec) {
			outError = "enumerate_failed: " + PathToUtf8(root);
			return false;
		}
		if (!it->is_regular_file()) {
			continue;
		}

		const std::filesystem::path relative = std::filesystem::relative(it->path(), root, ec);
		if (ec) {
			outError = "relative_path_failed: " + PathToUtf8(it->path());
			return false;
		}
		outFiles.emplace(relative.generic_string(), it->path());
	}

	return true;
}

bool CompareDirectoryTrees(
	const std::filesystem::path& leftRoot,
	const std::filesystem::path& rightRoot,
	std::string& outSummary)
{
	outSummary.clear();

	std::map<std::string, std::filesystem::path> leftFiles;
	std::map<std::string, std::filesystem::path> rightFiles;
	std::string error;
	if (!BuildFileMap(leftRoot, leftFiles, error)) {
		outSummary = error;
		return false;
	}
	if (!BuildFileMap(rightRoot, rightFiles, error)) {
		outSummary = error;
		return false;
	}

	size_t comparedCount = 0;
	for (const auto& [relativePath, leftPath] : leftFiles) {
		if (ShouldIgnorePathForRoundTripCompare(relativePath)) {
			continue;
		}

		const auto rightIt = rightFiles.find(relativePath);
		if (rightIt == rightFiles.end()) {
			outSummary = "missing_in_roundtrip: " + relativePath;
			return false;
		}

		const std::filesystem::path extension = leftPath.extension();
		if (extension == std::filesystem::path(L".json")) {
			if (!CompareJsonFile(leftPath, rightIt->second, outSummary)) {
				return false;
			}
			++comparedCount;
			continue;
		}
		if (extension == std::filesystem::path(L".txt") ||
			extension == std::filesystem::path(L".xml")) {
			if (!CompareNormalizedTextFile(leftPath, rightIt->second, outSummary)) {
				return false;
			}
			++comparedCount;
			continue;
		}

		std::vector<std::uint8_t> leftBytes;
		std::vector<std::uint8_t> rightBytes;
		if (!ReadFileBytes(leftPath, leftBytes, error)) {
			outSummary = error;
			return false;
		}
		if (!ReadFileBytes(rightIt->second, rightBytes, error)) {
			outSummary = error;
			return false;
		}
		if (leftBytes != rightBytes) {
			outSummary =
				"content_mismatch: " + relativePath +
				", left_bytes=" + std::to_string(leftBytes.size()) +
				", right_bytes=" + std::to_string(rightBytes.size());
			return false;
		}
		++comparedCount;
	}

	for (const auto& [relativePath, rightPath] : rightFiles) {
		(void)rightPath;
		if (ShouldIgnorePathForRoundTripCompare(relativePath)) {
			continue;
		}
		if (!leftFiles.contains(relativePath)) {
			outSummary = "extra_in_roundtrip: " + relativePath;
			return false;
		}
	}

	outSummary =
		"compared_files=" + std::to_string(comparedCount) +
		", left=" + PathToUtf8(leftRoot) +
		", right=" + PathToUtf8(rightRoot);
	return true;
}

int RunVerifyRoundTrip(
	const char* inputPath,
	const char* workDir,
	const char* outputPath,
	const e2txt::ReadOptions& readOptions = {})
{
	const std::filesystem::path root(workDir);
	const std::filesystem::path originalDir = root / "original_unpacked";
	const std::filesystem::path roundtripDir = root / "roundtrip_unpacked";
	std::error_code ec;
	std::filesystem::remove_all(root, ec);
	std::filesystem::create_directories(root, ec);

	std::string summary;
	std::string error;
	if (!DoUnpack(std::filesystem::path(inputPath), originalDir, summary, error, readOptions)) {
		return PrintStringResult("verify-roundtrip", -1, error.c_str());
	}
	std::filesystem::path writtenOutputPath;
	if (!DoPack(originalDir, std::filesystem::path(outputPath), summary, error, &writtenOutputPath)) {
		return PrintStringResult("verify-roundtrip", -1, error.c_str());
	}
	if (!DoUnpack(writtenOutputPath, roundtripDir, summary, error)) {
		return PrintStringResult("verify-roundtrip", -1, error.c_str());
	}

	std::string compareSummary;
	if (!CompareDirectoryTrees(originalDir, roundtripDir, compareSummary)) {
		return PrintStringResult("verify-roundtrip", -1, compareSummary.c_str());
	}

	return PrintStringResult("verify-roundtrip", 0, compareSummary.c_str());
}

int RunSupportLibraryDump(const char* inputPath, const char* outputPath = nullptr)
{
	const std::filesystem::path effectiveInputPath = ResolveAbsolutePath(std::filesystem::path(inputPath));
	if (!IsSupportLibraryFileExtension(effectiveInputPath)) {
		return PrintStringResult("decrypt-fne", -1, "support_library_file_type_unsupported");
	}

	const std::filesystem::path requestedOutputPath =
		outputPath == nullptr ? std::filesystem::path() : ResolveAbsolutePath(std::filesystem::path(outputPath));
	const std::filesystem::path effectiveOutputPath =
		ResolveSupportLibraryDumpOutputPath(effectiveInputPath, requestedOutputPath);

	std::string summary;
	std::string error;
	if (!support_library_public_info::DumpSupportLibraryPublicInfoToFile(
			effectiveInputPath,
			effectiveOutputPath,
			summary,
			error)) {
		return PrintStringResult("decrypt-fne", -1, error.c_str());
	}
	return PrintStringResult("decrypt-fne", 0, summary.c_str());
}

int RunDragDropUnpack(const char* inputPath, const UnpackOptions& unpackOptions = {})
{
	const std::filesystem::path input(inputPath);
	const std::filesystem::path outputDir = input.parent_path() / input.stem();

	std::string summary;
	std::string error;
	if (!DoUnpackInternal(input, outputDir, summary, error, unpackOptions)) {
		return PrintStringResult("unpack", -1, error.c_str());
	}
	return PrintStringResult("unpack", 0, summary.c_str());
}

int RunSelfUpdate(const bool force)
{
	const self_update::UpdateResult result = self_update::ScheduleSelfUpdate(APP_VERSION, force);
	return PrintStringResult("self-update", result.ok ? 0 : -1, result.message.c_str());
}

void PrintUsage()
{
	std::cout << Utf8Literal(u8"e-packager 用法:") << std::endl;
	std::cout << Utf8Literal(u8"  e-packager                           # 封包当前项目到 .\\pack\\<info.json sourceFileName>") << std::endl;
	std::cout << Utf8Literal(u8"  e-packager <input.e|input.ec> [--password <text>] [--main-only]       # 拆包 .e/.ec 文件到同目录下同名文件夹（拖放直接打开）") << std::endl;
	std::cout << Utf8Literal(u8"  e-packager <input.fne>               # 导出支持库公开接口到同目录 .txt（仅 Win32 版可用）") << std::endl;
	std::cout << Utf8Literal(u8"  e-packager unpack <input.e|input.ec> <output-dir> [--password <text>] [--main-only]    # 拆包到指定目录") << std::endl;
	std::cout << Utf8Literal(u8"  e-packager decrypt-fne <input.fne> [output.txt]      # 导出支持库公开接口单文件（仅 Win32 版可用）") << std::endl;
	std::cout << Utf8Literal(u8"  e-packager pack <input-dir> <output.e|output.ec> [--password <text>] [--compile-check ...]  # 封包，可选 AutoLinker 无头编译确认") << std::endl;
	std::cout << Utf8Literal(u8"       --compile-check [--eide <e.exe>] [--autolinker-test <AutoLinkerTest.exe>] [--compile-target auto|win_exe|win_console_exe|win_dll|ecom] [--compile-static] [--compile-timeout <seconds>]") << std::endl;
	std::cout << Utf8Literal(u8"  e-packager validate <input-dir> [--diagnostics text|json]  # 快速检查声明、基础语法和可确定的类型错误") << std::endl;
	std::cout << Utf8Literal(u8"  e-packager compile <input.e|input-dir> <output.exe|output.dll> [--diagnostics text|json] [--compile-mode semantic|legacy-blackmoon|blackmoon] [--arch host|x86|x64] [--subsystem auto|console|windows] [--legacy-blackmoon-mode asm|cpp|mfc] [--dll] [--define <macro>]... [--compiler <cl.exe>] [--linker <link.exe>] [--lib <lib-dir>] [--eide <e.exe>] [--legacy-blackmoon-dir <dir>] [--blackmoon-core-dir <dir>] [--blackmoon-x86-dir <dir>] [--blackmoon-x64-dir <dir>] [--x86-decoder <e-packager.exe>] [--blackmoon-timeout <seconds>]  # 默认 semantic；窗口工程自动使用 Windows 子系统") << std::endl;
	std::cout << Utf8Literal(u8"  e-packager compile-check <input.e|input.ec> [--eide <e.exe>] [--autolinker-test <AutoLinkerTest.exe>] [--compile-target ...] [--compile-static] [--compile-timeout <seconds>]  # 直接执行权威无头编译") << std::endl;
	std::cout << Utf8Literal(u8"  e-packager update <input-dir> [--add-ecom <file.ec>]... [--add-elib <name|file.fne>]... [--add-image <file|name=file>]... [--add-audio <file|name=file>]...   # 刷新派生内容并新增资源") << std::endl;
#if defined(_M_X64)
	std::cout << Utf8Literal(u8"  e-packager /update [--force]         # x64 构建不启用自更新") << std::endl;
#else
	std::cout << Utf8Literal(u8"  e-packager /update [--force]         # 从 GitHub Release 下载最新版本并替换当前 e-packager.exe") << std::endl;
#endif
	std::cout << Utf8Literal(u8"  e-packager compare-bundle <input.e|input.ec> <input-dir> [--password <text>]   # 比较原文件与目录") << std::endl;
	std::cout << Utf8Literal(u8"  e-packager roundtrip <input.e|input.ec> <work-dir> <output.e|output.ec> [--password <text>]      # 拆包再封包") << std::endl;
	std::cout << Utf8Literal(u8"  e-packager verify-roundtrip <input.e|input.ec> <work-dir> <output.e|output.ec> [--password <text>]  # 验证往返一致性") << std::endl;
	std::cout << Utf8Literal(u8"  e-packager version|--version|-v   # 查看当前程序版本") << std::endl;
}

}  // namespace

int RunCommand(int argc, char* argv[])
{
	if (argc < 2) {
		return RunDefaultPack();
	}

	const std::string command = argv[1];
	if (IsVersionCommand(command)) {
		PrintVersion();
		return EXIT_SUCCESS;
	}
	if (command == "help" || command == "--help" || command == "/?") {
		PrintUsage();
		return EXIT_SUCCESS;
	}
	if (command == "/update" || command == "self-update" || command == "--update") {
		bool force = false;
		for (int index = 2; index < argc; ++index) {
			const std::string option = argv[index];
			if (option == "--force") {
				force = true;
				continue;
			}
			PrintUsage();
			return EXIT_FAILURE;
		}
		return RunSelfUpdate(force);
	}
	if (command == "unpack") {
		if (argc < 4) {
			PrintUsage();
			return EXIT_FAILURE;
		}
		UnpackOptions unpackOptions;
		if (!ParseUnpackOptions(argc, argv, 4, unpackOptions)) {
			PrintUsage();
			return EXIT_FAILURE;
		}
		return RunUnpack(argv[2], argv[3], unpackOptions);
	}
	if (command == "decrypt-fne" || command == "dump-fne") {
		if (argc < 3 || argc > 4) {
			PrintUsage();
			return EXIT_FAILURE;
		}
		return RunSupportLibraryDump(argv[2], argc >= 4 ? argv[3] : nullptr);
	}
	if (command == "pack") {
		if (argc < 4) {
			PrintUsage();
			return EXIT_FAILURE;
		}
		PackCommandOptions packOptions;
		if (!ParsePackCommandOptions(argc, argv, 4, packOptions)) {
			PrintUsage();
			return EXIT_FAILURE;
		}
		return RunPack(argv[2], argv[3], packOptions);
	}
	if (command == "validate") {
		if (argc < 3) {
			PrintUsage();
			return EXIT_FAILURE;
		}
		DiagnosticOutputFormat format = DiagnosticOutputFormat::Text;
		for (int index = 3; index < argc; ++index) {
			const std::string option = argv[index];
			if (option == "--diagnostics" && index + 1 < argc) {
				if (!ParseDiagnosticOutputFormat(argv[++index], format)) {
					PrintUsage();
					return EXIT_FAILURE;
				}
				continue;
			}
			if (option.rfind("--diagnostics=", 0) == 0) {
				if (!ParseDiagnosticOutputFormat(option.substr(std::string("--diagnostics=").size()), format)) {
					PrintUsage();
					return EXIT_FAILURE;
				}
				continue;
			}
			PrintUsage();
			return EXIT_FAILURE;
		}
		return RunValidate(argv[2], format);
	}
	if (command == "compile") {
		if (argc < 4) {
			PrintUsage();
			return EXIT_FAILURE;
		}
		CompileCommandOptions commandOptions;
		ecompiler::Options& options = commandOptions.compilerOptions;
		DiagnosticOutputFormat diagnosticFormat = DiagnosticOutputFormat::Text;
		const auto appendBlackMoonCoreDirectory = [&options](const std::filesystem::path& directory) {
			const std::filesystem::path resolved = ResolveAbsolutePath(directory);
			if (options.blackMoonCoreDirectory.empty()) options.blackMoonCoreDirectory = resolved;
			options.blackMoonCoreDirectories.push_back(resolved);
		};
		const auto appendBlackMoonX64Directory = [&options](const std::filesystem::path& directory) {
			const std::filesystem::path resolved = ResolveAbsolutePath(directory);
			if (options.blackMoonX64Directory.empty()) options.blackMoonX64Directory = resolved;
			options.blackMoonX64Directories.push_back(resolved);
			if (options.blackMoonCoreDirectory.empty()) options.blackMoonCoreDirectory = resolved;
			options.blackMoonCoreDirectories.push_back(resolved);
		};
		for (int index = 4; index < argc; ++index) {
			const std::string option = argv[index];
			if (option == "--diagnostics" && index + 1 < argc) {
				if (!ParseDiagnosticOutputFormat(argv[++index], diagnosticFormat)) {
					PrintUsage();
					return EXIT_FAILURE;
				}
				continue;
			}
			if (option.rfind("--diagnostics=", 0) == 0) {
				if (!ParseDiagnosticOutputFormat(option.substr(std::string("--diagnostics=").size()), diagnosticFormat)) {
					PrintUsage();
					return EXIT_FAILURE;
				}
				continue;
			}
			if (option == "--dll") {
				options.buildDll = true;
				continue;
			}
			if (option == "--subsystem" && index + 1 < argc) {
				if (!ParseExecutableSubsystem(argv[++index], options.subsystem)) {
					PrintUsage();
					return EXIT_FAILURE;
				}
				continue;
			}
			if (option.rfind("--subsystem=", 0) == 0) {
				if (!ParseExecutableSubsystem(option.substr(std::string("--subsystem=").size()), options.subsystem)) {
					PrintUsage();
					return EXIT_FAILURE;
				}
				continue;
			}
			if (option == "--blackmoon") {
				options.compileMode = ecompiler::CompileMode::BlackMoonCompatibility;
				continue;
			}
			if (option == "--legacy-blackmoon") {
				options.compileMode = ecompiler::CompileMode::LegacyBlackMoon;
				continue;
			}
			if (option == "--arch" && index + 1 < argc) {
				if (!ParseTargetArchitecture(argv[++index], options.targetArchitecture)) {
					PrintUsage();
					return EXIT_FAILURE;
				}
				continue;
			}
			if (option.rfind("--arch=", 0) == 0) {
				if (!ParseTargetArchitecture(option.substr(std::string("--arch=").size()), options.targetArchitecture)) {
					PrintUsage();
					return EXIT_FAILURE;
				}
				continue;
			}
			if ((option == "--compile-mode" || option == "--backend") && index + 1 < argc) {
				if (!ParseCompileMode(argv[++index], options.compileMode)) {
					PrintUsage();
					return EXIT_FAILURE;
				}
				continue;
			}
			if (option.rfind("--compile-mode=", 0) == 0 || option.rfind("--backend=", 0) == 0) {
				const std::string prefix = option.rfind("--compile-mode=", 0) == 0 ? "--compile-mode=" : "--backend=";
				if (!ParseCompileMode(option.substr(prefix.size()), options.compileMode)) {
					PrintUsage();
					return EXIT_FAILURE;
				}
				continue;
			}
			if ((option == "--blackmoon-mode" || option == "--legacy-blackmoon-mode") && index + 1 < argc) {
				if (!ParseBlackMoonMode(argv[++index], options.blackMoonMode)) {
					PrintUsage();
					return EXIT_FAILURE;
				}
				options.compileMode = ecompiler::CompileMode::LegacyBlackMoon;
				continue;
			}
			if (option.rfind("--blackmoon-mode=", 0) == 0 || option.rfind("--legacy-blackmoon-mode=", 0) == 0) {
				const std::string prefix = option.rfind("--legacy-blackmoon-mode=", 0) == 0
					? "--legacy-blackmoon-mode=" : "--blackmoon-mode=";
				if (!ParseBlackMoonMode(option.substr(prefix.size()), options.blackMoonMode)) {
					PrintUsage();
					return EXIT_FAILURE;
				}
				options.compileMode = ecompiler::CompileMode::LegacyBlackMoon;
				continue;
			}
			if ((option == "--compiler" || option == "--linker" || option == "--lib") && index + 1 < argc) {
				if (option == "--compiler") options.compilerPath = ResolveAbsolutePath(std::filesystem::path(argv[++index]));
				else if (option == "--linker") options.linkerPath = ResolveAbsolutePath(std::filesystem::path(argv[++index]));
				else options.libraryPath = ResolveAbsolutePath(std::filesystem::path(argv[++index]));
				continue;
			}
			if (option.rfind("--compiler=", 0) == 0) {
				options.compilerPath = ResolveAbsolutePath(std::filesystem::path(option.substr(std::string("--compiler=").size())));
				continue;
			}
			if (option.rfind("--linker=", 0) == 0) {
				options.linkerPath = ResolveAbsolutePath(std::filesystem::path(option.substr(std::string("--linker=").size())));
				continue;
			}
			if (option.rfind("--lib=", 0) == 0) {
				options.libraryPath = ResolveAbsolutePath(std::filesystem::path(option.substr(std::string("--lib=").size())));
				continue;
			}
			if ((option == "--define" || option == "-D") && index + 1 < argc) {
				options.conditionMacros.emplace_back(argv[++index]);
				continue;
			}
			if (option == "--eide" && index + 1 < argc) {
				options.eIdePath = ResolveAbsolutePath(std::filesystem::path(argv[++index]));
				continue;
			}
			if ((option == "--blackmoon-dir" || option == "--legacy-blackmoon-dir") && index + 1 < argc) {
				options.blackMoonDirectory = ResolveAbsolutePath(std::filesystem::path(argv[++index]));
				continue;
			}
			if ((option == "--blackmoon-core-dir" || option == "--blackmoon-x86-dir") && index + 1 < argc) {
				appendBlackMoonCoreDirectory(std::filesystem::path(argv[++index]));
				continue;
			}
			if (option.rfind("--blackmoon-core-dir=", 0) == 0 || option.rfind("--blackmoon-x86-dir=", 0) == 0) {
				const std::string prefix = option.rfind("--blackmoon-x86-dir=", 0) == 0
					? "--blackmoon-x86-dir=" : "--blackmoon-core-dir=";
				appendBlackMoonCoreDirectory(std::filesystem::path(option.substr(prefix.size())));
				continue;
			}
			if (option == "--blackmoon-x64-dir" && index + 1 < argc) {
				appendBlackMoonX64Directory(std::filesystem::path(argv[++index]));
				continue;
			}
			if (option.rfind("--blackmoon-x64-dir=", 0) == 0) {
				appendBlackMoonX64Directory(std::filesystem::path(option.substr(std::string("--blackmoon-x64-dir=").size())));
				continue;
			}
			if (option == "--x86-decoder" && index + 1 < argc) {
				options.x86DecoderPath = ResolveAbsolutePath(std::filesystem::path(argv[++index]));
				continue;
			}
			if (option.rfind("--x86-decoder=", 0) == 0) {
				options.x86DecoderPath = ResolveAbsolutePath(std::filesystem::path(option.substr(std::string("--x86-decoder=").size())));
				continue;
			}
			if (option == "--blackmoon-timeout" && index + 1 < argc) {
				if (!TryParseUnsignedInt(argv[++index], options.blackMoonTimeoutSeconds)) {
					PrintUsage();
					return EXIT_FAILURE;
				}
				continue;
			}
			PrintUsage();
			return EXIT_FAILURE;
		}
		return RunCompile(argv[2], argv[3], std::move(options), diagnosticFormat);
	}
	if (command == "compile-check") {
		if (argc < 3) {
			PrintUsage();
			return EXIT_FAILURE;
		}
		autolinker_compile_check::Options options;
		if (!ParseStandaloneCompileCheckOptions(argc, argv, 3, options)) {
			PrintUsage();
			return EXIT_FAILURE;
		}
		return RunCompileCheck(argv[2], options);
	}
	if (command == "update") {
		if (argc < 3) {
			PrintUsage();
			return EXIT_FAILURE;
		}

		std::vector<std::string> addEcomInputs;
		std::vector<std::string> addElibInputs;
		std::vector<std::string> addImageInputs;
		std::vector<std::string> addAudioInputs;
		for (int index = 3; index < argc; ++index) {
			const std::string option = argv[index];
			if (option == "--add-ecom") {
				if (index + 1 >= argc) {
					PrintUsage();
					return EXIT_FAILURE;
				}
				addEcomInputs.emplace_back(argv[++index]);
				continue;
			}
			if (option == "--add-elib") {
				if (index + 1 >= argc) {
					PrintUsage();
					return EXIT_FAILURE;
				}
				addElibInputs.emplace_back(argv[++index]);
				continue;
			}
			if (option == "--add-image") {
				if (index + 1 >= argc) {
					PrintUsage();
					return EXIT_FAILURE;
				}
				addImageInputs.emplace_back(argv[++index]);
				continue;
			}
			if (option == "--add-audio") {
				if (index + 1 >= argc) {
					PrintUsage();
					return EXIT_FAILURE;
				}
				addAudioInputs.emplace_back(argv[++index]);
				continue;
			}

			PrintUsage();
			return EXIT_FAILURE;
		}

		return RunUpdate(argv[2], addEcomInputs, addElibInputs, addImageInputs, addAudioInputs);
	}
	if (command == "compare-bundle") {
		if (argc < 4) {
			PrintUsage();
			return EXIT_FAILURE;
		}
		e2txt::ReadOptions readOptions;
		if (!ParseReadOptions(argc, argv, 4, readOptions)) {
			PrintUsage();
			return EXIT_FAILURE;
		}
		return RunCompareBundle(argv[2], argv[3], readOptions);
	}
	if (command == "roundtrip") {
		if (argc < 5) {
			PrintUsage();
			return EXIT_FAILURE;
		}
		e2txt::ReadOptions readOptions;
		if (!ParseReadOptions(argc, argv, 5, readOptions)) {
			PrintUsage();
			return EXIT_FAILURE;
		}
		return RunRoundTrip(argv[2], argv[3], argv[4], readOptions);
	}
	if (command == "verify-roundtrip") {
		if (argc < 5) {
			PrintUsage();
			return EXIT_FAILURE;
		}
		e2txt::ReadOptions readOptions;
		if (!ParseReadOptions(argc, argv, 5, readOptions)) {
			PrintUsage();
			return EXIT_FAILURE;
		}
		return RunVerifyRoundTrip(argv[2], argv[3], argv[4], readOptions);
	}

	// Drag-and-drop: a single .e/.ec/.fne file path passed directly
	if (argc == 2 || argc >= 4) {
		std::filesystem::path inputPath(command);
		std::string ext = inputPath.extension().string();
		std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
		if (ext == ".e" || ext == ".ec") {
			UnpackOptions unpackOptions;
			if (!ParseUnpackOptions(argc, argv, 2, unpackOptions)) {
				PrintUsage();
				return EXIT_FAILURE;
			}
			return RunDragDropUnpack(argv[1], unpackOptions);
		}
		if (argc == 2 && IsSupportLibraryFileExtension(inputPath)) {
			return RunSupportLibraryDump(argv[1]);
		}
	}

	PrintUsage();
	return EXIT_FAILURE;
}

int MainImpl(int argc, char* argv[])
{
	ConfigureConsoleForUtf8();
	const bool versionInvocation = IsVersionInvocation(argc, argv);
	if (!versionInvocation) {
		std::cerr << "e-packager " << APP_VERSION << std::endl;
	}

	// 后台异步检查更新（预发布版本跳过）。
	std::future<std::string> updateFuture;
#if !defined(_M_X64)
	if (!versionInvocation && !update_check::IsPreRelease(APP_VERSION)) {
		updateFuture = std::async(std::launch::async, update_check::FetchLatestTag);
	}
#endif

	e2txt::ClearRuntimeWarnings();
	const int result = RunCommand(argc, argv);
	for (const auto& warning : e2txt::ConsumeRuntimeWarnings()) {
		std::cerr << Utf8Literal(u8"提示: ") << warning << std::endl;
	}

	// 主命令执行完毕后，检查是否有可用的新版本。
	if (updateFuture.valid()) {
		if (updateFuture.wait_for(std::chrono::milliseconds(1500)) == std::future_status::ready) {
			const std::string latest = updateFuture.get();
			if (!latest.empty() && update_check::IsNewer(latest, APP_VERSION)) {
				std::cerr << Utf8Literal(u8"提示: 新版本可用 ") << latest
					<< " -> https://github.com/aiqinxuancai/e-packager/releases/latest"
					<< std::endl;
			}
		}
	}

	return result;
}

int main(int argc, char* argv[])
{
	try {
		return MainImpl(argc, argv);
	}
	catch (const std::exception& ex) {
		std::cerr << "fatal: " << ex.what() << std::endl;
		return EXIT_FAILURE;
	}
	catch (...) {
		std::cerr << "fatal: unknown exception" << std::endl;
		return EXIT_FAILURE;
	}
}
