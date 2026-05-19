#include "SelfUpdater.h"

#include <Windows.h>

#include <algorithm>
#include <cctype>
#include <cwctype>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "PathHelper.h"
#include "UpdateCheck.h"

namespace self_update {

namespace {

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

bool WriteUtf8TextFileBom(const std::filesystem::path& path, const std::string& text, std::string& outError)
{
	outError.clear();
	std::error_code ec;
	if (path.has_parent_path()) {
		std::filesystem::create_directories(path.parent_path(), ec);
		if (ec) {
			outError = "create_text_dir_failed: " + PathToUtf8(path.parent_path());
			return false;
		}
	}

	std::ofstream out(path, std::ios::binary | std::ios::trunc);
	if (!out.is_open()) {
		outError = "open_text_for_write_failed: " + PathToUtf8(path);
		return false;
	}

	static constexpr unsigned char kUtf8Bom[] = {0xEF, 0xBB, 0xBF};
	out.write(reinterpret_cast<const char*>(kUtf8Bom), sizeof(kUtf8Bom));
	const std::string normalized = NormalizeCrLfForWrite(text);
	out.write(normalized.data(), static_cast<std::streamsize>(normalized.size()));
	if (!out.good()) {
		outError = "write_text_failed: " + PathToUtf8(path);
		return false;
	}
	return true;
}

bool EndsWithAscii(const std::string& text, const std::string& suffix)
{
	return text.size() >= suffix.size() &&
		text.compare(text.size() - suffix.size(), suffix.size(), suffix) == 0;
}

bool ContainsAscii(const std::string& text, const std::string& needle)
{
	return text.find(needle) != std::string::npos;
}

bool ContainsWindowsMarker(const std::string& name, const bool isExe)
{
	return ContainsAscii(name, "windows") ||
		ContainsAscii(name, "win32") ||
		ContainsAscii(name, "win64") ||
		ContainsAscii(name, "-win-") ||
		ContainsAscii(name, "_win_") ||
		ContainsAscii(name, ".win.") ||
		ContainsAscii(name, "-win.") ||
		ContainsAscii(name, "_win.") ||
		(isExe && ContainsAscii(name, "e-packager"));
}

bool ContainsX64Marker(const std::string& name)
{
	return ContainsAscii(name, "x64") ||
		ContainsAscii(name, "amd64") ||
		ContainsAscii(name, "win64") ||
		ContainsAscii(name, "x86_64") ||
		ContainsAscii(name, "x86-64");
}

bool ContainsX86Marker(const std::string& name)
{
	const bool explicitX86 =
		ContainsAscii(name, "x86") &&
		!ContainsAscii(name, "x86_64") &&
		!ContainsAscii(name, "x86-64");
	return ContainsAscii(name, "win32") ||
		ContainsAscii(name, "i386") ||
		ContainsAscii(name, "ia32") ||
		explicitX86;
}

std::string CurrentUpdateArchName()
{
#if defined(_M_X64)
	return "x64";
#else
	return "win32";
#endif
}

bool IsDevelopmentVersion(const std::string& version)
{
	const std::string lower = ToLowerAsciiCopy(TrimAsciiCopy(version));
	return lower.empty() || lower == "dev" || lower == "debug";
}

struct SelectedUpdateAsset {
	update_check::ReleaseAsset asset;
	bool architectureMatched = false;
};

bool TrySelectUpdateAsset(
	const update_check::LatestRelease& release,
	SelectedUpdateAsset& outSelection,
	std::string& outError)
{
	outSelection = {};
	outError.clear();

	int bestScore = -100000;
	bool found = false;
	for (const auto& asset : release.assets) {
		const std::string name = ToLowerAsciiCopy(asset.name);
		const bool isArchive = EndsWithAscii(name, ".zip");
		const bool isExe = EndsWithAscii(name, ".exe");
		if (!isArchive && !isExe) {
			continue;
		}
		if (!ContainsWindowsMarker(name, isExe)) {
			continue;
		}

#if defined(_M_X64)
		const bool archMatched = ContainsX64Marker(name);
		const bool otherArch = ContainsX86Marker(name);
#else
		const bool archMatched = ContainsX86Marker(name);
		const bool otherArch = ContainsX64Marker(name);
#endif
		if (otherArch && !archMatched) {
			continue;
		}

		int score = 0;
		if (ContainsAscii(name, "e-packager")) {
			score += 40;
		}
		if (archMatched) {
			score += 100;
		}
		if (otherArch) {
			score -= 10;
		}
		if (isArchive) {
			score += 10;
		}
		if (isExe) {
			score += 5;
		}

		if (!found || score > bestScore) {
			found = true;
			bestScore = score;
			outSelection.asset = asset;
			outSelection.architectureMatched = archMatched;
		}
	}

	if (!found) {
		outError = "release_asset_not_found";
		return false;
	}
	return true;
}

std::wstring QuoteCommandLineArgument(const std::wstring& argument)
{
	if (argument.empty()) {
		return L"\"\"";
	}

	bool needsQuotes = false;
	for (const wchar_t ch : argument) {
		if (std::iswspace(ch) || ch == L'"') {
			needsQuotes = true;
			break;
		}
	}
	if (!needsQuotes) {
		return argument;
	}

	std::wstring result = L"\"";
	size_t backslashes = 0;
	for (const wchar_t ch : argument) {
		if (ch == L'\\') {
			++backslashes;
			continue;
		}
		if (ch == L'"') {
			result.append(backslashes * 2 + 1, L'\\');
			result.push_back(ch);
			backslashes = 0;
			continue;
		}
		result.append(backslashes, L'\\');
		backslashes = 0;
		result.push_back(ch);
	}
	result.append(backslashes * 2, L'\\');
	result.push_back(L'"');
	return result;
}

std::wstring AsciiToWide(const std::string& text)
{
	return std::wstring(text.begin(), text.end());
}

std::filesystem::path GetCurrentExecutablePath()
{
	std::wstring buffer(MAX_PATH, L'\0');
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

std::string BuildSelfUpdateScriptText()
{
	return R"powershell(
param(
  [int]$SourcePid,
  [string]$TargetPath,
  [string]$AssetUrl,
  [string]$AssetName,
  [string]$LogPath
)
$ErrorActionPreference = 'Stop'
function Write-UpdateLog([string]$Message) {
  try {
    $stamp = Get-Date -Format 'yyyy-MM-dd HH:mm:ss'
    Add-Content -LiteralPath $LogPath -Encoding UTF8 -Value "[$stamp] $Message" -ErrorAction Stop
  } catch {
  }
}
try {
  Write-UpdateLog "self-update started; target=$TargetPath; asset=$AssetName"
  $workDir = Join-Path ([IO.Path]::GetTempPath()) ('e-packager-self-update-work-' + [guid]::NewGuid().ToString('N'))
  New-Item -ItemType Directory -Force -Path $workDir | Out-Null
  $downloadPath = Join-Path $workDir $AssetName
  Write-UpdateLog "downloading $AssetUrl"
  Invoke-WebRequest -Uri $AssetUrl -OutFile $downloadPath -UseBasicParsing -Headers @{ 'User-Agent' = 'e-packager-self-update/1.0' }

  $lowerName = $AssetName.ToLowerInvariant()
  if ($lowerName.EndsWith('.zip')) {
    $extractDir = Join-Path $workDir 'extract'
    New-Item -ItemType Directory -Force -Path $extractDir | Out-Null
    Expand-Archive -LiteralPath $downloadPath -DestinationPath $extractDir -Force
    $candidate = Get-ChildItem -LiteralPath $extractDir -Recurse -File -Filter 'e-packager.exe' | Select-Object -First 1
    if ($null -eq $candidate) {
      throw 'e-packager.exe not found in release archive'
    }
    $newExe = $candidate.FullName
  } else {
    $newExe = $downloadPath
  }

  for ($i = 0; $i -lt 90; $i++) {
    try {
      $process = Get-Process -Id $SourcePid -ErrorAction Stop
      Start-Sleep -Seconds 1
    } catch {
      break
    }
  }
  try {
    Get-Process -Id $SourcePid -ErrorAction Stop | Out-Null
    throw "source process $SourcePid did not exit in time"
  } catch [Microsoft.PowerShell.Commands.ProcessCommandException] {
  }

  $backupPath = $TargetPath + '.old'
  Remove-Item -LiteralPath $backupPath -Force -ErrorAction SilentlyContinue
  for ($i = 0; $i -lt 30; $i++) {
    try {
      if (Test-Path -LiteralPath $TargetPath) {
        Move-Item -LiteralPath $TargetPath -Destination $backupPath -Force
      } elseif (-not (Test-Path -LiteralPath $backupPath)) {
        throw "target executable missing: $TargetPath"
      }
      Copy-Item -LiteralPath $newExe -Destination $TargetPath -Force
      Remove-Item -LiteralPath $backupPath -Force -ErrorAction SilentlyContinue
      Write-UpdateLog "self-update completed"
      exit 0
    } catch {
      if ($i -eq 29) {
        if ((Test-Path -LiteralPath $backupPath) -and -not (Test-Path -LiteralPath $TargetPath)) {
          Move-Item -LiteralPath $backupPath -Destination $TargetPath -Force -ErrorAction SilentlyContinue
        }
        throw
      }
      Start-Sleep -Seconds 1
    }
  }
} catch {
  Write-UpdateLog ("self-update failed: " + $_.Exception.Message)
  exit 1
}
)powershell";
}

bool LaunchSelfUpdateHelper(
	const update_check::ReleaseAsset& asset,
	const std::filesystem::path& targetPath,
	std::filesystem::path& outLogPath,
	std::string& outError)
{
	outError.clear();
	outLogPath.clear();

	std::error_code ec;
	const std::filesystem::path tempRoot =
		std::filesystem::temp_directory_path(ec) /
		("e-packager-self-update-" + std::to_string(GetCurrentProcessId()));
	if (ec) {
		outError = "resolve_temp_dir_failed";
		return false;
	}
	std::filesystem::create_directories(tempRoot, ec);
	if (ec) {
		outError = "create_self_update_dir_failed: " + PathToUtf8(tempRoot);
		return false;
	}

	const std::filesystem::path scriptPath = tempRoot / "apply-update.ps1";
	outLogPath = tempRoot / "update.log";
	if (!WriteUtf8TextFileBom(scriptPath, BuildSelfUpdateScriptText(), outError)) {
		return false;
	}

	std::wstring command = L"powershell.exe -NoProfile -ExecutionPolicy Bypass -WindowStyle Hidden -File ";
	command += QuoteCommandLineArgument(scriptPath.wstring());
	command += L" -SourcePid ";
	command += std::to_wstring(GetCurrentProcessId());
	command += L" -TargetPath ";
	command += QuoteCommandLineArgument(targetPath.wstring());
	command += L" -AssetUrl ";
	command += QuoteCommandLineArgument(AsciiToWide(asset.browserDownloadUrl));
	command += L" -AssetName ";
	command += QuoteCommandLineArgument(AsciiToWide(asset.name));
	command += L" -LogPath ";
	command += QuoteCommandLineArgument(outLogPath.wstring());

	STARTUPINFOW startupInfo{};
	startupInfo.cb = sizeof(startupInfo);
	PROCESS_INFORMATION processInfo{};
	std::vector<wchar_t> mutableCommand(command.begin(), command.end());
	mutableCommand.push_back(L'\0');
	if (!CreateProcessW(
			nullptr,
			mutableCommand.data(),
			nullptr,
			nullptr,
			FALSE,
			CREATE_NO_WINDOW,
			nullptr,
			nullptr,
			&startupInfo,
			&processInfo)) {
		outError = "launch_self_update_helper_failed: " + std::to_string(GetLastError());
		return false;
	}

	CloseHandle(processInfo.hThread);
	CloseHandle(processInfo.hProcess);
	return true;
}

}  // namespace

UpdateResult ScheduleSelfUpdate(const std::string& currentVersion, const bool force)
{
#if defined(_M_X64)
	(void)currentVersion;
	(void)force;
	return UpdateResult{
		true,
		"skipped_x64_release_not_published: GitHub Release only provides win32 package"};
#else
	std::string error;
	update_check::LatestRelease release;
	if (!update_check::FetchLatestRelease(release, &error)) {
		return UpdateResult{false, error};
	}

	if (!force &&
		!IsDevelopmentVersion(currentVersion) &&
		!update_check::IsPreRelease(currentVersion) &&
		!update_check::IsNewer(release.tagName, currentVersion)) {
		return UpdateResult{
			true,
			"already_latest current=" + currentVersion + ", latest=" + release.tagName};
	}

	SelectedUpdateAsset selection;
	if (!TrySelectUpdateAsset(release, selection, error)) {
		return UpdateResult{false, error};
	}

	const std::filesystem::path targetPath = GetCurrentExecutablePath();
	if (targetPath.empty()) {
		return UpdateResult{false, "resolve_current_executable_failed"};
	}

	std::filesystem::path logPath;
	if (!LaunchSelfUpdateHelper(selection.asset, targetPath, logPath, error)) {
		return UpdateResult{false, error};
	}

	std::string summary =
		"scheduled latest=" + release.tagName +
		", current=" + currentVersion +
		", arch=" + CurrentUpdateArchName() +
		", asset=" + selection.asset.name +
		", target=" + PathToUtf8(targetPath) +
		", log=" + PathToUtf8(logPath);
	if (!selection.architectureMatched) {
		summary += ", architecture=fallback";
	}
	return UpdateResult{true, summary};
#endif
}

}  // namespace self_update
