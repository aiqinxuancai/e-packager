#include "UpdateCheck.h"

#include <Windows.h>
#include <winhttp.h>

#include <algorithm>
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

std::string FetchLatestTag()
{
	std::string result;

	HINTERNET hSession = WinHttpOpen(
		L"e-packager-update-check/1.0",
		WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
		WINHTTP_NO_PROXY_NAME,
		WINHTTP_NO_PROXY_BYPASS,
		0);
	if (!hSession) return result;

	HINTERNET hConnect = WinHttpConnect(
		hSession, L"api.github.com", INTERNET_DEFAULT_HTTPS_PORT, 0);
	if (!hConnect) {
		WinHttpCloseHandle(hSession);
		return result;
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
		WinHttpCloseHandle(hConnect);
		WinHttpCloseHandle(hSession);
		return result;
	}

	// 域名解析 / 连接 / 发送 / 接收 超时，单位毫秒
	WinHttpSetTimeouts(hRequest, 3000, 3000, 3000, 3000);

	if (WinHttpSendRequest(hRequest, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
			WINHTTP_NO_REQUEST_DATA, 0, 0, 0) &&
		WinHttpReceiveResponse(hRequest, nullptr))
	{
		std::string body;
		DWORD available = 0;
		while (WinHttpQueryDataAvailable(hRequest, &available) && available > 0) {
			std::vector<char> buf(static_cast<size_t>(available) + 1, '\0');
			DWORD read = 0;
			if (!WinHttpReadData(hRequest, buf.data(), available, &read)) break;
			body.append(buf.data(), read);
		}
		try {
			const auto j = json::parse(body);
			if (j.contains("tag_name") && j["tag_name"].is_string()) {
				result = j["tag_name"].get<std::string>();
			}
		}
		catch (...) {}
	}

	WinHttpCloseHandle(hRequest);
	WinHttpCloseHandle(hConnect);
	WinHttpCloseHandle(hSession);
	return result;
}

}  // namespace update_check
