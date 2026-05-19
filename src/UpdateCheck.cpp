#include "UpdateCheck.h"

#include <Windows.h>
#include <winhttp.h>

#include <algorithm>
#include <cstdint>
#include <string>
#include <vector>

#include "..\thirdparty\json.hpp"

#pragma comment(lib, "Winhttp.lib")

namespace update_check {

namespace {

using json = nlohmann::json;

// 去掉前导 'v'/'V' 及预发布后缀（例如 "v1.2.3-beta.1" -> "1.2.3"）。
std::string NormalizeVersion(const std::string& s)
{
	std::string str = s;
	if (!str.empty() && (str[0] == 'v' || str[0] == 'V')) {
		str = str.substr(1);
	}
	const auto dash = str.find('-');
	if (dash != std::string::npos) {
		str = str.substr(0, dash);
	}
	return str;
}

bool ParseSemVer(const std::string& s, int& major, int& minor, int& patch)
{
	major = minor = patch = 0;
	return sscanf_s(s.c_str(), "%d.%d.%d", &major, &minor, &patch) >= 1;
}

bool HttpGetLatestReleaseJson(std::string& outBody, std::string* outError)
{
	outBody.clear();
	if (outError != nullptr) {
		outError->clear();
	}

	HINTERNET hSession = WinHttpOpen(
		L"e-packager-update-check/1.0",
		WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
		WINHTTP_NO_PROXY_NAME,
		WINHTTP_NO_PROXY_BYPASS,
		0);
	if (!hSession) {
		if (outError != nullptr) {
			*outError = "winhttp_open_failed";
		}
		return false;
	}

	HINTERNET hConnect = WinHttpConnect(
		hSession, L"api.github.com", INTERNET_DEFAULT_HTTPS_PORT, 0);
	if (!hConnect) {
		if (outError != nullptr) {
			*outError = "winhttp_connect_failed";
		}
		WinHttpCloseHandle(hSession);
		return false;
	}

	HINTERNET hRequest = WinHttpOpenRequest(
		hConnect,
		L"GET",
		L"/repos/aiqinxuancai/e-packager/releases/latest",
		nullptr,
		WINHTTP_NO_REFERER,
		WINHTTP_DEFAULT_ACCEPT_TYPES,
		WINHTTP_FLAG_SECURE);
	if (!hRequest) {
		if (outError != nullptr) {
			*outError = "winhttp_open_request_failed";
		}
		WinHttpCloseHandle(hConnect);
		WinHttpCloseHandle(hSession);
		return false;
	}

	// 域名解析 / 连接 / 发送 / 接收超时，单位毫秒。
	WinHttpSetTimeouts(hRequest, 5000, 5000, 5000, 10000);

	const wchar_t* headers = L"Accept: application/vnd.github+json\r\nUser-Agent: e-packager-update-check/1.0\r\n";
	bool ok = false;
	if (WinHttpSendRequest(
			hRequest,
			headers,
			static_cast<DWORD>(-1L),
			WINHTTP_NO_REQUEST_DATA,
			0,
			0,
			0) &&
		WinHttpReceiveResponse(hRequest, nullptr)) {
		DWORD statusCode = 0;
		DWORD statusSize = sizeof(statusCode);
		WinHttpQueryHeaders(
			hRequest,
			WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
			WINHTTP_HEADER_NAME_BY_INDEX,
			&statusCode,
			&statusSize,
			WINHTTP_NO_HEADER_INDEX);
		if (statusCode >= 200 && statusCode < 300) {
			DWORD available = 0;
			while (WinHttpQueryDataAvailable(hRequest, &available) && available > 0) {
				std::vector<char> buf(static_cast<size_t>(available) + 1, '\0');
				DWORD read = 0;
				if (!WinHttpReadData(hRequest, buf.data(), available, &read)) {
					break;
				}
				outBody.append(buf.data(), read);
			}
			ok = !outBody.empty();
		}
		else if (outError != nullptr) {
			*outError = "github_api_status_" + std::to_string(statusCode);
		}
	}
	else if (outError != nullptr) {
		*outError = "github_api_request_failed";
	}

	WinHttpCloseHandle(hRequest);
	WinHttpCloseHandle(hConnect);
	WinHttpCloseHandle(hSession);
	if (!ok && outError != nullptr && outError->empty()) {
		*outError = "github_api_empty_response";
	}
	return ok;
}

}  // namespace

bool IsPreRelease(const std::string& version)
{
	std::string lower = version;
	std::transform(lower.begin(), lower.end(), lower.begin(), ::tolower);
	return lower.find("alpha") != std::string::npos ||
		lower.find("beta") != std::string::npos ||
		lower.find("pre") != std::string::npos ||
		lower.find("rc") != std::string::npos;
}

bool IsNewer(const std::string& latestTag, const std::string& currentVersion)
{
	int lMaj = 0, lMin = 0, lPatch = 0;
	int cMaj = 0, cMin = 0, cPatch = 0;
	if (!ParseSemVer(NormalizeVersion(latestTag), lMaj, lMin, lPatch)) return false;
	if (!ParseSemVer(NormalizeVersion(currentVersion), cMaj, cMin, cPatch)) return false;
	if (lMaj != cMaj) return lMaj > cMaj;
	if (lMin != cMin) return lMin > cMin;
	return lPatch > cPatch;
}

bool FetchLatestRelease(LatestRelease& outRelease, std::string* outError)
{
	outRelease = {};
	if (outError != nullptr) {
		outError->clear();
	}

	std::string body;
	if (!HttpGetLatestReleaseJson(body, outError)) {
		return false;
	}

	try {
		const auto j = json::parse(body);
		if (j.contains("tag_name") && j["tag_name"].is_string()) {
			outRelease.tagName = j["tag_name"].get<std::string>();
		}
		outRelease.prerelease = j.value("prerelease", false);
		if (const auto it = j.find("assets"); it != j.end() && it->is_array()) {
			for (const auto& item : *it) {
				ReleaseAsset asset;
				asset.name = item.value("name", std::string());
				asset.browserDownloadUrl = item.value("browser_download_url", std::string());
				asset.size = item.value("size", static_cast<std::uint64_t>(0));
				if (!asset.name.empty() && !asset.browserDownloadUrl.empty()) {
					outRelease.assets.push_back(std::move(asset));
				}
			}
		}
	}
	catch (const std::exception& ex) {
		if (outError != nullptr) {
			*outError = std::string("github_api_parse_failed: ") + ex.what();
		}
		return false;
	}

	if (outRelease.tagName.empty()) {
		if (outError != nullptr) {
			*outError = "github_api_missing_tag";
		}
		return false;
	}
	return true;
}

std::string FetchLatestTag()
{
	LatestRelease release;
	if (!FetchLatestRelease(release, nullptr)) {
		return std::string();
	}
	return release.tagName;
}

}  // namespace update_check
