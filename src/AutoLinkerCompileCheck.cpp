#include "AutoLinkerCompileCheck.h"

#include <Windows.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <fstream>
#include <sstream>
#include <string_view>
#include <system_error>
#include <vector>

#include "..\thirdparty\json.hpp"
#include "PathHelper.h"

namespace autolinker_compile_check {
namespace {

using json = nlohmann::json;

constexpr std::size_t kMaximumDiagnosticTextBytes = 16 * 1024;

std::filesystem::path ResolveAbsolutePath(const std::filesystem::path& path)
{
	if (path.empty()) {
		return {};
	}
	std::error_code ec;
	const std::filesystem::path absolute = std::filesystem::absolute(path, ec);
	return ec ? path : absolute;
}

bool IsRegularFile(const std::filesystem::path& path)
{
	std::error_code ec;
	return !path.empty() && std::filesystem::is_regular_file(path, ec);
}

std::filesystem::path ReadEnvironmentPath(const wchar_t* name)
{
	const DWORD required = GetEnvironmentVariableW(name, nullptr, 0);
	if (required == 0) {
		return {};
	}
	std::wstring value(static_cast<std::size_t>(required), L'\0');
	const DWORD written = GetEnvironmentVariableW(name, value.data(), required);
	if (written == 0 || written >= required) {
		return {};
	}
	value.resize(written);
	return std::filesystem::path(value);
}

std::filesystem::path GetCurrentExecutablePath()
{
	std::vector<wchar_t> buffer(1024, L'\0');
	for (;;) {
		const DWORD length = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
		if (length == 0) {
			return {};
		}
		if (length < buffer.size() - 1) {
			return std::filesystem::path(std::wstring(buffer.data(), length));
		}
		buffer.resize(buffer.size() * 2, L'\0');
	}
}

std::filesystem::path SearchExecutableOnPath(const wchar_t* fileName)
{
	const DWORD required = SearchPathW(nullptr, fileName, nullptr, 0, nullptr, nullptr);
	if (required == 0) {
		return {};
	}
	std::wstring value(static_cast<std::size_t>(required) + 1, L'\0');
	wchar_t* filePart = nullptr;
	const DWORD written = SearchPathW(
		nullptr,
		fileName,
		nullptr,
		static_cast<DWORD>(value.size()),
		value.data(),
		&filePart);
	if (written == 0 || written >= value.size()) {
		return {};
	}
	value.resize(written);
	return std::filesystem::path(value);
}

std::filesystem::path ResolveEIdePath(const std::filesystem::path& requested)
{
	if (!requested.empty()) {
		return ResolveAbsolutePath(requested);
	}
	const std::filesystem::path environment = ReadEnvironmentPath(L"E_PACKAGER_EIDE");
	if (!environment.empty()) {
		return ResolveAbsolutePath(environment);
	}
	for (const auto& candidate : GetRegisteredEplOpenCommandExecutablePaths()) {
		if (IsRegularFile(candidate)) {
			return ResolveAbsolutePath(candidate);
		}
	}
	return {};
}

std::filesystem::path ResolveLauncherPath(const std::filesystem::path& requested)
{
	if (!requested.empty()) {
		return ResolveAbsolutePath(requested);
	}
	const std::filesystem::path environment = ReadEnvironmentPath(L"E_PACKAGER_AUTOLINKER_TEST");
	if (!environment.empty()) {
		return ResolveAbsolutePath(environment);
	}

	const std::filesystem::path executable = GetCurrentExecutablePath();
	if (!executable.empty()) {
		const std::filesystem::path sibling = executable.parent_path() / L"AutoLinkerTest.exe";
		if (IsRegularFile(sibling)) {
			return ResolveAbsolutePath(sibling);
		}
	}

	const std::filesystem::path fromPath = SearchExecutableOnPath(L"AutoLinkerTest.exe");
	return fromPath.empty() ? std::filesystem::path() : ResolveAbsolutePath(fromPath);
}

bool IsSupportedTarget(const std::string_view target)
{
	return target == "auto" || target == "win_exe" || target == "win_console_exe" ||
		target == "win_dll" || target == "ecom";
}

std::wstring QuoteCommandLineArgument(const std::wstring& argument)
{
	if (!argument.empty() && argument.find_first_of(L" \t\n\v\"") == std::wstring::npos) {
		return argument;
	}

	std::wstring quoted(1, L'\"');
	std::size_t backslashes = 0;
	for (const wchar_t ch : argument) {
		if (ch == L'\\') {
			++backslashes;
			continue;
		}
		if (ch == L'\"') {
			quoted.append(backslashes * 2 + 1, L'\\');
			quoted.push_back(ch);
			backslashes = 0;
			continue;
		}
		quoted.append(backslashes, L'\\');
		backslashes = 0;
		quoted.push_back(ch);
	}
	quoted.append(backslashes * 2, L'\\');
	quoted.push_back(L'\"');
	return quoted;
}

void AppendCommandLineArgument(std::wstring& commandLine, const std::wstring& argument)
{
	if (!commandLine.empty()) {
		commandLine.push_back(L' ');
	}
	commandLine += QuoteCommandLineArgument(argument);
}

std::string ReadTextFile(const std::filesystem::path& path, const std::size_t maximumBytes = 0)
{
	std::ifstream input(path, std::ios::binary);
	if (!input.is_open()) {
		return {};
	}
	std::string text;
	if (maximumBytes == 0) {
		text.assign(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
		return text;
	}
	text.resize(maximumBytes);
	input.read(text.data(), static_cast<std::streamsize>(text.size()));
	text.resize(static_cast<std::size_t>(input.gcount()));
	return text;
}

std::string TrimAscii(std::string value)
{
	const auto isSpace = [](const unsigned char ch) {
		return ch == ' ' || ch == '\t' || ch == '\r' || ch == '\n' || ch == '\f' || ch == '\v';
	};
	while (!value.empty() && isSpace(static_cast<unsigned char>(value.front()))) {
		value.erase(value.begin());
	}
	while (!value.empty() && isSpace(static_cast<unsigned char>(value.back()))) {
		value.pop_back();
	}
	return value;
}

std::string TruncateDiagnostic(std::string text)
{
	text = TrimAscii(std::move(text));
	if (text.size() <= kMaximumDiagnosticTextBytes) {
		return text;
	}
	text.resize(kMaximumDiagnosticTextBytes);
	text += "\n... diagnostic output truncated";
	return text;
}

bool CreateTemporaryDirectory(std::filesystem::path& outDirectory, std::string& outError)
{
	outDirectory.clear();
	wchar_t temporaryPath[MAX_PATH + 1] = {};
	const DWORD length = GetTempPathW(MAX_PATH, temporaryPath);
	if (length == 0 || length > MAX_PATH) {
		outError = "compile_check_temp_path_unavailable";
		return false;
	}

	const std::filesystem::path root =
		std::filesystem::path(temporaryPath) / L"e-packager" / L"compile-check";
	std::error_code ec;
	std::filesystem::create_directories(root, ec);
	if (ec) {
		outError = "compile_check_temp_directory_create_failed: " + PathToUtf8(root);
		return false;
	}

	const std::uint64_t seed = static_cast<std::uint64_t>(GetTickCount64());
	for (unsigned int attempt = 0; attempt < 100; ++attempt) {
		const std::wstring name = std::to_wstring(GetCurrentProcessId()) + L"-" +
			std::to_wstring(seed) + L"-" + std::to_wstring(attempt);
		const std::filesystem::path candidate = root / name;
		ec.clear();
		if (std::filesystem::create_directory(candidate, ec)) {
			outDirectory = candidate;
			return true;
		}
		if (ec && ec != std::errc::file_exists) {
			outError = "compile_check_temp_directory_create_failed: " + PathToUtf8(candidate);
			return false;
		}
	}

	outError = "compile_check_temp_directory_collision";
	return false;
}

class TemporaryDirectory final {
public:
	explicit TemporaryDirectory(std::filesystem::path path) : path_(std::move(path))
	{
	}

	~TemporaryDirectory()
	{
		if (path_.empty()) {
			return;
		}
		std::error_code ec;
		std::filesystem::remove_all(path_, ec);
	}

	TemporaryDirectory(const TemporaryDirectory&) = delete;
	TemporaryDirectory& operator=(const TemporaryDirectory&) = delete;

private:
	std::filesystem::path path_;
};

std::string ArtifactExtension(const std::string_view target)
{
	if (target == "win_dll") {
		return ".dll";
	}
	if (target == "ecom") {
		return ".ec";
	}
	return ".exe";
}

std::string BuildFailureDiagnostic(
	const json* result,
	const DWORD launcherExitCode,
	const bool launcherTimedOut,
	const std::string& launcherOutput)
{
	std::ostringstream detail;
	detail << "compile_check_failed";
	if (launcherTimedOut) {
		detail << ": AutoLinker launcher exceeded the outer timeout";
	}
	else if (result != nullptr) {
		const std::string error = result->value("error", std::string());
		if (!error.empty()) {
			detail << ": " << error;
		}
	}
	else {
		detail << ": AutoLinker result JSON was not produced";
	}
	detail << "\nlauncher_exit_code=" << launcherExitCode;

	if (result != nullptr && result->contains("compile_result") && (*result)["compile_result"].is_object()) {
		const json& compileResult = (*result)["compile_result"];
		const std::string page = compileResult.value("caret_page_name", std::string());
		const int row = compileResult.value("caret_row", -1);
		if (!page.empty() || row >= 0) {
			detail << "\nerror_location: page=" << page << ", row=" << row;
		}
		const std::string line = compileResult.value("caret_line_text", std::string());
		if (!line.empty()) {
			detail << "\nerror_line: " << line;
		}
		const std::string ideOutput = TruncateDiagnostic(
			compileResult.value("output_window_text", std::string()));
		if (!ideOutput.empty()) {
			detail << "\nIDE output:\n" << ideOutput;
		}
	}

	if (result == nullptr) {
		const std::string fallback = TruncateDiagnostic(launcherOutput);
		if (!fallback.empty()) {
			detail << "\nAutoLinker output:\n" << fallback;
		}
	}
	return detail.str();
}

}  // namespace

bool Prepare(const Options& options, PreparedOptions& outOptions, std::string& outError)
{
	outOptions = {};
	outError.clear();
	outOptions.eIdePath = ResolveEIdePath(options.eIdePath);
	outOptions.launcherPath = ResolveLauncherPath(options.launcherPath);
	outOptions.target = options.target.empty() ? "auto" : options.target;
	outOptions.staticCompile = options.staticCompile;
	outOptions.timeoutSeconds = options.timeoutSeconds;

	if (!IsRegularFile(outOptions.eIdePath)) {
		outError = "compile_check_eide_not_found: use --eide <e.exe> or E_PACKAGER_EIDE";
		return false;
	}
	if (!IsRegularFile(outOptions.launcherPath)) {
		outError = "compile_check_autolinker_test_not_found: use --autolinker-test <AutoLinkerTest.exe> or E_PACKAGER_AUTOLINKER_TEST";
		return false;
	}
	if (!IsSupportedTarget(outOptions.target)) {
		outError = "compile_check_target_invalid: " + outOptions.target;
		return false;
	}
	if (outOptions.timeoutSeconds == 0 || outOptions.timeoutSeconds > 3600) {
		outError = "compile_check_timeout_invalid: expected 1..3600 seconds";
		return false;
	}
	return true;
}

Result RunToArtifact(
	const std::filesystem::path& sourcePath,
	const std::filesystem::path& artifactPath,
	const PreparedOptions& options,
	const std::filesystem::path& invocationDirectory)
{
	Result checkResult;
	const std::filesystem::path effectiveSourcePath = ResolveAbsolutePath(sourcePath);
	const std::filesystem::path effectiveArtifactPath = ResolveAbsolutePath(artifactPath);
	if (!IsRegularFile(effectiveSourcePath)) {
		checkResult.error = "compile_check_source_not_found: " + PathToUtf8(effectiveSourcePath);
		return checkResult;
	}
	if (effectiveArtifactPath.empty()) {
		checkResult.error = "compile_check_output_path_missing";
		return checkResult;
	}
	const std::filesystem::path resultPath = invocationDirectory / L"result.json";
	const std::filesystem::path launcherLogPath = invocationDirectory / L"launcher.log";

	std::wstring commandLine;
	AppendCommandLineArgument(commandLine, options.launcherPath.wstring());
	AppendCommandLineArgument(commandLine, L"headless-compile");
	AppendCommandLineArgument(commandLine, options.eIdePath.wstring());
	AppendCommandLineArgument(commandLine, effectiveSourcePath.wstring());
	AppendCommandLineArgument(commandLine, effectiveArtifactPath.wstring());
	AppendCommandLineArgument(commandLine, L"--target");
	AppendCommandLineArgument(commandLine, Utf8PathToPath(options.target).wstring());
	if (options.staticCompile) {
		AppendCommandLineArgument(commandLine, L"--static");
	}
	AppendCommandLineArgument(commandLine, L"--result");
	AppendCommandLineArgument(commandLine, resultPath.wstring());
	AppendCommandLineArgument(commandLine, L"--timeout");
	AppendCommandLineArgument(commandLine, std::to_wstring(options.timeoutSeconds));

	SECURITY_ATTRIBUTES inheritedSecurityAttributes{};
	inheritedSecurityAttributes.nLength = sizeof(inheritedSecurityAttributes);
	inheritedSecurityAttributes.bInheritHandle = TRUE;
	HANDLE launcherLog = CreateFileW(
		launcherLogPath.c_str(),
		GENERIC_WRITE,
		FILE_SHARE_READ | FILE_SHARE_DELETE,
		&inheritedSecurityAttributes,
		CREATE_ALWAYS,
		FILE_ATTRIBUTE_TEMPORARY,
		nullptr);
	if (launcherLog == INVALID_HANDLE_VALUE) {
		checkResult.error = "compile_check_launcher_log_create_failed";
		return checkResult;
	}

	STARTUPINFOW startup{};
	startup.cb = sizeof(startup);
	startup.dwFlags = STARTF_USESHOWWINDOW | STARTF_USESTDHANDLES;
	startup.wShowWindow = SW_HIDE;
	startup.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
	startup.hStdOutput = launcherLog;
	startup.hStdError = launcherLog;
	PROCESS_INFORMATION process{};
	std::vector<wchar_t> mutableCommandLine(commandLine.begin(), commandLine.end());
	mutableCommandLine.push_back(L'\0');
	const std::filesystem::path workingDirectory = effectiveSourcePath.parent_path();
	const BOOL created = CreateProcessW(
		options.launcherPath.c_str(),
		mutableCommandLine.data(),
		nullptr,
		nullptr,
		TRUE,
		CREATE_NO_WINDOW,
		nullptr,
		workingDirectory.empty() ? nullptr : workingDirectory.c_str(),
		&startup,
		&process);
	CloseHandle(launcherLog);
	if (created == FALSE) {
		checkResult.error = "compile_check_launcher_start_failed: win32_error=" +
			std::to_string(GetLastError());
		return checkResult;
	}

	const std::uint64_t outerTimeoutMilliseconds =
		(static_cast<std::uint64_t>(options.timeoutSeconds) + 60u) * 1000u;
	const DWORD waitMilliseconds = outerTimeoutMilliseconds > MAXDWORD
		? MAXDWORD
		: static_cast<DWORD>(outerTimeoutMilliseconds);
	const DWORD waitResult = WaitForSingleObject(process.hProcess, waitMilliseconds);
	const bool launcherTimedOut = waitResult == WAIT_TIMEOUT;
	if (launcherTimedOut) {
		TerminateProcess(process.hProcess, 5);
		WaitForSingleObject(process.hProcess, 5000);
	}

	DWORD launcherExitCode = static_cast<DWORD>(-1);
	GetExitCodeProcess(process.hProcess, &launcherExitCode);
	CloseHandle(process.hThread);
	CloseHandle(process.hProcess);

	json parsedResult;
	const json* resultPointer = nullptr;
	const std::string resultText = ReadTextFile(resultPath);
	if (!resultText.empty()) {
		try {
			parsedResult = json::parse(resultText);
			if (parsedResult.is_object()) {
				resultPointer = &parsedResult;
			}
		}
		catch (...) {
			resultPointer = nullptr;
		}
	}

	bool jsonOk = false;
	bool artifactVerified = false;
	if (resultPointer != nullptr) {
		try {
			jsonOk = resultPointer->value("ok", false);
			if (resultPointer->contains("compile_result") && (*resultPointer)["compile_result"].is_object()) {
				artifactVerified = (*resultPointer)["compile_result"].value("artifact_verified", false);
			}
		}
		catch (...) {
			jsonOk = false;
			artifactVerified = false;
		}
	}

	checkResult.ok = !launcherTimedOut && launcherExitCode == 0 && jsonOk && artifactVerified &&
		IsRegularFile(effectiveArtifactPath);
	if (!checkResult.ok) {
		checkResult.error = BuildFailureDiagnostic(
			resultPointer,
			launcherExitCode,
			launcherTimedOut,
			ReadTextFile(launcherLogPath, kMaximumDiagnosticTextBytes));
		return checkResult;
	}

	std::string resolvedTarget = options.target;
	std::uint64_t artifactBytes = 0;
	try {
		resolvedTarget = resultPointer->value("resolved_target", resolvedTarget);
		const json& compileResult = (*resultPointer)["compile_result"];
		artifactBytes = compileResult.value("output_file_size_after_compile", std::uint64_t(0));
	}
	catch (...) {
		// 成功条件已经由 ok 和 artifact_verified 确认，摘要字段缺失不改变结果。
	}
	checkResult.summary = "compile_check=passed, target=" + resolvedTarget +
		", static=" + (options.staticCompile ? "true" : "false") +
		", artifact_bytes=" + std::to_string(artifactBytes);
	return checkResult;
}

Result Run(const std::filesystem::path& sourcePath, const PreparedOptions& options)
{
	Result result;
	std::filesystem::path temporaryDirectory;
	if (!CreateTemporaryDirectory(temporaryDirectory, result.error)) {
		return result;
	}
	const TemporaryDirectory temporaryDirectoryGuard(temporaryDirectory);
	const std::filesystem::path artifactPath =
		temporaryDirectory / (L"artifact" + Utf8PathToPath(ArtifactExtension(options.target)).wstring());
	return RunToArtifact(sourcePath, artifactPath, options, temporaryDirectory);
}

Result CompileToOutput(
	const std::filesystem::path& sourcePath,
	const std::filesystem::path& outputPath,
	const PreparedOptions& options)
{
	Result result;
	std::filesystem::path temporaryDirectory;
	if (!CreateTemporaryDirectory(temporaryDirectory, result.error)) {
		return result;
	}
	const TemporaryDirectory temporaryDirectoryGuard(temporaryDirectory);
	return RunToArtifact(sourcePath, outputPath, options, temporaryDirectory);
}

}  // namespace autolinker_compile_check
