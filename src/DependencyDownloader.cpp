#include "DependencyDownloader.h"

#include "PathHelper.h"
#include "../thirdparty/json.hpp"

#include <Windows.h>
#include <winhttp.h>

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#pragma comment(lib, "Winhttp.lib")

namespace dependency_download {
namespace {

using json = nlohmann::json;

bool IsRegularFile(const std::filesystem::path& path)
{
	std::error_code error;
	return std::filesystem::is_regular_file(path, error);
}

std::string LowerAscii(std::string value)
{
	std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch) {
		return static_cast<char>(std::tolower(ch));
	});
	return value;
}

bool IsCoreDependency(const std::string& dependencyFileName)
{
	std::filesystem::path path = Utf8PathToPath(dependencyFileName);
	std::string stem = LowerAscii(path.stem().string());
	return stem == "krnln";
}

std::wstring QuoteArgument(const std::wstring& value)
{
	std::wstring result = L"\"";
	for (const wchar_t ch : value) {
		if (ch == L'\"') result += L"\\\"";
		else result.push_back(ch);
	}
	result += L"\"";
	return result;
}

std::wstring QuotePowerShellLiteral(const std::wstring& value)
{
	std::wstring result = L"'";
	for (const wchar_t ch : value) {
		if (ch == L'\'') result += L"''";
		else result.push_back(ch);
	}
	result += L"'";
	return result;
}

bool RunPowerShell(
	const std::wstring& command,
	const std::filesystem::path& workingDirectory,
	std::string& outError)
{
	outError.clear();
	std::filesystem::path executable;
	wchar_t systemDirectory[MAX_PATH]{};
	const UINT length = GetSystemDirectoryW(systemDirectory, std::size(systemDirectory));
	if (length == 0 || length >= std::size(systemDirectory)) {
		outError = "get_system_directory_failed:" + std::to_string(GetLastError());
		return false;
	}
	executable = std::filesystem::path(systemDirectory) / L"WindowsPowerShell" / L"v1.0" / L"powershell.exe";
	if (!std::filesystem::is_regular_file(executable)) {
		outError = "powershell_not_found:" + PathToUtf8(executable);
		return false;
	}

	const std::wstring commandLine = QuoteArgument(executable) +
		L" -NoLogo -NoProfile -NonInteractive -ExecutionPolicy Bypass -Command " +
		QuoteArgument(command);
	std::vector<wchar_t> mutableCommand(commandLine.begin(), commandLine.end());
	mutableCommand.push_back(L'\0');
	STARTUPINFOW startup{};
	startup.cb = sizeof(startup);
	PROCESS_INFORMATION process{};
	if (!CreateProcessW(
			executable.c_str(), mutableCommand.data(), nullptr, nullptr, FALSE,
			CREATE_NO_WINDOW, nullptr, workingDirectory.c_str(), &startup, &process)) {
		outError = "start_powershell_failed:" + std::to_string(GetLastError());
		return false;
	}
	WaitForSingleObject(process.hProcess, INFINITE);
	DWORD exitCode = 1;
	GetExitCodeProcess(process.hProcess, &exitCode);
	CloseHandle(process.hThread);
	CloseHandle(process.hProcess);
	if (exitCode != 0) {
		outError = "powershell_failed:exit=" + std::to_string(exitCode);
		return false;
	}
	return true;
}

bool DownloadUrl(
	const std::string& url,
	const std::filesystem::path& outputPath,
	std::string& outError)
{
	outError.clear();
	const int wideLength = MultiByteToWideChar(
		CP_UTF8, MB_ERR_INVALID_CHARS, url.data(), static_cast<int>(url.size()), nullptr, 0);
	if (wideLength <= 0) {
		outError = "dependency_url_is_not_utf8";
		return false;
	}
	std::wstring wideUrl(static_cast<std::size_t>(wideLength), L'\0');
	if (MultiByteToWideChar(
		CP_UTF8, MB_ERR_INVALID_CHARS, url.data(), static_cast<int>(url.size()),
		wideUrl.data(), wideLength) != wideLength) {
		outError = "dependency_url_conversion_failed";
		return false;
	}
	HINTERNET session = WinHttpOpen(
		L"e-packager-dependency/1.0", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
		WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
	if (!session) {
		outError = "winhttp_open_failed:" + std::to_string(GetLastError());
		return false;
	}
	URL_COMPONENTS components{};
	components.dwStructSize = sizeof(components);
	components.dwSchemeLength = static_cast<DWORD>(-1);
	components.dwHostNameLength = static_cast<DWORD>(-1);
	components.dwUrlPathLength = static_cast<DWORD>(-1);
	components.dwExtraInfoLength = static_cast<DWORD>(-1);
	if (!WinHttpCrackUrl(wideUrl.c_str(), 0, 0, &components)) {
		outError = "winhttp_crack_url_failed:" + std::to_string(GetLastError());
		WinHttpCloseHandle(session);
		return false;
	}
	const std::wstring host(components.lpszHostName, components.dwHostNameLength);
	std::wstring path(components.lpszUrlPath, components.dwUrlPathLength);
	if (components.dwExtraInfoLength > 0) {
		path.append(components.lpszExtraInfo, components.dwExtraInfoLength);
	}
	const bool secure = components.nScheme == INTERNET_SCHEME_HTTPS;
	HINTERNET connection = WinHttpConnect(session, host.c_str(), components.nPort, 0);
	if (!connection) {
		outError = "winhttp_connect_failed:" + std::to_string(GetLastError());
		WinHttpCloseHandle(session);
		return false;
	}
	HINTERNET request = WinHttpOpenRequest(
		connection, L"GET", path.c_str(), nullptr, WINHTTP_NO_REFERER,
		WINHTTP_DEFAULT_ACCEPT_TYPES, secure ? WINHTTP_FLAG_SECURE : 0);
	if (!request) {
		outError = "winhttp_open_request_failed:" + std::to_string(GetLastError());
		WinHttpCloseHandle(connection);
		WinHttpCloseHandle(session);
		return false;
	}
	WinHttpSetTimeouts(request, 10000, 10000, 10000, 120000);
	const wchar_t* headers = L"User-Agent: e-packager-dependency/1.0\r\n";
	bool ok = false;
	HANDLE file = CreateFileW(
		outputPath.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
		FILE_ATTRIBUTE_TEMPORARY, nullptr);
	if (file == INVALID_HANDLE_VALUE) {
		outError = "create_dependency_archive_failed:" + PathToUtf8(outputPath);
	}
	else if (WinHttpSendRequest(request, headers, static_cast<DWORD>(-1L),
			WINHTTP_NO_REQUEST_DATA, 0, 0, 0) && WinHttpReceiveResponse(request, nullptr)) {
		DWORD statusCode = 0;
		DWORD statusSize = sizeof(statusCode);
		WinHttpQueryHeaders(request,
			WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
			WINHTTP_HEADER_NAME_BY_INDEX, &statusCode, &statusSize,
			WINHTTP_NO_HEADER_INDEX);
		if (statusCode >= 200 && statusCode < 300) {
			ok = true;
			for (;;) {
				DWORD available = 0;
				if (!WinHttpQueryDataAvailable(request, &available)) {
					ok = false;
					break;
				}
				if (available == 0) break;
				std::vector<std::uint8_t> buffer(available);
				DWORD read = 0;
				if (!WinHttpReadData(request, buffer.data(), available, &read)) {
					ok = false;
					break;
				}
				DWORD written = 0;
				if (!WriteFile(file, buffer.data(), read, &written, nullptr) || written != read) {
					ok = false;
					break;
				}
			}
		}
		else {
			outError = "dependency_download_http_status=" + std::to_string(statusCode);
		}
	}
	else {
		outError = "dependency_download_request_failed:" + std::to_string(GetLastError());
	}
	CloseHandle(file);
	WinHttpCloseHandle(request);
	WinHttpCloseHandle(connection);
	WinHttpCloseHandle(session);
	if (!ok) {
		if (outError.empty()) outError = "dependency_download_failed";
		std::error_code ignored;
		std::filesystem::remove(outputPath, ignored);
	}
	return ok;
}

std::filesystem::path DependencyCacheRoot()
{
	wchar_t buffer[MAX_PATH * 4]{};
	DWORD length = GetEnvironmentVariableW(L"LOCALAPPDATA", buffer, std::size(buffer));
	if (length > 0 && length < std::size(buffer)) {
		return std::filesystem::path(buffer) / L"e-packager" / L"dependencies" / L"BlackMoonModernCore";
	}
	std::error_code ec;
	return std::filesystem::temp_directory_path(ec) / L"e-packager" / L"dependencies" / L"BlackMoonModernCore";
}

bool FindAdapterRoot(
	const std::filesystem::path& root,
	const std::string& architecture,
	std::filesystem::path& outRoot)
{
	outRoot.clear();
	std::error_code ec;
	for (const auto& entry : std::filesystem::recursive_directory_iterator(root, ec)) {
		if (ec) break;
		if (!entry.is_regular_file(ec) || entry.path().filename() != L"krnln_adapter.json") continue;
		std::ifstream input(entry.path(), std::ios::binary);
		if (!input) continue;
		try {
			const json manifest = json::parse(input);
			if (manifest.value("formatVersion", 0) == 1 &&
				manifest.value("architecture", std::string()) == architecture &&
				manifest.value("abi", std::string()) == "ecompiler-fne-execute-v1") {
				outRoot = entry.path().parent_path();
				return true;
			}
		}
		catch (...) {
		}
	}
	return false;
}

bool DownloadBlackMoonCore(
	const ecompiler::TargetArchitecture architecture,
	std::filesystem::path& outSearchRoot,
	std::string& outError)
{
	outSearchRoot.clear();
	outError.clear();
	const std::string architectureName = architecture == ecompiler::TargetArchitecture::X64 ? "x64" : "x86";
	const std::filesystem::path cacheRoot = DependencyCacheRoot();
	std::error_code ec;
	std::filesystem::create_directories(cacheRoot, ec);
	if (ec) {
		outError = "create_dependency_cache_failed:" + PathToUtf8(cacheRoot);
		return false;
	}

	if (FindAdapterRoot(cacheRoot, architectureName, outSearchRoot)) return true;

	const std::filesystem::path releaseInfo = cacheRoot / L"latest.json";
	if (!DownloadUrl(
		"https://api.github.com/repos/aiqinxuancai/BlackMoonModernCore/releases/latest",
		releaseInfo, outError)) {
		return false;
	}
	std::ifstream releaseInput(releaseInfo, std::ios::binary);
	if (!releaseInput) {
		outError = "open_dependency_release_metadata_failed";
		return false;
	}
	try {
		std::ostringstream buffer;
		buffer << releaseInput.rdbuf();
		const json release = json::parse(buffer.str());
		const std::string tag = release.value("tag_name", std::string());
		if (tag.empty()) {
			outError = "dependency_release_missing_tag";
			return false;
		}
		const std::string exactPrefix = "BlackMoonKernelStaticLib-" + tag + "-" + architectureName + ".zip";
		const std::string universalName = "BlackMoonKernelStaticLib-" + tag + ".zip";
		std::string assetUrl;
		std::string assetName;
		if (const auto assets = release.find("assets"); assets != release.end() && assets->is_array()) {
			for (const auto& asset : *assets) {
				const std::string name = asset.value("name", std::string());
				if (name == exactPrefix) {
					assetName = name;
					assetUrl = asset.value("browser_download_url", std::string());
					break;
				}
			}
			if (assetUrl.empty()) {
				for (const auto& asset : *assets) {
					const std::string name = asset.value("name", std::string());
					if (name == universalName) {
						assetName = name;
						assetUrl = asset.value("browser_download_url", std::string());
						break;
					}
				}
			}
		}
		if (assetUrl.empty()) {
			outError = "dependency_release_asset_not_found:" + architectureName;
			return false;
		}
		const std::filesystem::path versionRoot = cacheRoot / Utf8PathToPath(tag) / Utf8PathToPath(architectureName);
		if (FindAdapterRoot(versionRoot, architectureName, outSearchRoot)) return true;
		std::filesystem::create_directories(versionRoot, ec);
		if (ec) {
			outError = "create_dependency_version_cache_failed:" + PathToUtf8(versionRoot);
			return false;
		}
		const std::filesystem::path archivePath = versionRoot / Utf8PathToPath(assetName);
		if (!IsRegularFile(archivePath) && !DownloadUrl(assetUrl, archivePath, outError)) return false;
		const std::filesystem::path extracted = versionRoot / L"extracted";
		std::filesystem::create_directories(extracted, ec);
		if (ec) {
			outError = "create_dependency_extract_dir_failed:" + PathToUtf8(extracted);
			return false;
		}
		const std::wstring command = L"Expand-Archive -LiteralPath " + QuotePowerShellLiteral(archivePath.wstring()) +
			L" -DestinationPath " + QuotePowerShellLiteral(extracted.wstring()) + L" -Force";
		if (!FindAdapterRoot(extracted, architectureName, outSearchRoot) &&
			!RunPowerShell(command, extracted, outError)) return false;
		if (!FindAdapterRoot(extracted, architectureName, outSearchRoot)) {
			outError = "dependency_adapter_manifest_not_found:" + PathToUtf8(extracted);
			return false;
		}
		return true;
	}
	catch (const std::exception& ex) {
		outError = std::string("dependency_release_parse_failed:") + ex.what();
		return false;
	}
}

}  // namespace

bool EnsureDependency(
	const std::string& dependencyFileName,
	const ecompiler::TargetArchitecture architecture,
	std::filesystem::path& outSearchRoot,
	std::string& outError)
{
	if (IsCoreDependency(dependencyFileName)) {
		return DownloadBlackMoonCore(architecture, outSearchRoot, outError);
	}
	outSearchRoot.clear();
	outError = "no_download_provider_for_dependency:" + dependencyFileName;
	return false;
}

}  // namespace dependency_download
