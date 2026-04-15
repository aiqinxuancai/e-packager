#include <cstdlib>
#include <Windows.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <string>
#include <vector>

#include "..\thirdparty\json.hpp"
#include "EFolderCodec.h"
#include "WorkspaceProjectSupport.h"
#include "e2txt.h"

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

std::string WideToUtf8(const std::wstring& text)
{
	if (text.empty()) {
		return std::string();
	}

	const int utf8Len = WideCharToMultiByte(
		CP_UTF8,
		0,
		text.data(),
		static_cast<int>(text.size()),
		nullptr,
		0,
		nullptr,
		nullptr);
	if (utf8Len <= 0) {
		return std::string();
	}

	std::string utf8(static_cast<size_t>(utf8Len), '\0');
	if (WideCharToMultiByte(
			CP_UTF8,
			0,
			text.data(),
			static_cast<int>(text.size()),
			utf8.data(),
			utf8Len,
			nullptr,
			nullptr) <= 0) {
		return std::string();
	}
	return utf8;
}

std::string PathToUtf8(const std::filesystem::path& path)
{
	return WideToUtf8(path.wstring());
}

bool DoUnpack(const std::string& inputPath, const std::string& outputDir, std::string& outSummary, std::string& outError)
{
	e2txt::Generator generator;
	e2txt::ProjectBundle bundle;
	if (!generator.GenerateBundle(inputPath, bundle, &outError)) {
		return false;
	}

	e2txt::BundleDirectoryCodec codec;
	if (!codec.WriteBundle(bundle, outputDir, &outError)) {
		return false;
	}
	if (!workspace_support::WriteWorkspaceFiles(std::filesystem::path(inputPath), std::filesystem::path(outputDir), outError)) {
		return false;
	}

	outSummary =
		"source_files=" + std::to_string(bundle.sourceFiles.size()) +
		", form_files=" + std::to_string(bundle.formFiles.size()) +
		", resources=" + std::to_string(bundle.resources.size()) +
		", output=" + outputDir;
	return true;
}

bool DoPack(const std::string& inputDir, const std::string& outputPath, std::string& outSummary, std::string& outError)
{
	e2txt::BundleDirectoryCodec codec;
	e2txt::ProjectBundle bundle;
	if (!codec.ReadBundle(inputDir, bundle, &outError)) {
		return false;
	}

	e2txt::Restorer restorer;
	if (!restorer.RestoreBundleToFile(bundle, outputPath, &outSummary, &outError)) {
		return false;
	}
	return true;
}

std::string ResourceDataDigest(const e2txt::BundleBinaryResource& resource)
{
	return e2txt::ComputeTextDigest(std::string(resource.data.begin(), resource.data.end()));
}

std::string BuildBundleDigestCompareText(const e2txt::ProjectBundle& fromE, const e2txt::ProjectBundle& fromDir)
{
	std::ostringstream stream;
	const std::string digestFromE = e2txt::ComputeBundleDigest(fromE);
	const std::string digestFromDir = e2txt::ComputeBundleDigest(fromDir);
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

	if (fromE.projectName != fromDir.projectName) {
		appendValueMismatch("projectName", fromE.projectName, fromDir.projectName);
		return stream.str();
	}
	if (fromE.versionText != fromDir.versionText) {
		appendValueMismatch("versionText", fromE.versionText, fromDir.versionText);
		return stream.str();
	}
	if (fromE.dependencies.size() != fromDir.dependencies.size()) {
		stream << "mismatch=dependencies.size\nleft=" << fromE.dependencies.size()
			<< "\nright=" << fromDir.dependencies.size() << "\n";
		return stream.str();
	}
	for (size_t index = 0; index < fromE.dependencies.size(); ++index) {
		const auto& left = fromE.dependencies[index];
		const auto& right = fromDir.dependencies[index];
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
	if (fromE.sourceFiles.size() != fromDir.sourceFiles.size()) {
		stream << "mismatch=sourceFiles.size\nleft=" << fromE.sourceFiles.size()
			<< "\nright=" << fromDir.sourceFiles.size() << "\n";
		return stream.str();
	}
	for (size_t index = 0; index < fromE.sourceFiles.size(); ++index) {
		const auto& left = fromE.sourceFiles[index];
		const auto& right = fromDir.sourceFiles[index];
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
	if (fromE.formFiles.size() != fromDir.formFiles.size()) {
		stream << "mismatch=formFiles.size\nleft=" << fromE.formFiles.size()
			<< "\nright=" << fromDir.formFiles.size() << "\n";
		return stream.str();
	}
	for (size_t index = 0; index < fromE.formFiles.size(); ++index) {
		const auto& left = fromE.formFiles[index];
		const auto& right = fromDir.formFiles[index];
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
	if (fromE.dataTypeText != fromDir.dataTypeText) {
		appendValueMismatch("dataTypeText.digest", e2txt::ComputeTextDigest(fromE.dataTypeText), e2txt::ComputeTextDigest(fromDir.dataTypeText));
		return stream.str();
	}
	if (fromE.dllDeclareText != fromDir.dllDeclareText) {
		appendValueMismatch("dllDeclareText.digest", e2txt::ComputeTextDigest(fromE.dllDeclareText), e2txt::ComputeTextDigest(fromDir.dllDeclareText));
		return stream.str();
	}
	if (fromE.constantText != fromDir.constantText) {
		appendValueMismatch("constantText.digest", e2txt::ComputeTextDigest(fromE.constantText), e2txt::ComputeTextDigest(fromDir.constantText));
		return stream.str();
	}
	if (fromE.globalText != fromDir.globalText) {
		appendValueMismatch("globalText.digest", e2txt::ComputeTextDigest(fromE.globalText), e2txt::ComputeTextDigest(fromDir.globalText));
		return stream.str();
	}
	if (fromE.resources.size() != fromDir.resources.size()) {
		stream << "mismatch=resources.size\nleft=" << fromE.resources.size()
			<< "\nright=" << fromDir.resources.size() << "\n";
		return stream.str();
	}
	for (size_t index = 0; index < fromE.resources.size(); ++index) {
		const auto& left = fromE.resources[index];
		const auto& right = fromDir.resources[index];
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
	if (fromE.folderAllocatedKey != fromDir.folderAllocatedKey) {
		stream << "mismatch=folderAllocatedKey\nleft=" << fromE.folderAllocatedKey
			<< "\nright=" << fromDir.folderAllocatedKey << "\n";
		return stream.str();
	}
	if (fromE.rootChildKeys != fromDir.rootChildKeys) {
		stream << "mismatch=rootChildKeys\nleft_count=" << fromE.rootChildKeys.size()
			<< "\nright_count=" << fromDir.rootChildKeys.size() << "\n";
		for (size_t index = 0; index < (std::min)(fromE.rootChildKeys.size(), fromDir.rootChildKeys.size()); ++index) {
			if (fromE.rootChildKeys[index] != fromDir.rootChildKeys[index]) {
				stream << "first_diff_index=" << index << "\n"
					<< "left=" << fromE.rootChildKeys[index] << "\n"
					<< "right=" << fromDir.rootChildKeys[index] << "\n";
				return stream.str();
			}
		}
		return stream.str();
	}
	if (fromE.folders.size() != fromDir.folders.size()) {
		stream << "mismatch=folders.size\nleft=" << fromE.folders.size()
			<< "\nright=" << fromDir.folders.size() << "\n";
		return stream.str();
	}
	for (size_t index = 0; index < fromE.folders.size(); ++index) {
		const auto& left = fromE.folders[index];
		const auto& right = fromDir.folders[index];
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
	if (fromE.windowBindings.size() != fromDir.windowBindings.size()) {
		stream << "mismatch=windowBindings.size\nleft=" << fromE.windowBindings.size()
			<< "\nright=" << fromDir.windowBindings.size() << "\n";
		return stream.str();
	}
	for (size_t index = 0; index < fromE.windowBindings.size(); ++index) {
		const auto& left = fromE.windowBindings[index];
		const auto& right = fromDir.windowBindings[index];
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
	if (!DoUnpack(inputPath, outputDir, summary, error)) {
		return PrintStringResult("unpack", -1, error.c_str());
	}
	return PrintStringResult("unpack", 0, summary.c_str());
}

int RunPack(const char* inputDir, const char* outputPath)
{
	std::string summary;
	std::string error;
	if (!DoPack(inputDir, outputPath, summary, error)) {
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
	if (!DoPack(PathToUtf8(projectRoot), PathToUtf8(outputPath), summary, error)) {
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
	if (!generator.GenerateBundle(inputPath, bundleFromE, &error)) {
		return PrintStringResult("compare-bundle", -1, error.c_str());
	}
	if (!codec.ReadBundle(inputDir, bundleFromDir, &error)) {
		return PrintStringResult("compare-bundle", -1, error.c_str());
	}
	const std::string summary = BuildBundleDigestCompareText(bundleFromE, bundleFromDir);
	return PrintStringResult("compare-bundle", 0, summary.c_str());
}

int RunRoundTrip(const char* inputPath, const char* workDir, const char* outputPath)
{
	const std::filesystem::path root(workDir);
	const std::filesystem::path unpackDir = root / "unpacked";
	std::error_code ec;
	std::filesystem::remove_all(unpackDir, ec);
	std::filesystem::create_directories(unpackDir, ec);

	std::string summary;
	std::string error;
	if (!DoUnpack(inputPath, unpackDir.string(), summary, error)) {
		return PrintStringResult("roundtrip", -1, error.c_str());
	}
	if (!DoPack(unpackDir.string(), outputPath, summary, error)) {
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
	if (!DoUnpack(inputPath, originalDir.string(), summary, error)) {
		return PrintStringResult("verify-roundtrip", -1, error.c_str());
	}
	if (!DoPack(originalDir.string(), outputPath, summary, error)) {
		return PrintStringResult("verify-roundtrip", -1, error.c_str());
	}
	if (!DoUnpack(outputPath, roundtripDir.string(), summary, error)) {
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
	const std::string outputDirStr = outputDir.string();

	std::string summary;
	std::string error;
	if (!DoUnpack(inputPath, outputDirStr, summary, error)) {
		return PrintStringResult("unpack", -1, error.c_str());
	}
	return PrintStringResult("unpack", 0, summary.c_str());
}

void PrintUsage()
{
	std::cout << "e-packager commands:" << std::endl;
	std::cout << "  e-packager                           # pack current project to .\\pack\\<info.json sourceFileName>" << std::endl;
	std::cout << "  e-packager <input.e>                 # unpack .e to a same-named directory beside it (drag-and-drop)" << std::endl;
	std::cout << "  e-packager unpack <input.e> <output-dir>" << std::endl;
	std::cout << "  e-packager pack <input-dir> <output.e>" << std::endl;
	std::cout << "  e-packager compare-bundle <input.e> <input-dir>" << std::endl;
	std::cout << "  e-packager roundtrip <input.e> <work-dir> <output.e>" << std::endl;
	std::cout << "  e-packager verify-roundtrip <input.e> <work-dir> <output.e>" << std::endl;
}

}  // namespace

int main(int argc, char* argv[])
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

	// Drag-and-drop: a single .e file path passed directly
	if (argc == 2) {
		std::filesystem::path inputPath(command);
		std::string ext = inputPath.extension().string();
		std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
		if (ext == ".e") {
			return RunDragDropUnpack(argv[1]);
		}
	}

	PrintUsage();
	return EXIT_FAILURE;
}
