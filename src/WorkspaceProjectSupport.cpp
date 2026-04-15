#include "WorkspaceProjectSupport.h"

#include <Windows.h>
#include <Wincrypt.h>

#include <array>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <optional>
#include <sstream>
#include <system_error>
#include <vector>

#include "..\thirdparty\json.hpp"

#pragma comment(lib, "Advapi32.lib")

namespace workspace_support {

namespace {

using json = nlohmann::json;

struct SourceFileInfo {
	std::string fileName;
	std::string fullPath;
	std::string modifiedTimeUtc;
	std::uint64_t fileSize = 0;
	std::string md5;
};

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

std::wstring Utf8ToWide(const std::string& text)
{
	if (text.empty()) {
		return std::wstring();
	}

	const int wideLen = MultiByteToWideChar(
		CP_UTF8,
		0,
		text.data(),
		static_cast<int>(text.size()),
		nullptr,
		0);
	if (wideLen <= 0) {
		return std::wstring();
	}

	std::wstring wide(static_cast<size_t>(wideLen), L'\0');
	if (MultiByteToWideChar(
			CP_UTF8,
			0,
			text.data(),
			static_cast<int>(text.size()),
			wide.data(),
			wideLen) <= 0) {
		return std::wstring();
	}
	return wide;
}

std::string PathToUtf8(const std::filesystem::path& path)
{
	return WideToUtf8(path.wstring());
}

std::string NormalizeCrLf(const std::string& text)
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

bool WriteUtf8TextFileBom(const std::filesystem::path& path, const std::string& utf8Text)
{
	std::error_code ec;
	if (path.has_parent_path()) {
		std::filesystem::create_directories(path.parent_path(), ec);
		if (ec) {
			return false;
		}
	}

	std::ofstream out(path, std::ios::binary | std::ios::trunc);
	if (!out.is_open()) {
		return false;
	}

	static constexpr unsigned char kBom[] = {0xEF, 0xBB, 0xBF};
	out.write(reinterpret_cast<const char*>(kBom), sizeof(kBom));
	const std::string normalized = NormalizeCrLf(utf8Text);
	if (!normalized.empty()) {
		out.write(normalized.data(), static_cast<std::streamsize>(normalized.size()));
	}
	return out.good();
}

bool ReadUtf8TextFile(const std::filesystem::path& path, std::string& outText)
{
	outText.clear();
	std::ifstream in(path, std::ios::binary);
	if (!in.is_open()) {
		return false;
	}

	std::string bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
	if (bytes.size() >= 3 &&
		static_cast<unsigned char>(bytes[0]) == 0xEF &&
		static_cast<unsigned char>(bytes[1]) == 0xBB &&
		static_cast<unsigned char>(bytes[2]) == 0xBF) {
		bytes.erase(0, 3);
	}
	outText = std::move(bytes);
	return true;
}

std::string FormatFileTimeUtc(const FILETIME& fileTime)
{
	SYSTEMTIME systemTime = {};
	if (!FileTimeToSystemTime(&fileTime, &systemTime)) {
		return std::string();
	}

	char buffer[64] = {};
	std::snprintf(
		buffer,
		sizeof(buffer),
		"%04u-%02u-%02uT%02u:%02u:%02uZ",
		static_cast<unsigned>(systemTime.wYear),
		static_cast<unsigned>(systemTime.wMonth),
		static_cast<unsigned>(systemTime.wDay),
		static_cast<unsigned>(systemTime.wHour),
		static_cast<unsigned>(systemTime.wMinute),
		static_cast<unsigned>(systemTime.wSecond));
	return buffer;
}

bool QuerySourceFileInfo(const std::filesystem::path& inputFile, SourceFileInfo& outInfo, std::string& outError)
{
	outInfo = {};
	outError.clear();

	const std::filesystem::path absolutePath = std::filesystem::absolute(inputFile);
	HANDLE file = CreateFileW(
		absolutePath.c_str(),
		GENERIC_READ,
		FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
		nullptr,
		OPEN_EXISTING,
		FILE_ATTRIBUTE_NORMAL,
		nullptr);
	if (file == INVALID_HANDLE_VALUE) {
		outError = "open_source_file_failed: " + PathToUtf8(absolutePath);
		return false;
	}

	bool ok = false;
	HCRYPTPROV provider = 0;
	HCRYPTHASH hash = 0;
	do {
		LARGE_INTEGER sizeValue = {};
		if (!GetFileSizeEx(file, &sizeValue) || sizeValue.QuadPart < 0) {
			outError = "query_source_size_failed: " + PathToUtf8(absolutePath);
			break;
		}

		FILETIME creationTime = {};
		FILETIME accessTime = {};
		FILETIME writeTime = {};
		if (!GetFileTime(file, &creationTime, &accessTime, &writeTime)) {
			outError = "query_source_time_failed: " + PathToUtf8(absolutePath);
			break;
		}

		if (!CryptAcquireContextW(&provider, nullptr, nullptr, PROV_RSA_FULL, CRYPT_VERIFYCONTEXT) ||
			!CryptCreateHash(provider, CALG_MD5, 0, 0, &hash)) {
			outError = "create_md5_failed: " + PathToUtf8(absolutePath);
			break;
		}

		std::array<BYTE, 8192> buffer = {};
		DWORD readBytes = 0;
		while (ReadFile(file, buffer.data(), static_cast<DWORD>(buffer.size()), &readBytes, nullptr) && readBytes > 0) {
			if (!CryptHashData(hash, buffer.data(), readBytes, 0)) {
				outError = "hash_source_file_failed: " + PathToUtf8(absolutePath);
				break;
			}
		}
		if (!outError.empty()) {
			break;
		}

		BYTE digest[16] = {};
		DWORD digestSize = sizeof(digest);
		if (CryptGetHashParam(hash, HP_HASHVAL, digest, &digestSize, 0) == FALSE) {
			outError = "read_md5_failed: " + PathToUtf8(absolutePath);
			break;
		}

		static constexpr char kHex[] = "0123456789abcdef";
		outInfo.md5.reserve(digestSize * 2);
		for (DWORD index = 0; index < digestSize; ++index) {
			outInfo.md5.push_back(kHex[(digest[index] >> 4) & 0x0F]);
			outInfo.md5.push_back(kHex[digest[index] & 0x0F]);
		}

		outInfo.fileName = PathToUtf8(absolutePath.filename());
		outInfo.fullPath = PathToUtf8(absolutePath);
		outInfo.modifiedTimeUtc = FormatFileTimeUtc(writeTime);
		outInfo.fileSize = static_cast<std::uint64_t>(sizeValue.QuadPart);
		ok = true;
	}
	while (false);

	if (hash != 0) {
		CryptDestroyHash(hash);
	}
	if (provider != 0) {
		CryptReleaseContext(provider, 0);
	}
	CloseHandle(file);
	return ok;
}

std::filesystem::path GetCurrentExecutablePath()
{
	std::wstring buffer;
	buffer.resize(static_cast<size_t>(MAX_PATH));

	for (;;) {
		const DWORD written = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
		if (written == 0) {
			return std::filesystem::path();
		}
		if (written < buffer.size() - 1) {
			buffer.resize(written);
			return std::filesystem::path(buffer);
		}
		buffer.resize(buffer.size() * 2);
	}
}

json BuildInfoJson(const SourceFileInfo& info)
{
	json infoJson;
	infoJson["version"] = 1;
	infoJson["sourceFileName"] = info.fileName;
	infoJson["sourcePath"] = info.fullPath;
	infoJson["sourceModifiedTimeUtc"] = info.modifiedTimeUtc;
	infoJson["sourceSize"] = info.fileSize;
	infoJson["sourceMd5"] = info.md5;
	infoJson["toolUrl"] = "https://github.com/aiqinxuancai/e-packager";
	return infoJson;
}

std::string BuildAgentsMarkdown(const SourceFileInfo& info)
{
	std::wostringstream stream;
	stream
		<< L"# AGENTS.md\r\n"
		<< L"\r\n"
		<< L"## 项目说明\r\n"
		<< L"\r\n"
		<< L"当前目录是由 `" << Utf8ToWide(info.fileName) << L"` 解包得到的易语言目录工程。\r\n"
		<< L"外部编辑器应直接修改本目录内容，再通过 `tool\\\\e-packager.exe` 回包生成 `.e` 文件。\r\n"
		<< L"\r\n"
		<< L"## 目录结构\r\n"
		<< L"\r\n"
		<< L"- `src/`：源码目录。普通程序集、类、窗口程序集使用 `.txt` 保存；窗口界面定义使用同名 `.xml` 保存。\r\n"
		<< L"- `src/.数据类型.txt`：数据类型定义。\r\n"
		<< L"- `src/.DLL声明.txt`：DLL 声明。\r\n"
		<< L"- `src/.常量.txt`：常量定义。\r\n"
		<< L"- `src/.全局变量.txt`：全局变量定义。\r\n"
		<< L"- `project/`：封包所需元数据与原生快照，请勿随意删除。\r\n"
		<< L"- `image/`：图片资源及 `list.json`。\r\n"
		<< L"- `audio/`：音频资源及 `list.json`。\r\n"
		<< L"- `tool/`：当前目录自带的 `e-packager.exe`。\r\n"
		<< L"- `info.json`：记录本目录来源 `.e` 的文件名、路径、修改时间、尺寸、MD5。\r\n"
		<< L"\r\n"
		<< L"## 回包方法\r\n"
		<< L"\r\n"
		<< L"在项目根目录执行以下任一方式：\r\n"
		<< L"\r\n"
		<< L"- 默认方式：`tool\\\\e-packager.exe`\r\n"
		<< L"  默认输出到 `pack/` 目录，文件名优先使用 `info.json` 中记录的原始文件名。\r\n"
		<< L"- 显式方式：`tool\\\\e-packager.exe pack . .\\\\pack\\\\" << Utf8ToWide(info.fileName) << L"`\r\n"
		<< L"\r\n"
		<< L"回包后的 `.e` 可直接在易语言 IDE 中打开并编译。\r\n"
		<< L"\r\n"
		<< L"## 错误定位\r\n"
		<< L"\r\n"
		<< L"如果回包时检测到语法错误，`e-packager.exe` 会输出类似以下信息：\r\n"
		<< L"\r\n"
		<< L"- `source_syntax_error: file=src/某页面.txt, line=行号, ...`\r\n"
		<< L"- `xml_syntax_error: file=src/某窗口.xml, line=行号, ...`\r\n"
		<< L"\r\n"
		<< L"请按报错中的文件与行号修正后再重新回包。\r\n"
		<< L"\r\n"
		<< L"## 工具来源\r\n"
		<< L"\r\n"
		<< L"本目录由 [e-packager](https://github.com/aiqinxuancai/e-packager) 解包生成。\r\n";
	return WideToUtf8(stream.str());
}

bool HasProjectMarkers(const std::filesystem::path& root)
{
	std::error_code ec;
	return std::filesystem::exists(root / "info.json", ec) &&
		std::filesystem::exists(root / "src", ec) &&
		std::filesystem::exists(root / "project" / "模块.json", ec);
}

bool ReadInfoJson(const std::filesystem::path& root, json& outInfo, std::string& outError)
{
	outError.clear();
	std::string text;
	if (!ReadUtf8TextFile(root / "info.json", text)) {
		outError = "read_info_json_failed: " + PathToUtf8(root / "info.json");
		return false;
	}

	try {
		outInfo = json::parse(text);
	}
	catch (const std::exception& ex) {
		outError = std::string("parse_info_json_failed: ") + ex.what();
		return false;
	}
	return true;
}

std::string ResolveDefaultOutputFileName(const json& infoJson, const std::filesystem::path& projectRoot)
{
	std::string fileName = infoJson.value("sourceFileName", std::string());
	if (fileName.empty()) {
		const std::string sourcePath = infoJson.value("sourcePath", std::string());
		if (!sourcePath.empty()) {
			fileName = PathToUtf8(std::filesystem::path(Utf8ToWide(sourcePath)).filename());
		}
	}
	if (fileName.empty()) {
		fileName = PathToUtf8(projectRoot.filename()) + ".e";
	}

	std::filesystem::path filePath = std::filesystem::path(Utf8ToWide(fileName)).filename();
	if (filePath.extension().empty()) {
		filePath += L".e";
	}

	const std::string resolved = PathToUtf8(filePath);
	return resolved.empty() ? std::string("project.e") : resolved;
}

bool CopyExecutableToToolDirectory(const std::filesystem::path& outputDir, std::string& outError)
{
	const std::filesystem::path executablePath = GetCurrentExecutablePath();
	if (executablePath.empty()) {
		outError = "query_current_executable_failed";
		return false;
	}

	const std::filesystem::path toolDir = outputDir / "tool";
	std::error_code ec;
	std::filesystem::create_directories(toolDir, ec);
	if (ec) {
		outError = "create_tool_dir_failed: " + PathToUtf8(toolDir);
		return false;
	}

	const std::filesystem::path destination = toolDir / executablePath.filename();
	std::filesystem::copy_file(executablePath, destination, std::filesystem::copy_options::overwrite_existing, ec);
	if (ec) {
		outError = "copy_tool_exe_failed: " + PathToUtf8(destination);
		return false;
	}
	return true;
}

}  // namespace

static constexpr int kSupportedInfoVersion = 1;

bool WriteWorkspaceFiles(
	const std::filesystem::path& inputFile,
	const std::filesystem::path& outputDir,
	std::string& outError)
{
	outError.clear();

	SourceFileInfo info;
	if (!QuerySourceFileInfo(inputFile, info, outError)) {
		return false;
	}

	if (!WriteUtf8TextFileBom(outputDir / "info.json", BuildInfoJson(info).dump(2))) {
		outError = "write_info_json_failed: " + PathToUtf8(outputDir / "info.json");
		return false;
	}

	if (!CopyExecutableToToolDirectory(outputDir, outError)) {
		return false;
	}

	if (!WriteUtf8TextFileBom(outputDir / "AGENTS.md", BuildAgentsMarkdown(info))) {
		outError = "write_agents_md_failed: " + PathToUtf8(outputDir / "AGENTS.md");
		return false;
	}

	return true;
}

bool ResolveDefaultPackOutput(
	const std::filesystem::path& currentDir,
	std::filesystem::path& outProjectRoot,
	std::filesystem::path& outOutputFile,
	std::string& outError)
{
	outProjectRoot.clear();
	outOutputFile.clear();
	outError.clear();

	std::vector<std::filesystem::path> candidates;
	candidates.push_back(currentDir);
	if (currentDir.filename() == "tool") {
		candidates.push_back(currentDir.parent_path());
	}

	const std::filesystem::path executablePath = GetCurrentExecutablePath();
	if (!executablePath.empty()) {
		const std::filesystem::path executableDir = executablePath.parent_path();
		candidates.push_back(executableDir);
		if (executableDir.filename() == "tool") {
			candidates.push_back(executableDir.parent_path());
		}
	}

	for (const auto& candidate : candidates) {
		if (candidate.empty()) {
			continue;
		}
		if (HasProjectMarkers(candidate)) {
			outProjectRoot = std::filesystem::absolute(candidate);
			break;
		}
	}

	if (outProjectRoot.empty()) {
		outError = "default_pack_project_root_not_found";
		return false;
	}

	json infoJson;
	if (!ReadInfoJson(outProjectRoot, infoJson, outError)) {
		return false;
	}

	const std::string outputFileName = ResolveDefaultOutputFileName(infoJson, outProjectRoot);
	std::error_code ec;
	std::filesystem::create_directories(outProjectRoot / "pack", ec);
	if (ec) {
		outError = "create_pack_dir_failed: " + PathToUtf8(outProjectRoot / "pack");
		return false;
	}

	outOutputFile = outProjectRoot / "pack" / std::filesystem::path(Utf8ToWide(outputFileName));
	return true;
}

bool ValidateInfoJsonVersion(const std::filesystem::path& projectRoot, std::string& outError)
{
	outError.clear();

	json infoJson;
	if (!ReadInfoJson(projectRoot, infoJson, outError)) {
		return false;
	}

	if (!infoJson.contains("version")) {
		outError = "info_json_missing_version: " + PathToUtf8(projectRoot / "info.json");
		return false;
	}

	const int version = infoJson.value("version", -1);
	if (version != kSupportedInfoVersion) {
		outError =
			"info_json_version_unsupported: version=" + std::to_string(version) +
			", supported=" + std::to_string(kSupportedInfoVersion) +
			", file=" + PathToUtf8(projectRoot / "info.json");
		return false;
	}

	return true;
}

}  // namespace workspace_support
