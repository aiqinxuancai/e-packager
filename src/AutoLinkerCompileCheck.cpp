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
	const DWORD ideExitCode,
	const bool ideTimedOut,
	const std::string& ideOutput)
{
	std::ostringstream detail;
	detail << "compile_check_failed";
	if (ideTimedOut) {
		detail << ": IDE exceeded the outer timeout";
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
	detail << "\nide_exit_code=" << ideExitCode;

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
		const std::string fallback = TruncateDiagnostic(ideOutput);
		if (!fallback.empty()) {
			detail << "\nIDE output:\n" << fallback;
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
	outOptions.target = options.target.empty() ? "auto" : options.target;
	outOptions.staticCompile = options.staticCompile;
	outOptions.timeoutSeconds = options.timeoutSeconds;

	if (!IsRegularFile(outOptions.eIdePath)) {
		outError = "compile_check_eide_not_found: use --eide <e.exe> or E_PACKAGER_EIDE";
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

Result CompileToOutputWithEide(
	const std::filesystem::path& sourcePath,
	const std::filesystem::path& outputPath,
	const std::filesystem::path& eIdePath,
	const std::string& target,
	const bool staticCompile,
	const unsigned int timeoutSeconds,
	const std::filesystem::path& projectSourcePath)
{
	Result checkResult;
	const std::filesystem::path effectiveSourcePath = ResolveAbsolutePath(sourcePath);
	const std::filesystem::path effectiveOutputPath = ResolveAbsolutePath(outputPath);
	const std::filesystem::path effectiveEidePath = ResolveEIdePath(eIdePath);
	if (!IsRegularFile(effectiveSourcePath)) {
		checkResult.error = "eide_compile_source_not_found: " + PathToUtf8(effectiveSourcePath);
		return checkResult;
	}
	if (effectiveOutputPath.empty()) {
		checkResult.error = "eide_compile_output_path_missing";
		return checkResult;
	}
	if (!IsRegularFile(effectiveEidePath)) {
		checkResult.error = "eide_compile_eide_not_found: use --eide <e.exe> or E_PACKAGER_EIDE";
		return checkResult;
	}
	const std::string effectiveTarget = target.empty() ? "auto" : target;
	if (!IsSupportedTarget(effectiveTarget)) {
		checkResult.error = "eide_compile_target_invalid: " + effectiveTarget;
		return checkResult;
	}
	if (timeoutSeconds == 0 || timeoutSeconds > 3600) {
		checkResult.error = "eide_compile_timeout_invalid: expected 1..3600 seconds";
		return checkResult;
	}

	std::error_code filesystemError;
	if (effectiveOutputPath.has_parent_path()) {
		std::filesystem::create_directories(effectiveOutputPath.parent_path(), filesystemError);
		if (filesystemError) {
			checkResult.error = "eide_compile_output_directory_create_failed: " + filesystemError.message();
			return checkResult;
		}
	}
	std::filesystem::path invocationDirectory;
	if (!CreateTemporaryDirectory(invocationDirectory, checkResult.error)) {
		return checkResult;
	}
	const TemporaryDirectory invocationGuard(invocationDirectory);
	const std::filesystem::path resultPath = invocationDirectory / L"result.json";
	const std::string invocationId =
		"e-packager-" + std::to_string(GetCurrentProcessId()) + "-" + std::to_string(GetTickCount64());

	std::wstring commandLine;
	AppendCommandLineArgument(commandLine, effectiveEidePath.wstring());
	AppendCommandLineArgument(commandLine, effectiveSourcePath.wstring());
	AppendCommandLineArgument(commandLine, L"--autolinker-headless-compile");
	AppendCommandLineArgument(commandLine, L"--autolinker-output");
	AppendCommandLineArgument(commandLine, effectiveOutputPath.wstring());
	AppendCommandLineArgument(commandLine, L"--autolinker-target");
	AppendCommandLineArgument(commandLine, Utf8PathToPath(effectiveTarget).wstring());
	// AutoLinker uses the original project path for IDE-side diagnostics and
	// project-scoped linker settings.  Keep it explicit when e-packager starts
	// e.exe directly (the temporary source path is only the transport file).
	if (!projectSourcePath.empty()) {
		AppendCommandLineArgument(commandLine, L"--autolinker-project-source");
		AppendCommandLineArgument(commandLine, ResolveAbsolutePath(projectSourcePath).wstring());
	}
	AppendCommandLineArgument(commandLine, L"--autolinker-result");
	AppendCommandLineArgument(commandLine, resultPath.wstring());
	AppendCommandLineArgument(commandLine, L"--autolinker-startup-timeout");
	AppendCommandLineArgument(commandLine, std::to_wstring(timeoutSeconds));
	AppendCommandLineArgument(commandLine, L"--autolinker-invocation-id");
	AppendCommandLineArgument(commandLine, Utf8PathToPath(invocationId).wstring());
	AppendCommandLineArgument(commandLine, staticCompile ? L"--autolinker-static" : L"--autolinker-no-static");
	AppendCommandLineArgument(commandLine, L"--autolinker-hide-window");
	AppendCommandLineArgument(commandLine, L"--autolinker-exit");

	STARTUPINFOW startup{};
	startup.cb = sizeof(startup);
	startup.dwFlags = STARTF_USESHOWWINDOW;
	startup.wShowWindow = SW_HIDE;
	PROCESS_INFORMATION process{};
	std::vector<wchar_t> mutableCommandLine(commandLine.begin(), commandLine.end());
	mutableCommandLine.push_back(L'\0');
	const BOOL created = CreateProcessW(
		effectiveEidePath.c_str(),
		mutableCommandLine.data(),
		nullptr,
		nullptr,
		FALSE,
		CREATE_NO_WINDOW | CREATE_SUSPENDED | CREATE_UNICODE_ENVIRONMENT,
		nullptr,
		effectiveSourcePath.parent_path().empty() ? nullptr : effectiveSourcePath.parent_path().c_str(),
		&startup,
		&process);
	if (!created) {
		checkResult.error = "eide_compile_start_failed: win32_error=" + std::to_string(GetLastError());
		return checkResult;
	}
	if (ResumeThread(process.hThread) == static_cast<DWORD>(-1)) {
		const DWORD error = GetLastError();
		TerminateProcess(process.hProcess, ERROR_PROCESS_ABORTED);
		WaitForSingleObject(process.hProcess, 5000);
		CloseHandle(process.hThread);
		CloseHandle(process.hProcess);
		checkResult.error = "eide_compile_resume_failed: win32_error=" + std::to_string(error);
		return checkResult;
	}
	CloseHandle(process.hThread);

	const std::uint64_t timeoutMilliseconds =
		(static_cast<std::uint64_t>(timeoutSeconds) + 60u) * 1000u;
	const DWORD waitMilliseconds = timeoutMilliseconds > MAXDWORD
		? MAXDWORD : static_cast<DWORD>(timeoutMilliseconds);
	const DWORD waitResult = WaitForSingleObject(process.hProcess, waitMilliseconds);
	const bool timedOut = waitResult == WAIT_TIMEOUT;
	if (timedOut) {
		TerminateProcess(process.hProcess, ERROR_TIMEOUT);
		WaitForSingleObject(process.hProcess, 5000);
	}
	DWORD exitCode = static_cast<DWORD>(-1);
	GetExitCodeProcess(process.hProcess, &exitCode);
	CloseHandle(process.hProcess);
	const std::string resultText = ReadTextFile(resultPath);
	json parsedResult;
	const json* resultPointer = nullptr;
	if (!resultText.empty()) {
		try {
			parsedResult = json::parse(resultText);
			if (parsedResult.is_object()) resultPointer = &parsedResult;
		}
		catch (...) {
			resultPointer = nullptr;
		}
	}
	if (waitResult == WAIT_FAILED) {
		checkResult.error = "eide_compile_wait_failed: win32_error=" + std::to_string(GetLastError());
		return checkResult;
	}
	if (timedOut || resultPointer == nullptr) {
		checkResult.error = BuildFailureDiagnostic(resultPointer, exitCode, timedOut, resultText);
		return checkResult;
	}

	bool artifactVerified = false;
	std::uint64_t artifactBytes = 0;
	std::string resolvedTarget = effectiveTarget;
	try {
		artifactVerified = parsedResult.value("ok", false) &&
			parsedResult.contains("compile_result") && parsedResult["compile_result"].is_object() &&
			parsedResult["compile_result"].value("artifact_verified", false) &&
			parsedResult["compile_result"].value("output_file_exists", false);
		resolvedTarget = parsedResult.value("resolved_target", resolvedTarget);
		if (parsedResult.contains("compile_result") && parsedResult["compile_result"].is_object()) {
			artifactBytes = parsedResult["compile_result"].value("output_file_size_after_compile", std::uint64_t(0));
		}
	}
	catch (...) {
		artifactVerified = false;
	}
	if (exitCode != 0 || !parsedResult.value("ok", false) || !artifactVerified || !IsRegularFile(effectiveOutputPath)) {
		checkResult.error = BuildFailureDiagnostic(resultPointer, exitCode, false, resultText);
		return checkResult;
	}
	checkResult.ok = true;
	checkResult.summary = "eide_compile=passed, target=" + resolvedTarget +
		", static=" + (staticCompile ? "true" : "false") +
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
	return CompileToOutputWithEide(
		sourcePath, artifactPath, options.eIdePath, options.target,
		options.staticCompile, options.timeoutSeconds);
}

Result CompileToOutput(
	const std::filesystem::path& sourcePath,
	const std::filesystem::path& outputPath,
	const PreparedOptions& options)
{
	return CompileToOutputWithEide(
		sourcePath, outputPath, options.eIdePath, options.target,
		options.staticCompile, options.timeoutSeconds);
}

}  // namespace autolinker_compile_check
