// 黑月源码编译方式：无黑月 FNE 注入地调用易代码转换器及其三种链接入口。
#include "BlackMoonCompiler.h"

#include "BlackMoonEcodeBridge.h"
#include "ECompiler.h"

#include "../AutoLinkerCompileCheck.h"
#include "../EFolderCodec.h"
#include "../PathHelper.h"
#include "../e2txt.h"

#include <Windows.h>

#include <algorithm>
#include <chrono>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

namespace ecompiler::blackmoon_compiler {
namespace {

std::filesystem::path AbsolutePath(const std::filesystem::path& path)
{
	std::error_code error;
	const std::filesystem::path absolute = std::filesystem::absolute(path, error);
	return error ? path : absolute;
}

bool IsRegularFile(const std::filesystem::path& path)
{
	std::error_code error;
	return !path.empty() && std::filesystem::is_regular_file(path, error);
}

std::wstring Quote(const std::filesystem::path& value)
{
	return L"\"" + value.wstring() + L"\"";
}

std::wstring Quote(const std::wstring& value)
{
	return L"\"" + value + L"\"";
}

std::string ToLowerAscii(std::string value)
{
	std::transform(value.begin(), value.end(), value.begin(), [](const unsigned char character) {
		return static_cast<char>(std::tolower(character));
	});
	return value;
}

// 黑月核心归档可能携带 MFC 的默认库指令。非 MFC 入口遇到这类归档时，
// 直接屏蔽 nafxcw 会留下 CString/CWordArray 等未解析符号；应改用 MFC
// 入口让 CRT/MFC 完整初始化，而不是用 /FORCE 忽略运行库冲突。
bool IsMfcRuntimeLinkFailure(const std::string& linkerError)
{
	const std::string lower = ToLowerAscii(linkerError);
	const bool hasMfcLibrary = lower.find("nafx") != std::string::npos ||
		lower.find("mfcs") != std::string::npos ||
		lower.find("uafxc") != std::string::npos ||
		(lower.find("mfc") != std::string::npos &&
			lower.find(".lib") != std::string::npos);
	const bool hasLinkDiagnostic = lower.find("lnk2001") != std::string::npos ||
		lower.find("lnk2005") != std::string::npos ||
		lower.find("lnk1120") != std::string::npos ||
		lower.find("lnk1169") != std::string::npos;
	return hasMfcLibrary && hasLinkDiagnostic;
}

bool HasExtension(const std::filesystem::path& path, const std::wstring_view extension)
{
	std::wstring actual = path.extension().wstring();
	std::transform(actual.begin(), actual.end(), actual.begin(), towlower);
	return actual == extension;
}

class TemporaryDirectory final {
public:
	explicit TemporaryDirectory(std::filesystem::path path) : path_(std::move(path)) {}
	~TemporaryDirectory()
	{
		if (path_.empty()) return;
		std::error_code error;
		std::filesystem::remove_all(path_, error);
	}

	TemporaryDirectory(const TemporaryDirectory&) = delete;
	TemporaryDirectory& operator=(const TemporaryDirectory&) = delete;

private:
	std::filesystem::path path_;
};

bool CreateTemporaryDirectory(std::filesystem::path& outPath, std::string& error)
{
	outPath.clear();
	wchar_t temporaryPath[MAX_PATH + 1] = {};
	const DWORD length = GetTempPathW(MAX_PATH, temporaryPath);
	if (length == 0 || length > MAX_PATH) {
		error = "blackmoon_temp_path_unavailable";
		return false;
	}

	const std::filesystem::path root = std::filesystem::path(temporaryPath) / L"e-packager" / L"blackmoon";
	std::error_code filesystemError;
	std::filesystem::create_directories(root, filesystemError);
	if (filesystemError) {
		error = "blackmoon_temp_directory_create_failed:" + PathToUtf8(root);
		return false;
	}

	const std::uint64_t seed = static_cast<std::uint64_t>(GetTickCount64());
	for (unsigned int attempt = 0; attempt < 100; ++attempt) {
		const std::filesystem::path candidate = root /
			(std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(seed) + L"-" + std::to_wstring(attempt));
		filesystemError.clear();
		if (std::filesystem::create_directory(candidate, filesystemError)) {
			outPath = candidate;
			return true;
		}
		if (filesystemError && filesystemError != std::errc::file_exists) {
			error = "blackmoon_temp_directory_create_failed:" + PathToUtf8(candidate);
			return false;
		}
	}

	error = "blackmoon_temp_directory_collision";
	return false;
}

bool WriteBytes(const std::filesystem::path& path, const std::vector<std::uint8_t>& bytes, std::string& error)
{
	std::ofstream output(path, std::ios::binary | std::ios::trunc);
	if (!output) {
		error = "blackmoon_stage_source_open_failed:" + PathToUtf8(path);
		return false;
	}
	if (!bytes.empty()) output.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
	if (!output.good()) {
		error = "blackmoon_stage_source_write_failed:" + PathToUtf8(path);
		return false;
	}
	return true;
}

bool PrepareSource(
	const std::filesystem::path& inputPath,
	const std::filesystem::path& workingDirectory,
	std::filesystem::path& outSourcePath,
	std::filesystem::path& outProjectRoot,
	std::string& error)
{
	outProjectRoot.clear();
	std::error_code filesystemError;
	if (std::filesystem::is_regular_file(inputPath, filesystemError)) {
		if (!HasExtension(inputPath, L".e")) {
			error = "blackmoon_input_must_be_e_or_directory:" + PathToUtf8(inputPath);
			return false;
		}
		outSourcePath = AbsolutePath(inputPath);
		outProjectRoot = outSourcePath.parent_path();
		return true;
	}
	if (!std::filesystem::is_directory(inputPath, filesystemError)) {
		error = "blackmoon_input_not_found:" + PathToUtf8(inputPath);
		return false;
	}

	e2txt::BundleDirectoryCodec codec;
	e2txt::ProjectBundle bundle;
	if (!codec.ReadBundle(PathToUtf8(inputPath), bundle, &error)) {
		error = "blackmoon_read_bundle_failed:" + error;
		return false;
	}
	if (bundle.sourceFileKind != e2txt::SourceFileKind::E) {
		error = "blackmoon_directory_input_must_be_e_project";
		return false;
	}
	outProjectRoot = AbsolutePath(inputPath);
	e2txt::Restorer restorer;
	std::vector<std::uint8_t> bytes;
	if (!restorer.RestoreBundleToBytes(bundle, bytes, &error)) {
		error = "blackmoon_restore_bundle_failed:" + error;
		return false;
	}
	outSourcePath = workingDirectory / L"source.e";
	return WriteBytes(outSourcePath, bytes, error);
}

bool ReadTextFile(const std::filesystem::path& path, std::string& outText)
{
	std::ifstream input(path, std::ios::binary);
	if (!input) return false;
	std::ostringstream text;
	text << input.rdbuf();
	outText = text.str();
	return true;
}

bool RunProcess(
	const std::filesystem::path& executable,
	const std::vector<std::wstring>& arguments,
	const std::filesystem::path& workingDirectory,
	const std::filesystem::path& logPath,
	const unsigned int timeoutSeconds,
	std::string& output,
	std::string& error)
{
	std::wstring command = Quote(executable);
	for (const std::wstring& argument : arguments) {
		command.push_back(L' ');
		command += argument;
	}
	HANDLE log = CreateFileW(
		logPath.c_str(), GENERIC_WRITE | GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
		nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_TEMPORARY, nullptr);
	if (log == INVALID_HANDLE_VALUE) {
		error = "blackmoon_process_log_create_failed:" + std::to_string(GetLastError());
		return false;
	}
	SetHandleInformation(log, HANDLE_FLAG_INHERIT, HANDLE_FLAG_INHERIT);
	STARTUPINFOW startup{};
	startup.cb = sizeof(startup);
	startup.dwFlags = STARTF_USESTDHANDLES;
	startup.hStdOutput = log;
	startup.hStdError = log;
	startup.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
	PROCESS_INFORMATION process{};
	std::vector<wchar_t> mutableCommand(command.begin(), command.end());
	mutableCommand.push_back(L'\0');
	const BOOL started = CreateProcessW(
		executable.c_str(), mutableCommand.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW,
		nullptr, workingDirectory.empty() ? nullptr : workingDirectory.c_str(), &startup, &process);
	if (!started) {
		const DWORD code = GetLastError();
		CloseHandle(log);
		error = "blackmoon_process_start_failed:" + PathToUtf8(executable) + ":" + std::to_string(code);
		return false;
	}
	const DWORD waitMilliseconds = timeoutSeconds >= (MAXDWORD / 1000u)
		? INFINITE
		: timeoutSeconds * 1000u;
	const DWORD waitResult = WaitForSingleObject(process.hProcess, waitMilliseconds);
	const DWORD waitError = waitResult == WAIT_FAILED ? GetLastError() : ERROR_SUCCESS;
	if (waitResult == WAIT_TIMEOUT) {
		TerminateProcess(process.hProcess, ERROR_TIMEOUT);
		WaitForSingleObject(process.hProcess, 5000);
	}
	DWORD exitCode = 1;
	GetExitCodeProcess(process.hProcess, &exitCode);
	CloseHandle(process.hThread);
	CloseHandle(process.hProcess);
	FlushFileBuffers(log);
	CloseHandle(log);
	ReadTextFile(logPath, output);
	std::error_code removeError;
	std::filesystem::remove(logPath, removeError);
	if (waitResult == WAIT_TIMEOUT) {
		error = "blackmoon_process_timeout:" + PathToUtf8(executable) + ":seconds=" + std::to_string(timeoutSeconds);
		if (!output.empty()) error += "\n" + output;
		return false;
	}
	if (waitResult == WAIT_FAILED) {
		error = "blackmoon_process_wait_failed:" + PathToUtf8(executable) + ":" + std::to_string(waitError);
		return false;
	}
	if (exitCode != 0) {
		error = "blackmoon_process_failed:" + PathToUtf8(executable) + ":exit=" + std::to_string(exitCode);
		if (!output.empty()) error += "\n" + output;
		return false;
	}
	return true;
}

std::filesystem::path FindArtifact(
	const std::string& fileName,
	const std::vector<std::filesystem::path>& directories)
{
	const std::filesystem::path requested = Utf8PathToPath(fileName);
	if (requested.empty()) return {};
	if (requested.is_absolute() && IsRegularFile(requested)) return requested;
	for (const std::filesystem::path& directory : directories) {
		const std::filesystem::path relativeCandidate = directory / requested;
		if (IsRegularFile(relativeCandidate)) return relativeCandidate;
		// 支持库通常只给出文件名；也兼容配置中遗留的相对目录。
		if (requested.has_parent_path()) {
			const std::filesystem::path fileNameCandidate = directory / requested.filename();
			if (IsRegularFile(fileNameCandidate)) return fileNameCandidate;
		}
	}
	return {};
}

void AppendUnique(std::vector<std::filesystem::path>& paths, const std::filesystem::path& path)
{
	if (path.empty()) return;
	for (const std::filesystem::path& existing : paths) {
		if (ToLowerAscii(PathToUtf8(existing.lexically_normal())) == ToLowerAscii(PathToUtf8(path.lexically_normal()))) return;
	}
	paths.push_back(path);
}

std::string ModeName(const BlackMoonMode mode)
{
	switch (mode) {
	case BlackMoonMode::Assembly: return "asm";
	case BlackMoonMode::Cpp: return "cpp";
	case BlackMoonMode::Mfc: return "mfc";
	}
	return "unknown";
}

bool WriteDllDefinition(
	const std::filesystem::path& path,
	const std::filesystem::path& outputPath,
	const std::vector<std::string>& names,
	std::string& error)
{
	std::ofstream output(path, std::ios::binary | std::ios::trunc);
	if (!output) {
		error = "blackmoon_def_open_failed:" + PathToUtf8(path);
		return false;
	}
	output << "LIBRARY \"" << outputPath.stem().string() << "\"\r\nEXPORTS\r\n";
	for (const std::string& name : names) {
		if (!name.empty()) output << "    " << name << "\r\n";
	}
	if (!output.good()) {
		error = "blackmoon_def_write_failed:" + PathToUtf8(path);
		return false;
	}
	return true;
}

}  // namespace

bool Compile(
	const std::filesystem::path& inputPath,
	const std::filesystem::path& outputPath,
	const Options& options,
	Result& result)
{
#if !defined(_M_IX86)
	(void)inputPath;
	(void)outputPath;
	(void)options;
	result = {};
	result.outputPath = outputPath;
	result.message = "blackmoon_compile_mode_requires_win32_e_packager";
	return false;
#else
	result = {};
	result.outputPath = outputPath;
	std::error_code filesystemError;
	const std::filesystem::path effectiveInput = AbsolutePath(inputPath);
	const std::filesystem::path effectiveOutput = AbsolutePath(outputPath);
	const std::filesystem::path outputDirectory = effectiveOutput.has_parent_path()
		? effectiveOutput.parent_path() : std::filesystem::current_path();
	std::filesystem::create_directories(outputDirectory, filesystemError);
	if (filesystemError) {
		result.message = "blackmoon_output_directory_create_failed:" + filesystemError.message();
		return false;
	}

	std::filesystem::path workingDirectory;
	std::string error;
	if (!CreateTemporaryDirectory(workingDirectory, error)) {
		result.message = error;
		return false;
	}
	const TemporaryDirectory workingDirectoryGuard(workingDirectory);

	std::filesystem::path sourcePath;
	std::filesystem::path projectRoot;
	if (!PrepareSource(effectiveInput, workingDirectory, sourcePath, projectRoot, error)) {
		result.message = error;
		return false;
	}

	const bool requestedDll = options.buildDll || HasExtension(effectiveOutput, L".dll");
	const std::filesystem::path stagePe = workingDirectory / (requestedDll ? L"ecode.dll" : L"ecode.exe");
	const auto stageResult = autolinker_compile_check::CompileToOutputWithEide(
		sourcePath,
		stagePe,
		options.eIdePath,
		requestedDll ? "win_dll" : "auto",
		// BlackMoon consumes the IDE's E-code container PE.  A static compile
		// performs the final linker pass and produces a normal PE instead, so the
		// staging compile must remain non-static.
		false,
		options.blackMoonTimeoutSeconds,
		effectiveInput);
	if (!stageResult.ok) {
		result.message = "blackmoon_stage_compile_failed:" + stageResult.error;
		return false;
	}
	if (!IsRegularFile(stagePe)) {
		result.message = "blackmoon_stage_artifact_missing:" + PathToUtf8(stagePe);
		return false;
	}
	std::filesystem::path retainedEcodePath;
	if (options.keepObject) {
		retainedEcodePath = effectiveOutput;
		retainedEcodePath.replace_extension(requestedDll ? L".blackmoon.ecode.dll" : L".blackmoon.ecode.exe");
		std::filesystem::copy_file(stagePe, retainedEcodePath, std::filesystem::copy_options::overwrite_existing, filesystemError);
		if (filesystemError) {
			result.message = "blackmoon_stage_artifact_copy_failed:" + filesystemError.message();
			return false;
		}
	}

	std::filesystem::path eidePath = options.eIdePath;
	if (eidePath.empty()) {
		const DWORD required = GetEnvironmentVariableW(L"E_PACKAGER_EIDE", nullptr, 0);
		if (required > 0) {
			std::wstring value(static_cast<std::size_t>(required), L'\0');
			const DWORD written = GetEnvironmentVariableW(L"E_PACKAGER_EIDE", value.data(), required);
			if (written > 0 && written < required) {
				value.resize(written);
				eidePath = value;
			}
		}
	}
	if (eidePath.empty()) {
		const auto candidates = GetRegisteredEplOpenCommandExecutablePaths();
		for (const auto& candidate : candidates) {
			if (IsRegularFile(candidate)) {
				eidePath = candidate;
				break;
			}
		}
	}
	if (eidePath.empty()) {
		result.message = "blackmoon_eide_not_found: use --eide <e.exe> or E_PACKAGER_EIDE";
		return false;
	}
	eidePath = AbsolutePath(eidePath);
	const std::filesystem::path eideDirectory = eidePath.parent_path();
	const std::filesystem::path blackMoonDirectory = options.blackMoonDirectory.empty()
		? eideDirectory / L"BlackMoon" : AbsolutePath(options.blackMoonDirectory);
	const std::filesystem::path blackMoonLibraryDirectory = blackMoonDirectory / L"lib";
	const std::filesystem::path blackMoonBinaryDirectory = blackMoonDirectory / L"bin";
	const std::filesystem::path linkerPath = options.legacyBlackMoonLinkerPath.empty()
		? blackMoonBinaryDirectory / L"LINK.EXE" : AbsolutePath(options.legacyBlackMoonLinkerPath);
	if (!IsRegularFile(linkerPath) || !std::filesystem::is_directory(blackMoonLibraryDirectory, filesystemError)) {
		result.message = "blackmoon_toolchain_not_found:root=" + PathToUtf8(blackMoonDirectory);
		return false;
	}

	std::vector<std::filesystem::path> libraryDirectories;
	if (!options.eDirectory.empty()) {
		libraryDirectories.push_back(AbsolutePath(options.eDirectory) / L"static_lib");
	}
	libraryDirectories.insert(libraryDirectories.end(), {
		blackMoonLibraryDirectory,
		blackMoonDirectory,
		eideDirectory / L"static_lib",
		outputDirectory,
		sourcePath.parent_path(),
		projectRoot,
	});
	const std::filesystem::path objectPath = workingDirectory / L"BlackMoon.obj";
	blackmoon::ConversionResult conversion;
	if (!blackmoon::ConvertEcodePeToObject(
		stagePe,
		objectPath,
		eideDirectory,
		blackMoonLibraryDirectory,
		libraryDirectories,
		conversion,
		error)) {
		result.message = "blackmoon_object_conversion_failed:" + error;
		return false;
	}
	if (conversion.isDll != requestedDll) {
		result.message = "blackmoon_stage_target_mismatch";
		return false;
	}
	if (options.keepObject) {
		result.objectPath = effectiveOutput;
		result.objectPath.replace_extension(L".blackmoon.obj");
		std::filesystem::copy_file(
			objectPath,
			result.objectPath,
			std::filesystem::copy_options::overwrite_existing,
			filesystemError);
		if (filesystemError) {
			result.message = "blackmoon_object_copy_failed:" + filesystemError.message();
			return false;
		}
	}

	std::filesystem::path definitionPath;
	if (conversion.isDll) {
		definitionPath = workingDirectory / L"exports.def";
		if (!WriteDllDefinition(definitionPath, effectiveOutput, conversion.exportNames, error)) {
			result.message = error;
			return false;
		}
	}

	std::filesystem::path entryObject;
	std::filesystem::path initObject;
	std::vector<std::wstring> linkerArguments;
	BlackMoonMode effectiveMode = options.blackMoonMode;
	bool usedMfcFallback = false;

	// 根据实际入口模式集中选择 OBJ 和运行库选项。这样在链接失败后可以
	// 用同一套规则切换到 MFC 入口，不会在多个分支中遗漏 DLL/COM 组合。
	auto configureLinkMode = [&](const BlackMoonMode mode) {
		entryObject.clear();
		initObject.clear();
		linkerArguments.clear();
		if (conversion.isDll) {
			if (mode == BlackMoonMode::Mfc) {
				entryObject = blackMoonLibraryDirectory /
					(conversion.hasDllMain ? L"BlackMoonMFCdll2.obj" : L"BlackMoonMFCdll.obj");
				initObject = blackMoonLibraryDirectory /
					(conversion.usesCom ? L"EyMFCComInit.obj" : L"EyInit.obj");
				linkerArguments.push_back(L"/DLL");
			}
			else {
				entryObject = blackMoonLibraryDirectory /
					(conversion.hasDllMain ? L"BlackMoonDll2.obj" : L"BlackMoonDll.obj");
				initObject = blackMoonLibraryDirectory /
					(conversion.usesCom ? L"EyComInit.obj" : L"EyInit.obj");
				linkerArguments.push_back(L"/DLL");
				if (mode == BlackMoonMode::Assembly) {
					linkerArguments.push_back(L"/ENTRY:DllMain@12");
					linkerArguments.push_back(L"/NODEFAULTLIB:LIBCMT");
					linkerArguments.push_back(L"/DEFAULTLIB:MSVCRT");
				}
				else {
					linkerArguments.push_back(L"/NODEFAULTLIB:MSVCRT");
					linkerArguments.push_back(L"/DEFAULTLIB:LIBCMT");
				}
			}
			linkerArguments.push_back(L"/DEF:" + Quote(definitionPath));
		}
		else {
			if (mode == BlackMoonMode::Mfc) {
				entryObject = blackMoonLibraryDirectory /
					(conversion.isConsole ? L"MFCBlackMoonCon.obj" : L"MFCBlackMoon.obj");
				initObject = blackMoonLibraryDirectory /
					(conversion.usesCom ? L"EyMFCComInit.obj" : L"EyInit.obj");
			}
			else {
				entryObject = blackMoonLibraryDirectory / L"BlackMoonExe.obj";
				initObject = blackMoonLibraryDirectory /
					(conversion.usesCom ? L"EyComInit.obj" : L"EyInit.obj");
				if (mode == BlackMoonMode::Assembly) {
					linkerArguments.push_back(L"/ENTRY:BMEntrypoint");
					linkerArguments.push_back(L"/OPT:NOWIN98");
					linkerArguments.push_back(L"/NODEFAULTLIB:LIBCMT");
					linkerArguments.push_back(L"/DEFAULTLIB:MSVCRT");
				}
			}
			linkerArguments.push_back(
				L"/SUBSYSTEM:" + std::wstring(conversion.isConsole ? L"CONSOLE" : L"WINDOWS"));
		}
	};
	configureLinkMode(effectiveMode);
	if (!IsRegularFile(entryObject) || !IsRegularFile(initObject)) {
		result.message = "blackmoon_entry_object_not_found";
		return false;
	}

	std::vector<std::filesystem::path> linkedLibraries;
	const std::filesystem::path coreLibrary = FindArtifact("krnln.lib", libraryDirectories);
	if (coreLibrary.empty()) {
		result.message = "blackmoon_core_library_not_found:" + PathToUtf8(blackMoonLibraryDirectory);
		return false;
	}
	AppendUnique(linkedLibraries, coreLibrary);
	for (const blackmoon::SupportLibrary& library : conversion.supportLibraries) {
		const std::filesystem::path archive = FindArtifact(library.name + "_static.lib", libraryDirectories);
		if (archive.empty()) {
			result.message = "blackmoon_support_library_not_found:" + library.name;
			return false;
		}
		AppendUnique(linkedLibraries, archive);
		for (const std::string& dependency : library.dependentLibraries) {
			const std::string name = std::filesystem::path(dependency).has_extension()
				? dependency : dependency + ".lib";
			const std::filesystem::path artifact = FindArtifact(name, libraryDirectories);
			if (!artifact.empty()) AppendUnique(linkedLibraries, artifact);
		}
	}
	for (const std::string& library : conversion.userLibraries) {
		const std::filesystem::path artifact = FindArtifact(library, libraryDirectories);
		if (artifact.empty()) {
			result.message = "blackmoon_user_library_not_found:" + library;
			return false;
		}
		AppendUnique(linkedLibraries, artifact);
	}

	auto buildLinkArguments = [&]() {
		std::vector<std::wstring> arguments;
		arguments.push_back(Quote(entryObject));
		arguments.push_back(Quote(objectPath));
		arguments.push_back(Quote(initObject));
		arguments.push_back(L"/OUT:" + Quote(effectiveOutput));
		arguments.push_back(L"/NOLOGO");
		arguments.push_back(L"/INCREMENTAL:NO");
		arguments.push_back(L"/MACHINE:I386");
		for (const std::filesystem::path& directory : libraryDirectories) {
			if (std::filesystem::is_directory(directory, filesystemError)) {
				arguments.push_back(L"/LIBPATH:" + Quote(directory));
			}
		}
		for (const std::filesystem::path& library : linkedLibraries) {
			arguments.push_back(Quote(library));
		}
		arguments.insert(arguments.end(), linkerArguments.begin(), linkerArguments.end());
		return arguments;
	};

	std::vector<std::wstring> arguments = buildLinkArguments();
	std::string linkerOutput;
	if (!RunProcess(
		linkerPath,
		arguments,
		workingDirectory,
		workingDirectory / L"link.log",
		options.blackMoonTimeoutSeconds,
		linkerOutput,
		error)) {
		const std::string firstLinkError = error;
		// 某些黑月核心归档（尤其包含 PY.OBJ 的版本）虽然名称上是
		// “非 MFC 核心库”，实际仍要求 nafxcw。此时非 MFC 入口既不能
		// 屏蔽 MFC，也不能靠 /FORCE 混合两个 CRT；改用 MFC 入口重试。
		if (effectiveMode != BlackMoonMode::Mfc &&
			IsMfcRuntimeLinkFailure(firstLinkError)) {
			effectiveMode = BlackMoonMode::Mfc;
			usedMfcFallback = true;
			configureLinkMode(effectiveMode);
			if (!IsRegularFile(entryObject) || !IsRegularFile(initObject)) {
				result.message = firstLinkError +
					"\nblackmoon_mfc_fallback_entry_object_not_found";
				return false;
			}
			arguments = buildLinkArguments();
			error.clear();
			if (!RunProcess(
				linkerPath,
				arguments,
				workingDirectory,
				workingDirectory / L"link.log",
				options.blackMoonTimeoutSeconds,
				linkerOutput,
				error)) {
				result.message = firstLinkError +
					"\nblackmoon_mfc_fallback_failed:" + error;
				return false;
			}
		}
		else {
			result.message = firstLinkError;
			return false;
		}
	}
	if (!IsRegularFile(effectiveOutput)) {
		result.message = "blackmoon_linker_reported_success_without_output:" + PathToUtf8(effectiveOutput);
		return false;
	}

	result.sourcePath = retainedEcodePath.empty() ? sourcePath : retainedEcodePath;
	const std::uintmax_t outputBytes = std::filesystem::file_size(effectiveOutput, filesystemError);
	if (filesystemError) {
		result.message = "blackmoon_output_size_read_failed:" + filesystemError.message();
		return false;
	}
	result.ok = true;
	result.message = "compiled:" + PathToUtf8(effectiveOutput) +
		";compile_mode=legacy-blackmoon;mode=" + ModeName(options.blackMoonMode) +
		";effective_mode=" + ModeName(effectiveMode) +
		(usedMfcFallback ? ";runtime_fallback=mfc" : "") +
		";artifact_bytes=" + std::to_string(outputBytes) +
		";stage=" + stageResult.summary +
		";object=" + (result.objectPath.empty() ? std::string("<discarded>") : PathToUtf8(result.objectPath));
	return true;
#endif
}

}  // namespace ecompiler::blackmoon_compiler
