#include <cstdlib>
#include <Windows.h>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <future>
#include <iostream>
#include <map>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "..\thirdparty\json.hpp"
#include "EFolderCodec.h"
#include "PathHelper.h"
#include "SupportLibraryPublicInfo.h"
#include "UpdateCheck.h"
#include "WorkspaceProjectSupport.h"
#include "e2txt.h"
#include "version.h"

namespace {

using json = nlohmann::json;

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
	bool unpackDependencyModules = true;
};

struct DependencyModuleAnnotation {
	size_t dependencyIndex = 0;
	std::string resolvedPath;
	std::string localWorkspace;
};

struct DependencyModuleExportResult {
	size_t exportedCount = 0;
	std::vector<DependencyModuleAnnotation> annotations;
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

std::string ToLowerAsciiCopy(std::string text)
{
	std::transform(text.begin(), text.end(), text.begin(), [](unsigned char ch) {
		return static_cast<char>(std::tolower(ch));
	});
	return text;
}

void PushUniquePathCandidate(std::vector<std::filesystem::path>& outPaths, const std::filesystem::path& path)
{
	if (path.empty()) {
		return;
	}
	const std::filesystem::path normalized = path.lexically_normal();
	if (std::find(outPaths.begin(), outPaths.end(), normalized) == outPaths.end()) {
		outPaths.push_back(normalized);
	}
}

std::vector<std::filesystem::path> BuildDependencyModuleCandidatePaths(
	const std::filesystem::path& sourcePath,
	const std::string& modulePathText)
{
	std::vector<std::filesystem::path> candidates;
	std::string normalizedText = TrimAsciiCopy(modulePathText);
	if (normalizedText.empty()) {
		return candidates;
	}

	if (normalizedText.size() >= 2 &&
		normalizedText.front() == '"' &&
		normalizedText.back() == '"') {
		normalizedText = normalizedText.substr(1, normalizedText.size() - 2);
	}
	if (!normalizedText.empty() && normalizedText.front() == '$') {
		normalizedText.erase(normalizedText.begin());
	}

	std::filesystem::path filePath(normalizedText);
	if (filePath.extension().empty()) {
		filePath += ".ec";
	}

	if (filePath.is_absolute()) {
		PushUniquePathCandidate(candidates, filePath);
		return candidates;
	}

	const auto addBaseCandidates = [&](const std::filesystem::path& baseDir) {
		if (baseDir.empty()) {
			return;
		}

		PushUniquePathCandidate(candidates, baseDir / filePath);
		PushUniquePathCandidate(candidates, baseDir / "ecom" / filePath);

		std::filesystem::path current = baseDir;
		while (!current.empty()) {
			PushUniquePathCandidate(candidates, current / "ecom" / filePath);
			if (current == current.root_path()) {
				break;
			}
			current = current.parent_path();
		}
	};

	std::error_code ec;
	if (!sourcePath.empty()) {
		addBaseCandidates(sourcePath.parent_path());
	}
	addBaseCandidates(std::filesystem::current_path(ec));
	addBaseCandidates(std::filesystem::path(GetBasePath()));
	for (const auto& registeredBaseDir : GetRegisteredEplOpenCommandBaseDirs()) {
		addBaseCandidates(registeredBaseDir);
	}

	return candidates;
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
	const e2txt::ProjectBundle& bundle)
{
	DependencyModuleExportResult result;
	std::filesystem::path ecomRoot = outputDir / "ecom";
	std::unordered_set<std::string> exportedPaths;
	std::unordered_map<std::string, std::string> localWorkspacesByResolvedPath;
	std::unordered_map<std::string, int> exportedDirNames;

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
		if (const auto localWorkspaceIt = localWorkspacesByResolvedPath.find(resolvedKey);
			localWorkspaceIt != localWorkspacesByResolvedPath.end()) {
			result.annotations.push_back(DependencyModuleAnnotation {
				.dependencyIndex = dependencyIndex,
				.resolvedPath = resolvedKey,
				.localWorkspace = localWorkspaceIt->second,
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

		if (!exportedPaths.insert(resolvedKey).second) {
			result.annotations.push_back(DependencyModuleAnnotation {
				.dependencyIndex = dependencyIndex,
				.resolvedPath = resolvedKey,
			});
			continue;
		}

		std::string childSummary;
		std::string childError;
		const UnpackOptions childOptions {
			.writeAgentsMarkdown = false,
			.unpackDependencyModules = false,
		};
		if (!DoUnpackInternal(resolvedPath, moduleOutputDir, childSummary, childError, childOptions)) {
			e2txt::AddRuntimeWarning(
				Utf8Literal(u8"易模块依赖解包失败：") + PathToUtf8(resolvedPath) +
				" => " + childError);
			result.annotations.push_back(DependencyModuleAnnotation {
				.dependencyIndex = dependencyIndex,
				.resolvedPath = resolvedKey,
			});
			continue;
		}

		localWorkspacesByResolvedPath[resolvedKey] = localWorkspace;
		result.annotations.push_back(DependencyModuleAnnotation {
			.dependencyIndex = dependencyIndex,
			.resolvedPath = resolvedKey,
			.localWorkspace = localWorkspace,
		});
		++result.exportedCount;
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
	DependencyRefreshResult& outResult,
	std::string& outError)
{
	outResult = {};
	outError.clear();

	if (!RemoveGeneratedDependencyArtifacts(outputDir, outError)) {
		return false;
	}

	if (exportEComModules) {
		const DependencyModuleExportResult ecomResult = ExportDependencyModules(sourcePath, outputDir, bundle);
		outResult.exportedEComModules = ecomResult.exportedCount;
		outResult.annotations.insert(
			outResult.annotations.end(),
			ecomResult.annotations.begin(),
			ecomResult.annotations.end());
	}

	const auto elibResult = support_library_public_info::ExportDependencies(sourcePath, outputDir, bundle.dependencies);
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
		if (!generator.GenerateBundle(PathToUtf8(effectiveInputPath), ecBundle, &outError)) {
			return false;
		}

		e2txt::ProjectBundle bridgeSourceBundle = ecBundle;
		bridgeSourceBundle.nativeSourceBytes.clear();
		bridgeSourceBundle.nativeBundleDigest.clear();

		e2txt::Restorer restorer;
		std::vector<std::uint8_t> eBytes;
		if (!restorer.RestoreBundleToBytes(bridgeSourceBundle, eBytes, &outError)) {
			return false;
		}

		std::filesystem::path bridgeSourcePath = effectiveInputPath;
		bridgeSourcePath += L".e";
		if (!generator.GenerateBundleFromBytes(eBytes, PathToUtf8(bridgeSourcePath), bundle, &outError)) {
			return false;
		}
		bundle.sourcePath = PathToUtf8(effectiveInputPath);
		bundle.sourceFileKind = e2txt::SourceFileKind::EC;
		ClearNativeReuseState(bundle);
		bundle.publicHeaderText = ecBundle.publicHeaderText;
		workspaceOptions.defaultPackOutputFileName = PathToUtf8(effectiveInputPath.filename()) + ".e";
	}
	else {
		if (!generator.GenerateBundle(PathToUtf8(effectiveInputPath), bundle, &outError)) {
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
	if (!RefreshDependencyArtifacts(
			effectiveInputPath,
			effectiveOutputDir,
			bundle,
			options.unpackDependencyModules && extension != ".ec",
			dependencyRefreshResult,
			outError)) {
		return false;
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
	std::string& outError)
{
	return DoUnpackInternal(inputPath, outputDir, outSummary, outError, UnpackOptions {});
}

bool DoPack(
	const std::filesystem::path& inputDir,
	const std::filesystem::path& outputPath,
	std::string& outSummary,
	std::string& outError,
	std::filesystem::path* outWrittenOutputPath = nullptr)
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
	if (!restorer.RestoreBundleToFile(bundle, PathToUtf8(effectiveOutputPath), &outSummary, &outError)) {
		return false;
	}
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

int RunUpdate(
	const std::filesystem::path& inputDir,
	const std::vector<std::string>& addEcomInputs,
	const std::vector<std::string>& addElibInputs)
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

	if (!codec.WriteBundle(bundle, PathToUtf8(effectiveInputDir), &error)) {
		return PrintStringResult("update", -1, error.c_str());
	}

	DependencyRefreshResult refreshResult;
	if (!RefreshDependencyArtifacts(
			workspaceSourcePath,
			effectiveInputDir,
			bundle,
			bundle.sourceFileKind != e2txt::SourceFileKind::EC,
			refreshResult,
			error)) {
		return PrintStringResult("update", -1, error.c_str());
	}

	const std::string summary =
		"dependencies_added=" + std::to_string(addedEcomCount + addedElibCount) +
		", add_ecom=" + std::to_string(addedEcomCount) +
		", add_elib=" + std::to_string(addedElibCount) +
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

int RunUnpack(const char* inputPath, const char* outputDir)
{
	std::string summary;
	std::string error;
	if (!DoUnpack(std::filesystem::path(inputPath), std::filesystem::path(outputDir), summary, error)) {
		return PrintStringResult("unpack", -1, error.c_str());
	}
	return PrintStringResult("unpack", 0, summary.c_str());
}

int RunPack(const char* inputDir, const char* outputPath)
{
	std::string summary;
	std::string error;
	if (!DoPack(std::filesystem::path(inputDir), std::filesystem::path(outputPath), summary, error)) {
		return PrintStringResult("pack", -1, error.c_str());
	}
	return PrintStringResult("pack", 0, summary.c_str());
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

int RunCompareBundle(const char* inputPath, const char* inputDir)
{
	e2txt::Generator generator;
	e2txt::BundleDirectoryCodec codec;
	e2txt::ProjectBundle bundleFromE;
	e2txt::ProjectBundle bundleFromDir;
	std::string error;
	const std::filesystem::path effectiveInputPath = ResolveAbsolutePath(std::filesystem::path(inputPath));
	const std::filesystem::path effectiveInputDir = ResolveAbsolutePath(std::filesystem::path(inputDir));
	if (!generator.GenerateBundle(PathToUtf8(effectiveInputPath), bundleFromE, &error)) {
		return PrintStringResult("compare-bundle", -1, error.c_str());
	}
	if (!codec.ReadBundle(PathToUtf8(effectiveInputDir), bundleFromDir, &error)) {
		return PrintStringResult("compare-bundle", -1, error.c_str());
	}
	const std::string summary = BuildBundleDigestCompareText(bundleFromE, bundleFromDir);
	return PrintStringResult("compare-bundle", 0, summary.c_str());
}

int RunRoundTrip(const char* inputPath, const char* workDir, const char* outputPath)
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
	if (!DoUnpack(effectiveInputPath, unpackDir, summary, error)) {
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
	value.erase("nativeBundleDigest");
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
	return relativePath.starts_with("src/.native_");
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

int RunVerifyRoundTrip(const char* inputPath, const char* workDir, const char* outputPath)
{
	const std::filesystem::path root(workDir);
	const std::filesystem::path originalDir = root / "original_unpacked";
	const std::filesystem::path roundtripDir = root / "roundtrip_unpacked";
	std::error_code ec;
	std::filesystem::remove_all(root, ec);
	std::filesystem::create_directories(root, ec);

	std::string summary;
	std::string error;
	if (!DoUnpack(std::filesystem::path(inputPath), originalDir, summary, error)) {
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

int RunDragDropUnpack(const char* inputPath)
{
	const std::filesystem::path input(inputPath);
	const std::filesystem::path outputDir = input.parent_path() / input.stem();

	std::string summary;
	std::string error;
	if (!DoUnpack(input, outputDir, summary, error)) {
		return PrintStringResult("unpack", -1, error.c_str());
	}
	return PrintStringResult("unpack", 0, summary.c_str());
}

void PrintUsage()
{
	std::cout << Utf8Literal(u8"e-packager 用法:") << std::endl;
	std::cout << Utf8Literal(u8"  e-packager                           # 封包当前项目到 .\\pack\\<info.json sourceFileName>") << std::endl;
	std::cout << Utf8Literal(u8"  e-packager <input.e|input.ec>       # 拆包 .e/.ec 文件到同目录下同名文件夹（拖放直接打开）") << std::endl;
	std::cout << Utf8Literal(u8"  e-packager unpack <input.e|input.ec> <output-dir>    # 拆包到指定目录") << std::endl;
	std::cout << Utf8Literal(u8"  e-packager pack <input-dir> <output.e|output.ec>      # 将目录封包为 .e/.ec 文件") << std::endl;
	std::cout << Utf8Literal(u8"  e-packager update <input-dir> [--add-ecom <file.ec>]... [--add-elib <name|file.fne>]...   # 刷新 ecom/elib 派生内容") << std::endl;
	std::cout << Utf8Literal(u8"  e-packager compare-bundle <input.e|input.ec> <input-dir>   # 比较原文件与目录") << std::endl;
	std::cout << Utf8Literal(u8"  e-packager roundtrip <input.e|input.ec> <work-dir> <output.e|output.ec>      # 拆包再封包") << std::endl;
	std::cout << Utf8Literal(u8"  e-packager verify-roundtrip <input.e|input.ec> <work-dir> <output.e|output.ec>  # 验证往返一致性") << std::endl;
}

}  // namespace

int RunCommand(int argc, char* argv[])
{
	if (argc < 2) {
		return RunDefaultPack();
	}

	const std::string command = argv[1];
	if (command == "help" || command == "--help" || command == "/?") {
		PrintUsage();
		return EXIT_SUCCESS;
	}
	if (command == "unpack") {
		if (argc != 4) {
			PrintUsage();
			return EXIT_FAILURE;
		}
		return RunUnpack(argv[2], argv[3]);
	}
	if (command == "pack") {
		if (argc != 4) {
			PrintUsage();
			return EXIT_FAILURE;
		}
		return RunPack(argv[2], argv[3]);
	}
	if (command == "update") {
		if (argc < 3) {
			PrintUsage();
			return EXIT_FAILURE;
		}

		std::vector<std::string> addEcomInputs;
		std::vector<std::string> addElibInputs;
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

			PrintUsage();
			return EXIT_FAILURE;
		}

		return RunUpdate(argv[2], addEcomInputs, addElibInputs);
	}
	if (command == "compare-bundle") {
		if (argc != 4) {
			PrintUsage();
			return EXIT_FAILURE;
		}
		return RunCompareBundle(argv[2], argv[3]);
	}
	if (command == "roundtrip") {
		if (argc != 5) {
			PrintUsage();
			return EXIT_FAILURE;
		}
		return RunRoundTrip(argv[2], argv[3], argv[4]);
	}
	if (command == "verify-roundtrip") {
		if (argc != 5) {
			PrintUsage();
			return EXIT_FAILURE;
		}
		return RunVerifyRoundTrip(argv[2], argv[3], argv[4]);
	}

	// Drag-and-drop: a single .e/.ec file path passed directly
	if (argc == 2) {
		std::filesystem::path inputPath(command);
		std::string ext = inputPath.extension().string();
		std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
		if (ext == ".e" || ext == ".ec") {
			return RunDragDropUnpack(argv[1]);
		}
	}

	PrintUsage();
	return EXIT_FAILURE;
}

int main(int argc, char* argv[])
{
	ConfigureConsoleForUtf8();
	std::cerr << "e-packager " << APP_VERSION << std::endl;

	// 后台异步检查更新（预发布版本跳过）。
	std::future<std::string> updateFuture;
	if (!update_check::IsPreRelease(APP_VERSION)) {
		updateFuture = std::async(std::launch::async, update_check::FetchLatestTag);
	}

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
