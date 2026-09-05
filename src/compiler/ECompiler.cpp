#include "ECompiler.h"

#include "BlackMoonCompiler.h"
#include "CompilerModel.h"
#include "CppEmitter.h"
#include "../PathHelper.h"
#include "../e2txt.h"
#include "../EFolderCodec.h"
#include "../../thirdparty/json.hpp"

#include <Windows.h>

#include <algorithm>
#include <cwchar>
#include <cwctype>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <optional>
#include <sstream>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

namespace ecompiler {
namespace {

using json = nlohmann::json;

std::wstring Quote(const std::filesystem::path& path)
{
	return L"\"" + path.wstring() + L"\"";
}

std::wstring Quote(const std::wstring& value)
{
	return L"\"" + value + L"\"";
}

bool IsRegularFile(const std::filesystem::path& path)
{
	std::error_code error;
	return std::filesystem::is_regular_file(path, error);
}

// Static libraries are COFF archives.  Reading the machine field from their
// object members lets the compiler reject a mismatched x86/x64 archive before
// generating a large C++ translation unit or invoking the linker.
std::optional<std::uint16_t> ReadCoffArchiveMachine(const std::filesystem::path& path)
{
	std::ifstream input(path, std::ios::binary);
	if (!input) return std::nullopt;
	char signature[8]{};
	input.read(signature, sizeof(signature));
	if (!input) return std::nullopt;
	if (std::string(signature, sizeof(signature)) != "!<arch>\n") {
		input.seekg(0, std::ios::beg);
		std::uint16_t machine = 0;
		input.read(reinterpret_cast<char*>(&machine), sizeof(machine));
		if (!input) return std::nullopt;
		return machine == 0x014c || machine == 0x8664 ? std::optional(machine) : std::nullopt;
	}

	bool sawObject = false;
	std::optional<std::uint16_t> firstMachine;
	for (;;) {
		char header[60]{};
		input.read(header, sizeof(header));
		if (input.eof()) break;
		if (!input) return std::nullopt;
		if (header[58] != '`' || header[59] != '\n') return std::nullopt;
		std::string sizeText(header + 48, 10);
		while (!sizeText.empty() && static_cast<unsigned char>(sizeText.back()) <= 0x20) sizeText.pop_back();
		std::uint64_t memberSize = 0;
		try {
			memberSize = sizeText.empty() ? 0 : std::stoull(sizeText);
		}
		catch (...) {
			return std::nullopt;
		}
		const std::streampos memberStart = input.tellg();
		if (memberStart < 0) return std::nullopt;
		std::uint16_t machine = 0;
		input.read(reinterpret_cast<char*>(&machine), sizeof(machine));
		if (!input) return std::nullopt;
		std::string memberName(header, 16);
		while (!memberName.empty() && (memberName.back() == ' ' || memberName.back() == '/')) memberName.pop_back();
		// Object members may use /<offset> names into the archive's long-name
		// table; only the two special linker/long-name members are non-COFF.
		const bool linkerMember = memberName.empty() || memberName == "/";
		if (!linkerMember && (machine == 0x014c || machine == 0x8664)) {
			sawObject = true;
			if (!firstMachine.has_value()) firstMachine = machine;
			else if (*firstMachine != machine) return std::nullopt;
		}
		input.seekg(memberStart + static_cast<std::streamoff>(memberSize), std::ios::beg);
		if (!input) return std::nullopt;
		if ((memberSize & 1u) != 0) input.seekg(1, std::ios::cur);
		if (!input) return std::nullopt;
	}
	return sawObject ? firstMachine : std::nullopt;
}

bool IsStaticLibraryCompatible(
	const std::filesystem::path& path,
	const TargetArchitecture architecture)
{
	const auto machine = ReadCoffArchiveMachine(path);
	if (!machine.has_value()) return true;
	return architecture == TargetArchitecture::X64 ? *machine == 0x8664 : *machine == 0x014c;
}

struct CoreArchiveSelection {
	std::filesystem::path primaryArchive;
	std::filesystem::path fallbackArchive;
	bool adapter = false;
};

bool ReadBlackMoonAdapterSelection(
	const std::filesystem::path& directory,
	const TargetArchitecture architecture,
	CoreArchiveSelection& selection)
{
	selection = {};
	const std::filesystem::path manifestPath = directory / L"krnln_adapter.json";
	if (!IsRegularFile(manifestPath)) return false;
	const std::string architectureName = architecture == TargetArchitecture::X64 ? "x64" : "x86";
	std::ifstream input(manifestPath, std::ios::binary);
	if (!input) return false;
	try {
		const json manifest = json::parse(input);
		if (manifest.value("formatVersion", 0) != 1 ||
			manifest.value("architecture", std::string()) != architectureName ||
			manifest.value("abi", std::string()) != "ecompiler-fne-execute-v1") {
			return false;
		}
		const std::string primaryName = manifest.value("primaryArchive", std::string());
		const std::string fallbackName = manifest.value("fallbackArchive", std::string());
		if (primaryName.empty() || fallbackName.empty()) return false;
		const std::filesystem::path primary = directory / Utf8PathToPath(primaryName);
		const std::filesystem::path fallback = directory / Utf8PathToPath(fallbackName);
		if (!IsRegularFile(primary) || !IsRegularFile(fallback) ||
			!IsStaticLibraryCompatible(primary, architecture) ||
			!IsStaticLibraryCompatible(fallback, architecture)) {
			return false;
		}
		selection.primaryArchive = primary;
		selection.fallbackArchive = fallback;
		selection.adapter = true;
		return true;
	}
	catch (...) {
		return false;
	}
}

std::filesystem::path AbsolutePath(const std::filesystem::path& path)
{
	std::error_code error;
	const auto absolute = std::filesystem::absolute(path, error);
	return error ? path : absolute;
}

std::filesystem::path LatestVersionDirectory(const std::filesystem::path& root)
{
	std::error_code error;
	std::vector<std::filesystem::path> directories;
	for (const auto& entry : std::filesystem::directory_iterator(root, error)) {
		if (entry.is_directory(error)) directories.push_back(entry.path());
	}
	std::sort(directories.begin(), directories.end(), [](const auto& left, const auto& right) {
		return left.filename().wstring() > right.filename().wstring();
	});
	return directories.empty() ? std::filesystem::path() : directories.front();
}

bool IsVcToolsDirectory(const std::filesystem::path& directory)
{
	return IsRegularFile(directory / L"include" / L"vector") &&
		std::filesystem::is_directory(directory / L"bin") &&
		std::filesystem::is_directory(directory / L"lib");
}

std::filesystem::path NormalizeVcToolsDirectory(const std::filesystem::path& configured)
{
	if (configured.empty()) return {};
	const std::filesystem::path root = AbsolutePath(configured);
	if (IsVcToolsDirectory(root)) return root;
	const std::filesystem::path latest = LatestVersionDirectory(root);
	return IsVcToolsDirectory(latest) ? latest : std::filesystem::path();
}

bool SelectWindowsSdk(
	const std::filesystem::path& configured,
	std::filesystem::path& sdkRoot,
	std::filesystem::path& versionedInclude)
{
	sdkRoot.clear();
	versionedInclude.clear();
	if (configured.empty()) return false;
	const std::filesystem::path root = AbsolutePath(configured);
	if (IsRegularFile(root / L"um" / L"windows.h")) {
		versionedInclude = root;
		sdkRoot = root.parent_path().parent_path();
		return true;
	}
	std::filesystem::path includeRoot;
	if (std::filesystem::is_directory(root / L"Include")) {
		sdkRoot = root;
		includeRoot = root / L"Include";
	}
	else if (root.filename() == L"Include") {
		sdkRoot = root.parent_path();
		includeRoot = root;
	}
	if (includeRoot.empty()) return false;
	versionedInclude = LatestVersionDirectory(includeRoot);
	return IsRegularFile(versionedInclude / L"um" / L"windows.h");
}

bool DiscoverBuildEnvironment(
	const TargetArchitecture architecture,
	const std::filesystem::path& configuredVcToolsDirectory,
	const std::filesystem::path& configuredWindowsSdkDirectory,
	std::filesystem::path& matchingCompiler,
	std::filesystem::path& resourceCompiler,
	std::vector<std::filesystem::path>& includeDirectories,
	std::vector<std::filesystem::path>& libraryDirectories,
	std::string& error)
{
	matchingCompiler.clear();
	resourceCompiler.clear();
	includeDirectories.clear();
	libraryDirectories.clear();
	std::filesystem::path vcTools = NormalizeVcToolsDirectory(configuredVcToolsDirectory);
	if (!configuredVcToolsDirectory.empty() && vcTools.empty()) {
		error = "vc_tools_directory_invalid:" + PathToUtf8(AbsolutePath(configuredVcToolsDirectory));
		return false;
	}
	wchar_t configured[MAX_PATH * 4] {};
	if (vcTools.empty() && GetEnvironmentVariableW(L"VCToolsInstallDir", configured, std::size(configured)) > 0) {
		vcTools = NormalizeVcToolsDirectory(std::filesystem::path(configured));
	}
	if (vcTools.empty() || !std::filesystem::is_directory(vcTools)) {
		std::error_code filesystemError;
		std::vector<std::filesystem::path> candidates;
		for (const auto& visualStudioRoot : {
			std::filesystem::path(L"C:\\Program Files\\Microsoft Visual Studio"),
			std::filesystem::path(L"C:\\Program Files (x86)\\Microsoft Visual Studio")
		}) {
			for (const auto& version : std::filesystem::directory_iterator(visualStudioRoot, filesystemError)) {
				if (!version.is_directory(filesystemError)) continue;
				for (const auto& edition : std::filesystem::directory_iterator(version.path(), filesystemError)) {
					if (!edition.is_directory(filesystemError)) continue;
					const auto candidate = NormalizeVcToolsDirectory(edition.path() / L"VC" / L"Tools" / L"MSVC");
					if (!candidate.empty()) candidates.push_back(candidate);
				}
			}
		}
		if (!candidates.empty()) {
			std::sort(candidates.begin(), candidates.end(), std::greater<>());
			vcTools = candidates.front();
		}
	}
	std::filesystem::path windowsSdkRoot;
	std::filesystem::path windowsKit;
	if (!configuredWindowsSdkDirectory.empty() &&
		!SelectWindowsSdk(configuredWindowsSdkDirectory, windowsSdkRoot, windowsKit)) {
		error = "windows_sdk_directory_invalid:" + PathToUtf8(AbsolutePath(configuredWindowsSdkDirectory));
		return false;
	}
	wchar_t sdkDirectory[MAX_PATH * 4]{};
	if (windowsKit.empty() && GetEnvironmentVariableW(L"WindowsSdkDir", sdkDirectory, std::size(sdkDirectory)) > 0) {
		SelectWindowsSdk(std::filesystem::path(sdkDirectory), windowsSdkRoot, windowsKit);
	}
	if (windowsKit.empty()) {
		for (const auto& candidate : {
			std::filesystem::path(L"C:\\Program Files (x86)\\Windows Kits\\10"),
			std::filesystem::path(L"C:\\Program Files\\Windows Kits\\10")
		}) {
			if (SelectWindowsSdk(candidate, windowsSdkRoot, windowsKit)) break;
		}
	}
	if (vcTools.empty() || windowsKit.empty()) {
		error = "visual_cpp_or_windows_sdk_not_found";
		return false;
	}
	const std::vector<std::filesystem::path> compilerCandidates = architecture == TargetArchitecture::X64
		? std::vector<std::filesystem::path> {
			vcTools / L"bin" / L"Hostx64" / L"x64" / L"cl.exe",
			vcTools / L"bin" / L"Hostx86" / L"x64" / L"cl.exe",
		}
		: std::vector<std::filesystem::path> {
			vcTools / L"bin" / L"Hostx86" / L"x86" / L"cl.exe",
			vcTools / L"bin" / L"Hostx64" / L"x86" / L"cl.exe",
		};
	for (const auto& candidate : compilerCandidates) {
		if (IsRegularFile(candidate)) {
			matchingCompiler = candidate;
			break;
		}
	}
	if (matchingCompiler.empty()) {
		error = std::string("matching_") +
			(architecture == TargetArchitecture::X64 ? "x64" : "x86") +
			"_compiler_not_found:" + PathToUtf8(vcTools);
		return false;
	}
	includeDirectories = {
		vcTools / L"include",
		windowsKit / L"ucrt",
		windowsKit / L"shared",
		windowsKit / L"um",
		windowsKit / L"winrt",
	};
	const std::filesystem::path kitVersion = windowsKit.filename();
	const std::filesystem::path kitLib = windowsSdkRoot / L"Lib" / kitVersion;
	const std::filesystem::path machineDirectory = architecture == TargetArchitecture::X64 ? L"x64" : L"x86";
	resourceCompiler = windowsSdkRoot / L"bin" / kitVersion / machineDirectory / L"rc.exe";
	libraryDirectories = {
		vcTools / L"lib" / machineDirectory,
		kitLib / L"ucrt" / machineDirectory,
		kitLib / L"um" / machineDirectory,
	};
	for (const auto& directory : includeDirectories) {
		if (!std::filesystem::is_directory(directory)) { error = "compiler_include_directory_not_found:" + PathToUtf8(directory); return false; }
	}
	for (const auto& directory : libraryDirectories) {
		if (!std::filesystem::is_directory(directory)) { error = "compiler_library_directory_not_found:" + PathToUtf8(directory); return false; }
	}
	return true;
}

bool WriteUtf8Source(const std::filesystem::path& path, const std::string& text, std::string& error)
{
	// CppEmitter is built with the legacy execution charset so that escaped
	// 易语言 text literals remain CP936 at runtime.  Its raw generated-runtime
	// snippets therefore also arrive here as CP936 bytes.  The generated file
	// is compiled as UTF-8, so normalize those bytes before writing; otherwise
	// wide literals such as L"目录" become mojibake (Ŀ¼) and the corresponding
	// properties are never applied.
	std::string utf8Text;
	if (!text.empty()) {
		const int wideLength = MultiByteToWideChar(936, MB_ERR_INVALID_CHARS,
			text.data(), static_cast<int>(text.size()), nullptr, 0);
		if (wideLength > 0) {
			std::wstring wide(static_cast<std::size_t>(wideLength), L'\0');
			MultiByteToWideChar(936, MB_ERR_INVALID_CHARS, text.data(),
				static_cast<int>(text.size()), wide.data(), wideLength);
			const int utf8Length = WideCharToMultiByte(CP_UTF8, 0, wide.data(),
				wideLength, nullptr, 0, nullptr, nullptr);
			if (utf8Length > 0) {
				utf8Text.resize(static_cast<std::size_t>(utf8Length));
				WideCharToMultiByte(CP_UTF8, 0, wide.data(), wideLength,
					utf8Text.data(), utf8Length, nullptr, nullptr);
			}
		}
	}
	if (utf8Text.empty() && !text.empty()) utf8Text = text;
	std::ofstream output(path, std::ios::binary | std::ios::trunc);
	if (!output) {
		error = "open_generated_source_failed:" + PathToUtf8(path);
		return false;
	}
	output.write("\xEF\xBB\xBF", 3);
	for (std::size_t index = 0; index < utf8Text.size(); ++index) {
		if (utf8Text[index] == '\n' && (index == 0 || utf8Text[index - 1] != '\r')) output.put('\r');
		output.put(utf8Text[index]);
	}
	if (!output.good()) {
		error = "write_generated_source_failed:" + PathToUtf8(path);
		return false;
	}
	return true;
}

bool WriteTextFile(const std::filesystem::path& path, const std::string& text, std::string& error)
{
	std::ofstream output(path, std::ios::binary | std::ios::trunc);
	if (!output) {
		error = "open_generated_definition_failed:" + PathToUtf8(path);
		return false;
	}
	output.write(text.data(), static_cast<std::streamsize>(text.size()));
	if (!output.good()) {
		error = "write_generated_definition_failed:" + PathToUtf8(path);
		return false;
	}
	return true;
}

std::string DefQuotedName(const std::string& value)
{
	std::string result = value;
	for (char& character : result) if (character == '"') character = '_';
	return result;
}

bool WriteDllDefinition(
	const std::filesystem::path& path,
	const std::string& libraryName,
	const std::vector<GeneratedSource::ExportedFunction>& exports,
	const TargetArchitecture architecture,
	std::string& error)
{
	std::ostringstream text;
	text << "LIBRARY \"" << DefQuotedName(libraryName) << "\"\r\nEXPORTS\r\n";
	for (const auto& item : exports) {
		if (item.name.empty() || item.symbol.empty()) continue;
		const std::string decorated = architecture == TargetArchitecture::X64 || item.usesCdecl
			? item.symbol
			: ("_" + item.symbol + "@" + std::to_string(item.stackBytes));
		text << "    " << item.name << "=" << decorated << "\r\n";
	}
	return WriteTextFile(path, text.str(), error);
}

bool WriteImportDefinition(
	const std::filesystem::path& path,
	const GeneratedSource::ImportedFunction& item,
	const TargetArchitecture architecture,
	std::string& error)
{
	std::ostringstream text;
	text << "LIBRARY \"" << DefQuotedName(item.moduleName) << "\"\r\nEXPORTS\r\n";
	(void)architecture;
	(void)item.usesCdecl;
	(void)item.stackBytes;
	// LIB.EXE treats the left-hand side of a DEF entry as the name that
	// Windows stores in the PE import table.  The generated C++ symbol is
	// intentionally different, so the emitter adds an /alternatename mapping
	// to the real symbol.  Emitting the local alias here would make the loader
	// search for ecompiler_import_* in the target DLL.
	text << "    " << item.entryName << "\r\n";
	return WriteTextFile(path, text.str(), error);
}

bool ReadTextFile(const std::filesystem::path& path, std::string& text)
{
	std::ifstream input(path, std::ios::binary);
	if (!input) return false;
	std::ostringstream content;
	content << input.rdbuf();
	text = content.str();
	return true;
}

bool RunProcess(
	const std::filesystem::path& executable,
	const std::vector<std::wstring>& arguments,
	const std::filesystem::path& workingDirectory,
	const std::filesystem::path& logPath,
	std::string& output,
	std::string& error,
	const std::filesystem::path& additionalPath = {})
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
		error = "create_process_log_failed:" + std::to_string(GetLastError());
		return false;
	}
	SetHandleInformation(log, HANDLE_FLAG_INHERIT, HANDLE_FLAG_INHERIT);
	STARTUPINFOW startup {};
	startup.cb = sizeof(startup);
	startup.dwFlags = STARTF_USESTDHANDLES;
	startup.hStdOutput = log;
	startup.hStdError = log;
	startup.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
	PROCESS_INFORMATION process {};
	std::vector<wchar_t> mutableCommand(command.begin(), command.end());
	mutableCommand.push_back(L'\0');
	std::vector<wchar_t> environment;
	if (!additionalPath.empty()) {
		LPWCH current = GetEnvironmentStringsW();
		if (current != nullptr) {
			bool pathFound = false;
			for (const wchar_t* item = current; *item != L'\0'; item += std::wcslen(item) + 1) {
				std::wstring entry(item);
				if (entry.size() >= 5 && _wcsnicmp(entry.c_str(), L"PATH=", 5) == 0) {
					entry = L"PATH=" + additionalPath.wstring() + L";" + entry.substr(5);
					pathFound = true;
				}
				environment.insert(environment.end(), entry.begin(), entry.end());
				environment.push_back(L'\0');
			}
			if (!pathFound) {
				const std::wstring entry = L"PATH=" + additionalPath.wstring();
				environment.insert(environment.end(), entry.begin(), entry.end());
				environment.push_back(L'\0');
			}
			environment.push_back(L'\0');
			FreeEnvironmentStringsW(current);
		}
	}
	const BOOL started = CreateProcessW(
		executable.c_str(), mutableCommand.data(), nullptr, nullptr, TRUE,
		CREATE_NO_WINDOW | (environment.empty() ? 0u : CREATE_UNICODE_ENVIRONMENT),
		environment.empty() ? nullptr : environment.data(), workingDirectory.c_str(), &startup, &process);
	if (!started) {
		const DWORD code = GetLastError();
		CloseHandle(log);
		error = "start_process_failed:" + PathToUtf8(executable) + ":" + std::to_string(code);
		return false;
	}
	WaitForSingleObject(process.hProcess, INFINITE);
	DWORD exitCode = 1;
	GetExitCodeProcess(process.hProcess, &exitCode);
	CloseHandle(process.hThread);
	CloseHandle(process.hProcess);
	FlushFileBuffers(log);
	CloseHandle(log);
	ReadTextFile(logPath, output);
	std::error_code ignored;
	std::filesystem::remove(logPath, ignored);
	if (exitCode != 0) {
		error = "process_failed:" + PathToUtf8(executable) + ":exit=" + std::to_string(exitCode);
		if (!output.empty()) error += "\n" + output;
		return false;
	}
	return true;
}

bool ReadPeMachine(
	const std::filesystem::path& path,
	std::uint16_t& machine)
{
	machine = 0;
	std::ifstream input(path, std::ios::binary);
	if (!input) return false;
	std::uint16_t dosMagic = 0;
	std::int32_t peOffset = 0;
	input.read(reinterpret_cast<char*>(&dosMagic), sizeof(dosMagic));
	input.seekg(0x3C, std::ios::beg);
	input.read(reinterpret_cast<char*>(&peOffset), sizeof(peOffset));
	if (!input || dosMagic != 0x5A4D || peOffset < 0 || peOffset > 0x1000000) return false;
	input.seekg(peOffset + 4, std::ios::beg);
	input.read(reinterpret_cast<char*>(&machine), sizeof(machine));
	return input.good();
}

void AppendPathCandidate(
	std::vector<std::filesystem::path>& candidates,
	const std::filesystem::path& candidate)
{
	if (candidate.empty()) return;
	const std::filesystem::path normalized = AbsolutePath(candidate).lexically_normal();
	for (const auto& existing : candidates) {
		if (existing.lexically_normal() == normalized) return;
	}
	candidates.push_back(normalized);
}

std::filesystem::path ResolveX86DecoderPath(const Options& options)
{
	std::vector<std::filesystem::path> candidates;
	AppendPathCandidate(candidates, options.x86DecoderPath);
	wchar_t configured[MAX_PATH * 4] {};
	if (GetEnvironmentVariableW(L"E_PACKAGER_X86_DECODER", configured, std::size(configured)) > 0) {
		AppendPathCandidate(candidates, std::filesystem::path(configured));
	}

	const std::filesystem::path executableDirectory = Utf8PathToPath(GetBasePath());
	const std::filesystem::path binDirectory = executableDirectory.parent_path().parent_path();
	AppendPathCandidate(candidates, binDirectory / L"Win32" / L"Release" / L"e-packager.exe");
	AppendPathCandidate(candidates, binDirectory / L"Win32" / L"Debug" / L"e-packager.exe");
	AppendPathCandidate(candidates, binDirectory / L"Win32" / L"e-packager.exe");
	const std::filesystem::path currentDirectory = std::filesystem::current_path();
	AppendPathCandidate(candidates, currentDirectory / L"bin" / L"Win32" / L"Release" / L"e-packager.exe");
	AppendPathCandidate(candidates, currentDirectory / L"e-packager.exe");

	for (const auto& candidate : candidates) {
		if (!IsRegularFile(candidate)) continue;
		std::uint16_t machine = 0;
		if (ReadPeMachine(candidate, machine) && machine == 0x014C) return candidate;
	}
	return {};
}

bool CreateTemporaryDirectory(
	std::filesystem::path& outDirectory,
	std::string& error)
{
	outDirectory.clear();
	wchar_t temporaryPath[MAX_PATH * 4] {};
	const DWORD length = GetTempPathW(static_cast<DWORD>(std::size(temporaryPath)), temporaryPath);
	if (length == 0 || length >= std::size(temporaryPath)) {
		error = "x64_source_decode_temp_path_failed:" + std::to_string(GetLastError());
		return false;
	}
	const std::filesystem::path root(temporaryPath);
	const std::wstring prefix = L"e-packager-x86-decode-" +
		std::to_wstring(GetCurrentProcessId()) + L"-" +
		std::to_wstring(GetTickCount64());
	for (unsigned int attempt = 0; attempt < 100; ++attempt) {
		const std::filesystem::path candidate = root / (prefix + L"-" + std::to_wstring(attempt));
		std::error_code ec;
		if (std::filesystem::create_directory(candidate, ec)) {
			outDirectory = candidate;
			return true;
		}
		if (ec && ec != std::errc::file_exists) {
			error = "x64_source_decode_temp_directory_failed:" + ec.message();
			return false;
		}
	}
	error = "x64_source_decode_temp_directory_collision";
	return false;
}

class TemporaryDirectoryGuard {
public:
	TemporaryDirectoryGuard() = default;
	explicit TemporaryDirectoryGuard(std::filesystem::path path) : path_(std::move(path)) {}
	TemporaryDirectoryGuard(const TemporaryDirectoryGuard&) = delete;
	TemporaryDirectoryGuard& operator=(const TemporaryDirectoryGuard&) = delete;
	TemporaryDirectoryGuard(TemporaryDirectoryGuard&& other) noexcept : path_(std::move(other.path_)) {}
	TemporaryDirectoryGuard& operator=(TemporaryDirectoryGuard&& other) noexcept {
		if (this != &other) {
			Reset();
			path_ = std::move(other.path_);
		}
		return *this;
	}
	~TemporaryDirectoryGuard() { Reset(); }

	const std::filesystem::path& path() const { return path_; }
	void Reset() {
		if (!path_.empty()) {
			std::error_code ignored;
			std::filesystem::remove_all(path_, ignored);
			path_.clear();
		}
	}

private:
	std::filesystem::path path_;
};

bool DecodeSourceWithX86Helper(
	const std::filesystem::path& inputPath,
	const Options& options,
	TemporaryDirectoryGuard& outDirectory,
	std::string& error)
{
	const std::filesystem::path decoder = ResolveX86DecoderPath(options);
	if (decoder.empty()) {
		error = "semantic_source_decoder_not_found:provide --x86-decoder or E_PACKAGER_X86_DECODER";
		return false;
	}
	std::filesystem::path directory;
	if (!CreateTemporaryDirectory(directory, error)) return false;
	outDirectory = TemporaryDirectoryGuard(directory);
	std::string processOutput;
	if (!RunProcess(
		decoder,
		{L"unpack", Quote(AbsolutePath(inputPath)), Quote(directory)},
		decoder.parent_path(),
		directory / L"decode.log",
		processOutput,
		error)) {
		error = "semantic_source_decode_failed:" + error;
		return false;
	}
	if (!IsRegularFile(directory / L"project" / L"_meta.json") ||
		!IsRegularFile(directory / L"project" / L".module.json")) {
		error = "semantic_source_decode_output_invalid:" + PathToUtf8(directory);
		return false;
	}
	return true;
}

bool ReadInputBundle(
	const std::filesystem::path& inputPath,
	e2txt::ProjectBundle& bundle,
	std::filesystem::path& inputRoot,
	const e2txt::ReadOptions& readOptions,
	std::string& error)
{
	std::error_code filesystemError;
	if (std::filesystem::is_directory(inputPath, filesystemError)) {
		e2txt::BundleDirectoryCodec codec;
		if (!codec.ReadBundle(PathToUtf8(inputPath), bundle, &error)) {
			error = "read_bundle_failed:" + error;
			return false;
		}
		inputRoot = inputPath;
		return true;
	}
	filesystemError.clear();
	if (!std::filesystem::is_regular_file(inputPath, filesystemError)) {
		error = "compile_input_not_found:" + PathToUtf8(inputPath);
		return false;
	}
	if (inputPath.extension() != L".e") {
		error = "compile_input_must_be_e_or_directory:" + PathToUtf8(inputPath);
		return false;
	}
	e2txt::Generator generator;
	if (!generator.GenerateBundle(PathToUtf8(inputPath), bundle, &error, readOptions)) {
		error = "read_e_project_failed:" + error;
		return false;
	}
	inputRoot = inputPath.parent_path();
	return true;
}

std::filesystem::path ProductRootFromCompiler(const std::filesystem::path& compiler)
{
	std::filesystem::path current = compiler;
	for (int index = 0; index < 4 && current.has_parent_path(); ++index) current = current.parent_path();
	return current;
}

std::filesystem::path FindStaticLibrary(
	const std::filesystem::path& directory,
	const Library& library,
	const TargetArchitecture architecture)
{
	if (directory.empty()) return {};
	std::vector<std::wstring> candidateNames;
	if (library.dependency.fileName == "krnln") {
		candidateNames.push_back(L"krnln_static.lib");
		candidateNames.push_back(L"krnln.lib");
		candidateNames.push_back(L"krnln_test.lib");
	}
	const std::filesystem::path metadataStem = library.metadata.filePath.stem();
	if (!metadataStem.empty()) {
		candidateNames.push_back(metadataStem.wstring() + L"_static.lib");
		candidateNames.push_back(metadataStem.wstring() + L".lib");
	}
	if (!library.dependency.fileName.empty()) {
		const std::filesystem::path dependencyPath = Utf8PathToPath(library.dependency.fileName);
		const std::wstring dependencyStem = dependencyPath.stem().wstring();
		if (!dependencyStem.empty()) {
			candidateNames.push_back(dependencyStem + L"_static.lib");
			candidateNames.push_back(dependencyStem + L".lib");
		}
	}
	for (const std::wstring& name : candidateNames) {
		const std::filesystem::path candidate = directory / name;
		if (IsRegularFile(candidate) && IsStaticLibraryCompatible(candidate, architecture)) return candidate;
	}
	std::error_code error;
	for (const auto& entry : std::filesystem::directory_iterator(directory, error)) {
		if (!entry.is_regular_file(error)) continue;
		std::wstring fileName = entry.path().filename().wstring();
		std::transform(fileName.begin(), fileName.end(), fileName.begin(), towlower);
		for (std::wstring expected : candidateNames) {
			std::transform(expected.begin(), expected.end(), expected.begin(), towlower);
			if (fileName == expected && IsStaticLibraryCompatible(entry.path(), architecture)) return entry.path();
		}
	}
	return {};
}

std::filesystem::path FindLibraryArtifact(
	const std::vector<std::filesystem::path>& directories,
	const std::string& requestedName)
{
	const std::filesystem::path requested = Utf8PathToPath(requestedName);
	if (requested.empty()) return {};
	if (requested.is_absolute() && IsRegularFile(requested)) return requested;
	std::vector<std::wstring> candidates;
	const std::wstring stem = requested.stem().wstring();
	const std::wstring extension = requested.extension().wstring();
	if (!extension.empty()) candidates.push_back(requested.filename().wstring());
	if (!stem.empty()) {
		candidates.push_back(stem + L"_static.lib");
		candidates.push_back(stem + L".lib");
	}
	for (const auto& directory : directories) {
		for (const auto& candidateName : candidates) {
			const auto candidate = directory / candidateName;
			if (IsRegularFile(candidate)) return candidate;
		}
	}
	for (const auto& directory : directories) {
		std::error_code error;
		for (const auto& entry : std::filesystem::directory_iterator(directory, error)) {
			if (!entry.is_regular_file(error)) continue;
			std::wstring actual = entry.path().filename().wstring();
			std::transform(actual.begin(), actual.end(), actual.begin(), towlower);
			for (std::wstring expected : candidates) {
				std::transform(expected.begin(), expected.end(), expected.begin(), towlower);
				if (actual == expected) return entry.path();
			}
		}
	}
	return {};
}

void AppendUniquePath(std::vector<std::filesystem::path>& paths, const std::filesystem::path& path)
{
	if (path.empty()) return;
	for (const auto& existing : paths) if (existing.lexically_normal() == path.lexically_normal()) return;
	paths.push_back(path);
}

void AppendSupportLibraryDirectories(
	std::vector<std::filesystem::path>& paths,
	const std::filesystem::path& configuredPath,
	const TargetArchitecture architecture)
{
	if (configuredPath.empty()) return;
	std::error_code error;
	std::filesystem::path root = AbsolutePath(configuredPath);
	if (std::filesystem::is_regular_file(root, error)) root = root.parent_path();
	if (root.empty()) return;

	const std::filesystem::path architectureName =
		architecture == TargetArchitecture::X64 ? L"x64" : L"x86";
	auto appendDirectory = [&](const std::filesystem::path& directory) {
		error.clear();
		if (std::filesystem::is_directory(directory, error)) AppendUniquePath(paths, directory);
	};

	appendDirectory(root);
	appendDirectory(root / architectureName);
	std::filesystem::path installRoot = root;
	std::wstring leaf = root.filename().wstring();
	std::transform(leaf.begin(), leaf.end(), leaf.begin(), towlower);
	if (leaf == L"lib" || leaf == L"static_lib") installRoot = root.parent_path();
	for (const auto& directoryName : { L"lib", L"static_lib" }) {
		const std::filesystem::path directory = installRoot / directoryName;
		appendDirectory(directory);
		appendDirectory(directory / architectureName);
	}
}

std::filesystem::path ReadEnvironmentPath(const wchar_t* name)
{
	const DWORD required = GetEnvironmentVariableW(name, nullptr, 0);
	if (required <= 1) return {};
	std::wstring value(static_cast<std::size_t>(required), L'\0');
	const DWORD written = GetEnvironmentVariableW(name, value.data(), required);
	if (written == 0 || written >= required) return {};
	value.resize(written);
	return std::filesystem::path(value);
}

void AppendDiscoveredELibraryDirectories(
	std::vector<std::filesystem::path>& paths,
	const Options& options,
	const TargetArchitecture architecture)
{
	// 易语言第三方支持库目前只提供 x86 FNE/静态实现；x64 语义编译
	// 仅使用匹配架构的 BlackMoonModernCore adapter。
	if (architecture == TargetArchitecture::X64) return;
	if (!options.eDirectory.empty()) {
		AppendSupportLibraryDirectories(paths, options.eDirectory, architecture);
		return;
	}
	// 省略显式目录时，只使用机器级自动发现来源。
	AppendSupportLibraryDirectories(paths, ReadEnvironmentPath(L"E_PACKAGER_EIDE"), architecture);
	for (const auto& baseDirectory : GetRegisteredEplOpenCommandBaseDirs()) {
		AppendSupportLibraryDirectories(paths, baseDirectory, architecture);
	}
}

std::filesystem::path FindStaticLibraryInDirectories(
	const std::vector<std::filesystem::path>& directories,
	const Library& library,
	const TargetArchitecture architecture)
{
	for (const auto& directory : directories) {
		const std::filesystem::path archive = FindStaticLibrary(directory, library, architecture);
		if (!archive.empty()) return archive;
	}
	return {};
}

struct CoreLibraryRoots {
	std::filesystem::path staticLibraryDirectory;
	std::filesystem::path metadataDirectory;
	CoreArchiveSelection coreArchive;
	std::vector<std::filesystem::path> searchDirectories;
};

void AddRootCandidates(
	std::vector<std::filesystem::path>& roots,
	const std::filesystem::path& root,
	const TargetArchitecture architecture)
{
	if (root.empty()) return;
	if (IsRegularFile(root)) {
		AppendUniquePath(roots, root.parent_path());
		return;
	}
	const std::filesystem::path architectureDirectory =
		architecture == TargetArchitecture::X64 ? L"x64" : L"x86";
	// Accept either a product root or a directly supplied lib/<arch>/
	// static_lib/<arch> directory. Walking a few parents keeps discovery
	// deterministic while covering release, product, and bin/<arch>/ layouts.
	std::filesystem::path base = root;
	for (unsigned int depth = 0; depth < 5 && !base.empty(); ++depth) {
		AppendUniquePath(roots, base);
		AppendUniquePath(roots, base / L"static_lib" / architectureDirectory);
		AppendUniquePath(roots, base / L"lib" / architectureDirectory);
		AppendUniquePath(roots, base / L"static_lib");
		AppendUniquePath(roots, base / L"lib");
		AppendUniquePath(roots, base / architectureDirectory);
		const std::filesystem::path parent = base.parent_path();
		if (parent == base) break;
		base = parent;
	}
}

std::filesystem::path FindCoreStaticArchive(
	const std::filesystem::path& directory,
	const TargetArchitecture architecture)
{
	std::error_code directoryError;
	if (directory.empty() || !std::filesystem::is_directory(directory, directoryError)) return {};
	static constexpr const wchar_t* names[] = {
		L"krnln_static.lib", L"krnln.lib", L"krnln_test.lib",
	};
	for (const wchar_t* name : names) {
		const std::filesystem::path candidate = directory / name;
		if (IsRegularFile(candidate) && IsStaticLibraryCompatible(candidate, architecture)) return candidate;
	}
	return {};
}

CoreLibraryRoots ResolveCoreLibraryRoots(
	const Options& options,
	const TargetArchitecture architecture)
{
	std::vector<std::filesystem::path> roots;
	for (const auto& directory : options.blackMoonCoreDirectories) {
		AddRootCandidates(roots, AbsolutePath(directory), architecture);
	}
	const auto& architectureDirectories = architecture == TargetArchitecture::X64
		? options.blackMoonX64Directories : options.blackMoonX86Directories;
	for (const auto& directory : architectureDirectories) {
		AddRootCandidates(roots, AbsolutePath(directory), architecture);
	}
	wchar_t configured[MAX_PATH * 4]{};
	if (GetEnvironmentVariableW(L"E_PACKAGER_BLACKMOON_CORE_DIR", configured, std::size(configured)) > 0) {
		AddRootCandidates(roots, AbsolutePath(std::filesystem::path(configured)), architecture);
	}
	const wchar_t* architectureEnvironment = architecture == TargetArchitecture::X64
		? L"E_PACKAGER_BLACKMOON_X64_DIR" : L"E_PACKAGER_BLACKMOON_X86_DIR";
	if (GetEnvironmentVariableW(architectureEnvironment, configured, std::size(configured)) > 0) {
		AddRootCandidates(roots, AbsolutePath(std::filesystem::path(configured)), architecture);
	}
	AddRootCandidates(roots, Utf8PathToPath(GetBasePath()), architecture);

	CoreLibraryRoots result;
	for (const auto& root : roots) {
		if (result.staticLibraryDirectory.empty()) {
			CoreArchiveSelection adapterArchive;
			if (ReadBlackMoonAdapterSelection(root, architecture, adapterArchive)) {
				result.staticLibraryDirectory = adapterArchive.primaryArchive.parent_path();
				result.coreArchive = std::move(adapterArchive);
			}
			else {
				const auto coreArchive = FindCoreStaticArchive(root, architecture);
				if (!coreArchive.empty()) result.staticLibraryDirectory = coreArchive.parent_path();
			}
		}
		if (result.metadataDirectory.empty() && IsRegularFile(root / L"krnln.fne")) {
			result.metadataDirectory = root;
		}
	}
	for (const auto& root : roots) {
		CoreArchiveSelection adapterArchive;
		if (ReadBlackMoonAdapterSelection(root, architecture, adapterArchive)) {
			AppendUniquePath(result.searchDirectories, root);
		}
		if (!FindCoreStaticArchive(root, architecture).empty()) AppendUniquePath(result.searchDirectories, root);
		if (IsRegularFile(root / L"krnln.fne")) AppendUniquePath(result.searchDirectories, root);
	}
	return result;
}

std::filesystem::path FindLibraryManager(
	const std::filesystem::path& compiler,
	const std::filesystem::path& linker)
{
	const std::vector<std::filesystem::path> candidates = {
		linker.parent_path() / L"lib.exe",
		compiler.parent_path() / L"lib.exe",
	};
	for (const auto& candidate : candidates) if (IsRegularFile(candidate)) return candidate;
	return {};
}

TargetArchitecture HostTargetArchitecture()
{
#if defined(_M_X64)
	return TargetArchitecture::X64;
#else
	return TargetArchitecture::X86;
#endif
}

bool ResolveToolchain(
	const Options& options,
	const TargetArchitecture architecture,
	std::filesystem::path& compiler,
	std::filesystem::path& linker,
	std::filesystem::path& resourceCompiler,
	std::filesystem::path& vcLibrary,
	std::filesystem::path& productRoot,
	std::vector<std::filesystem::path>* outIncludeDirectories,
	std::vector<std::filesystem::path>* outSystemLibraryDirectories,
	std::string& error)
{
	if (outIncludeDirectories != nullptr) outIncludeDirectories->clear();
	if (outSystemLibraryDirectories != nullptr) outSystemLibraryDirectories->clear();
	std::vector<std::filesystem::path> includeDirectories;
	std::vector<std::filesystem::path> systemLibraryDirectories;
	std::filesystem::path discoveredCompiler;
	if (!DiscoverBuildEnvironment(
			architecture, options.vcToolsDirectory, options.windowsSdkDirectory,
			discoveredCompiler, resourceCompiler, includeDirectories, systemLibraryDirectories, error)) {
		return false;
	}
	compiler = options.compilerPath.empty() ? discoveredCompiler : options.compilerPath;
	if (outIncludeDirectories != nullptr) *outIncludeDirectories = includeDirectories;
	if (outSystemLibraryDirectories != nullptr) *outSystemLibraryDirectories = systemLibraryDirectories;
	linker = options.linkerPath.empty() ? discoveredCompiler.parent_path() / L"link.exe" : options.linkerPath;
	productRoot = ProductRootFromCompiler(discoveredCompiler);
	vcLibrary = systemLibraryDirectories.front();
	if (!IsRegularFile(compiler)) { error = "compiler_not_found:" + PathToUtf8(compiler); return false; }
	if (!IsRegularFile(linker)) { error = "linker_not_found:" + PathToUtf8(linker); return false; }
	if (!std::filesystem::is_directory(vcLibrary)) { error = "linker_library_directory_not_found:" + PathToUtf8(vcLibrary); return false; }
	return true;
}

}  // namespace

bool Compile(
	const std::filesystem::path& inputPath,
	const std::filesystem::path& outputPath,
	const Options& options,
	Result& result)
{
	const TargetArchitecture targetArchitecture = options.targetArchitecture == TargetArchitecture::Host
		? HostTargetArchitecture() : options.targetArchitecture;
	const bool targetX64 = targetArchitecture == TargetArchitecture::X64;
	CompileMode effectiveCompileMode = options.compileMode;
	if (effectiveCompileMode == CompileMode::BlackMoonCompatibility) {
		effectiveCompileMode = targetArchitecture == TargetArchitecture::X86
			? CompileMode::LegacyBlackMoon : CompileMode::Semantic;
	}
	if (effectiveCompileMode == CompileMode::LegacyBlackMoon) {
		if (targetArchitecture != TargetArchitecture::X86) {
			result = {};
			result.outputPath = outputPath;
			result.message = "legacy_blackmoon_requires_x86";
			return false;
		}
		if (!options.vcToolsDirectory.empty() || !options.windowsSdkDirectory.empty() ||
			!options.compilerPath.empty() || !options.linkerPath.empty()) {
			result = {};
			result.outputPath = outputPath;
			result.message = "legacy_blackmoon_uses_legacy_blackmoon_linker_and_ignores_vc_toolchain_options";
			return false;
		}
		return blackmoon_compiler::Compile(inputPath, outputPath, options, result);
	}
	result = {};
	result.outputPath = outputPath;
	std::string error;
	if (!options.legacyBlackMoonLinkerPath.empty()) {
		result.message = "semantic_does_not_use_legacy_blackmoon_linker";
		return false;
	}
	if (!options.eDirectory.empty()) {
		std::error_code directoryError;
		const std::filesystem::path directory = AbsolutePath(options.eDirectory);
		if (!std::filesystem::is_directory(directory, directoryError)) {
			result.message = "e_language_directory_not_found:" + PathToUtf8(directory);
			return false;
		}
	}
	std::filesystem::path compiler;
	std::filesystem::path linker;
	std::filesystem::path resourceCompiler;
	std::filesystem::path vcLibrary;
	std::filesystem::path productRoot;
	std::vector<std::filesystem::path> includeDirectories;
	std::vector<std::filesystem::path> systemLibraryDirectories;
	if (!ResolveToolchain(
		options, targetArchitecture, compiler, linker, resourceCompiler, vcLibrary, productRoot,
		&includeDirectories, &systemLibraryDirectories, error)) {
		result.message = error;
		return false;
	}
	CoreLibraryRoots coreRoots = ResolveCoreLibraryRoots(options, targetArchitecture);
	const bool usesModernCoreAdapter = coreRoots.coreArchive.adapter;
	std::vector<std::filesystem::path> supportLibrarySearchDirectories;
	supportLibrarySearchDirectories = coreRoots.searchDirectories;
	if (!coreRoots.staticLibraryDirectory.empty()) {
		AppendUniquePath(supportLibrarySearchDirectories, coreRoots.staticLibraryDirectory);
	}
	if (!coreRoots.metadataDirectory.empty()) {
		AppendUniquePath(supportLibrarySearchDirectories, coreRoots.metadataDirectory);
	}
	if (targetArchitecture == TargetArchitecture::X64 && coreRoots.staticLibraryDirectory.empty()) {
		result.message = "semantic_core_library_not_found:provide --blackmoon-core-dir";
		return false;
	}
	AppendDiscoveredELibraryDirectories(
		supportLibrarySearchDirectories, options, targetArchitecture);
	std::filesystem::path bundleInput = inputPath;
	TemporaryDirectoryGuard sourceDecoderDirectory;
	std::error_code inputTypeError;
	if (std::filesystem::is_regular_file(inputPath, inputTypeError) &&
		inputPath.extension() == L".e") {
		if (!DecodeSourceWithX86Helper(inputPath, options, sourceDecoderDirectory, error)) {
			result.message = error;
			return false;
		}
		bundleInput = sourceDecoderDirectory.path();
	}
	e2txt::ProjectBundle bundle;
	std::filesystem::path inputRoot;
	e2txt::ReadOptions readOptions;
	readOptions.supportLibrarySearchDirectories = supportLibrarySearchDirectories;
	readOptions.restrictSupportLibrarySearch = targetArchitecture == TargetArchitecture::X64 || usesModernCoreAdapter;
	if (!ReadInputBundle(bundleInput, bundle, inputRoot, readOptions, error)) {
		result.message = error;
		return false;
	}
	Program program;
	if (!BuildCompilerModel(
		std::move(bundle), inputRoot, supportLibrarySearchDirectories, options.conditionMacros,
		targetArchitecture, readOptions.restrictSupportLibrarySearch, program, error)) {
		result.message = "compiler_model_failed:" + error;
		return false;
	}
	program.buildDll = options.buildDll || outputPath.extension() == L".dll";
	if (program.buildDll) {
		program.windowsGui = true;
	}
	else if (options.subsystem == ExecutableSubsystem::WindowsGui) {
		program.windowsGui = true;
	}
	else if (options.subsystem == ExecutableSubsystem::Console) {
		program.windowsGui = false;
	}
	else {
		program.windowsGui = program.bundle.projectSubsystem == e2txt::ProjectSubsystem::WindowsGui;
	}
	program.useLegacyX86RuntimeBridge = !targetX64 && !usesModernCoreAdapter;
	GeneratedSource generated;
	if (!EmitCppSource(program, generated, error)) {
		result.message = "source_generation_failed:" + error;
		return false;
	}
	std::error_code filesystemError;
	const std::filesystem::path outputDirectory = outputPath.has_parent_path() ? outputPath.parent_path() : std::filesystem::current_path();
	std::filesystem::create_directories(outputDirectory, filesystemError);
	if (filesystemError) {
		result.message = "create_output_directory_failed:" + filesystemError.message();
		return false;
	}
	result.sourcePath = outputPath;
	result.sourcePath.replace_extension(L".generated.cpp");
	result.objectPath = outputPath;
	result.objectPath.replace_extension(L".obj");
	if (!WriteUtf8Source(result.sourcePath, generated.text, error)) {
		result.message = error;
		return false;
	}
	const std::filesystem::path logPath = outputDirectory / (outputPath.stem().wstring() + L".compile.log");
	std::string processOutput;
	std::vector<std::filesystem::path> generatedImportLibraries;
	const std::filesystem::path libraryManager = FindLibraryManager(compiler, linker);
	for (const auto& import : generated.imports) {
		if (import.moduleName.empty() || import.entryName.empty()) continue;
		const std::filesystem::path importDef = outputDirectory /
			(outputPath.stem().wstring() + L".import." + std::to_wstring(import.commandIndex) + L".def");
		const std::filesystem::path importLib = outputDirectory /
			(outputPath.stem().wstring() + L".import." + std::to_wstring(import.commandIndex) + L".lib");
		if (!WriteImportDefinition(importDef, import, targetArchitecture, error)) {
			result.message = error;
			return false;
		}
		generatedImportLibraries.push_back(importLib);
		const std::filesystem::path importLog = outputDirectory /
			(outputPath.stem().wstring() + L".import." + std::to_wstring(import.commandIndex) + L".log");
		std::string importOutput;
		if (!IsRegularFile(libraryManager) || !RunProcess(libraryManager,
			{L"/NOLOGO", targetX64 ? L"/MACHINE:X64" : L"/MACHINE:I386", L"/DEF:" + Quote(importDef), L"/OUT:" + Quote(importLib)},
			outputDirectory, importLog, importOutput, error)) {
			result.message = "generate_import_library_failed:" + import.moduleName + ":" + error;
			return false;
		}
	}
	const std::filesystem::path modernRuntimeLibrary = systemLibraryDirectories.front() / L"libcmt.lib";
	const std::filesystem::path modernCppRuntimeLibrary = systemLibraryDirectories.front() / L"libcpmt.lib";
	const std::filesystem::path modernVcruntimeLibrary = systemLibraryDirectories.front() / L"libvcruntime.lib";
	const std::filesystem::path modernUcrtLibrary = systemLibraryDirectories.at(1) / L"libucrt.lib";
	for (const auto& required : { modernRuntimeLibrary, modernCppRuntimeLibrary, modernVcruntimeLibrary, modernUcrtLibrary }) {
		if (!IsRegularFile(required)) {
			result.message = "modern_runtime_library_not_found:" + PathToUtf8(required);
			return false;
		}
	}
	std::vector<std::wstring> compilerArguments = {
		L"/nologo", L"/c", L"/O2", L"/Gy", L"/Zl", L"/GS-", L"/GR-", L"/EHsc", L"/MT", L"/std:c++20",
		L"/source-charset:utf-8", L"/execution-charset:.936", L"/Fo" + Quote(result.objectPath), Quote(result.sourcePath),
	};
	if (!targetX64) compilerArguments.insert(compilerArguments.begin() + 8, L"/arch:IA32");
	for (const auto& directory : includeDirectories) compilerArguments.push_back(L"/I" + Quote(directory));
	if (!RunProcess(compiler, compilerArguments, outputDirectory, logPath, processOutput, error)) {
		result.message = error;
		return false;
	}
	const std::filesystem::path staticLibrary = coreRoots.staticLibraryDirectory.empty()
		? productRoot / L"static_lib" : coreRoots.staticLibraryDirectory;
	const bool useLegacyX86Runtime = !targetX64 && !usesModernCoreAdapter;
	std::filesystem::path vc6RuntimeLibrary = productRoot / L"linker" / L"VC6linker" / L"Lib" / L"MSVCRT.LIB";
	std::filesystem::path mfcLibrary = vcLibrary / L"NAFXCW.LIB";
	if (useLegacyX86Runtime && !options.eDirectory.empty()) {
		const std::filesystem::path eRoot = AbsolutePath(options.eDirectory);
		const std::vector<std::pair<std::filesystem::path, std::filesystem::path>> legacyCandidates = {
			{eRoot / L"linker" / L"VC6linker" / L"Lib" / L"MSVCRT.LIB", eRoot / L"linker" / L"VC6linker" / L"MFC" / L"Lib" / L"NAFXCW.LIB"},
			{eRoot / L"linker" / L"VC2022Linker" / L"lib" / L"msvcrt.lib", eRoot / L"linker" / L"VC2022Linker" / L"lib" / L"NAFXCW.LIB"},
			{eRoot / L"BlackMoon" / L"lib" / L"MSVCRT.LIB", eRoot / L"BlackMoon" / L"lib" / L"NAFXCW.LIB"},
		};
		for (const auto& [runtimeCandidate, mfcCandidate] : legacyCandidates) {
			if (IsRegularFile(runtimeCandidate) && IsRegularFile(mfcCandidate)) { vc6RuntimeLibrary = runtimeCandidate; mfcLibrary = mfcCandidate; break; }
		}
	}
	if (useLegacyX86Runtime) {
		for (const std::filesystem::path& required : { mfcLibrary }) {
			if (!IsRegularFile(required)) {
				result.message = "mfc_runtime_file_not_found:" + PathToUtf8(required);
				return false;
			}
		}
		if (!IsRegularFile(vc6RuntimeLibrary)) {
			result.message = "vc6_runtime_library_not_found:" + PathToUtf8(vc6RuntimeLibrary);
			return false;
		}
	}
	std::vector<std::filesystem::path> supportLibraries;
	std::vector<std::filesystem::path> dependencyDirectories = {
		staticLibrary, vcLibrary, program.inputRoot, program.inputRoot / L"elib",
	};
	if (!options.eDirectory.empty()) {
		const std::filesystem::path eRoot = AbsolutePath(options.eDirectory);
		dependencyDirectories.push_back(eRoot / L"static_lib");
		dependencyDirectories.push_back(eRoot / L"BlackMoon");
		dependencyDirectories.push_back(eRoot / L"BlackMoon" / L"lib");
	}
	for (const auto& directory : supportLibrarySearchDirectories) dependencyDirectories.push_back(directory);
	for (const auto& library : program.libraries) {
		if (!library.metadata.filePath.empty()) dependencyDirectories.push_back(library.metadata.filePath.parent_path());
	}
	bool usesCoreLibrary = false;
	bool usesBlackMoonCoreAdapter = false;
	for (const auto& library : program.libraries) {
		if (library.dependency.fileName != "krnln") continue;
		usesCoreLibrary = true;
		std::filesystem::path path;
		if (coreRoots.coreArchive.adapter) {
			path = coreRoots.coreArchive.primaryArchive;
			usesBlackMoonCoreAdapter = true;
		}
		else {
			path = FindStaticLibraryInDirectories(dependencyDirectories, library, targetArchitecture);
		}
		if (path.empty()) {
			result.message = "core_static_archive_not_found:" + PathToUtf8(staticLibrary);
			return false;
		}
		AppendUniquePath(supportLibraries, path);
		if (usesBlackMoonCoreAdapter) {
			AppendUniquePath(supportLibraries, coreRoots.coreArchive.fallbackArchive);
		}
		break;
	}
	for (const std::size_t libraryIndex : generated.reachableLibraries) {
		if (libraryIndex >= program.libraries.size()) continue;
		if (program.libraries[libraryIndex].dependency.fileName == "krnln") continue;
		if (!program.libraries[libraryIndex].implementationAvailable) {
			result.message = "support_library_target_implementation_not_available:" +
				program.libraries[libraryIndex].dependency.fileName;
			return false;
		}
		std::filesystem::path path = FindStaticLibraryInDirectories(
			dependencyDirectories, program.libraries[libraryIndex], targetArchitecture);
		if (path.empty()) {
			result.message = "support_library_static_archive_not_found:" + program.libraries[libraryIndex].dependency.fileName + ":" + PathToUtf8(staticLibrary);
			return false;
		}
		AppendUniquePath(supportLibraries, path);
	}
	// FNEs expose their link-time dependencies through NL_GET_DEPENDENT_LIBS.
	// Resolve those names from the product static-lib tree (or the VC library
	// directory) so adding a new support library does not require compile-mode code
	// changes.  Standard platform imports are supplied below in the same way.
	for (const std::size_t libraryIndex : generated.reachableLibraries) {
		if (libraryIndex >= program.libraries.size()) continue;
		for (const std::string& dependencyName : program.libraries[libraryIndex].metadata.dependentLibraries) {
			const auto artifact = FindLibraryArtifact(dependencyDirectories, dependencyName);
			if (!artifact.empty()) AppendUniquePath(supportLibraries, artifact);
		}
	}
	if (usesCoreLibrary && useLegacyX86Runtime) {
		// The stock core archive contains its database/media bridge entry points
		// but does not publish those two transitive archives through the FNE
		// notification table.  They are archive-level dependencies, independent
		// of which individual source command is used.
		for (const auto& fileName : { L"odbcdb_static.lib", L"mp3_static.lib" }) {
			std::filesystem::path dependency = staticLibrary / fileName;
			if (!IsRegularFile(dependency) && !options.eDirectory.empty()) {
				const std::filesystem::path candidate = AbsolutePath(options.eDirectory) / L"static_lib" / fileName;
				if (IsRegularFile(candidate)) dependency = candidate;
			}
			if (!IsRegularFile(dependency)) {
				result.message = "core_runtime_dependency_not_found:" + PathToUtf8(dependency);
				return false;
			}
			AppendUniquePath(supportLibraries, dependency);
		}
		if (!options.eDirectory.empty()) {
			const std::filesystem::path eRoot = AbsolutePath(options.eDirectory);
			for (const auto& directory : {eRoot / L"BlackMoon" / L"lib", eRoot / L"linker" / L"VC6linker" / L"Lib"}) {
				const std::filesystem::path dao = FindLibraryArtifact({directory}, "DAOUUID.LIB");
				if (!dao.empty()) { AppendUniquePath(supportLibraries, dao); break; }
			}
		}
	}
	std::vector<std::wstring> linkerArguments;
	if (targetX64 || usesModernCoreAdapter) {
		linkerArguments = {
			L"/NOLOGO", program.windowsGui ? L"/SUBSYSTEM:WINDOWS" : L"/SUBSYSTEM:CONSOLE", targetX64 ? L"/MACHINE:X64" : L"/MACHINE:I386", L"/INCREMENTAL:NO", L"/OPT:REF",
			L"/LIBPATH:" + Quote(vcLibrary), L"/OUT:" + Quote(outputPath), Quote(result.objectPath),
		};
		// Some third-party E support libraries are still distributed as VC6
		// archives and carry a /DEFAULTLIB:LIBC directive.  Modern semantic
		// builds use the discovered /MT CRT; allowing the obsolete LIBC archive
		// into the link would either fail with LNK1104 or duplicate UCRT symbols.
		// Suppress only that legacy default-library directive; explicit support
		// archives and the modern CRT remain linked normally.
		if (!targetX64 && usesModernCoreAdapter) {
			linkerArguments.insert(linkerArguments.begin() + 5, L"/NODEFAULTLIB:LIBC");
		}
	}
	else {
		linkerArguments = {
			L"/NOLOGO", L"/FORCE:MULTIPLE", program.windowsGui ? L"/SUBSYSTEM:WINDOWS" : L"/SUBSYSTEM:CONSOLE", L"/MACHINE:I386", L"/INCREMENTAL:NO", L"/OPT:REF",
			L"/NODEFAULTLIB:LIBCMT", L"/INCLUDE:_LegacyVc6Swprintf", L"/ALTERNATENAME:_swprintf=_LegacyVc6Swprintf", L"/ALTERNATENAME:__swprintf=_LegacyVc6Swprintf", L"/ALTERNATENAME:___eapp_info=_eapp_info_data", L"/LIBPATH:" + Quote(vcLibrary), L"/OUT:" + Quote(outputPath),
			Quote(result.objectPath), Quote(mfcLibrary),
		};
	}
	if (!program.buildDll) linkerArguments.push_back(L"/MANIFEST:EMBED");
	if (program.buildDll) {
		linkerArguments.push_back(L"/DLL");
		linkerArguments.push_back(L"/SUBSYSTEM:WINDOWS");
		const std::filesystem::path definitionPath = outputDirectory / (outputPath.stem().wstring() + L".def");
		if (!WriteDllDefinition(definitionPath, outputPath.stem().string(), generated.exports, targetArchitecture, error)) {
			result.message = error;
			return false;
		}
		linkerArguments.push_back(L"/DEF:" + Quote(definitionPath));
	}
	if (!program.buildDll) {
		if (!IsRegularFile(resourceCompiler)) {
			result.message = "windows_sdk_resource_compiler_not_found:" + PathToUtf8(resourceCompiler);
			return false;
		}
	}
	for (const auto& directory : systemLibraryDirectories) linkerArguments.push_back(L"/LIBPATH:" + Quote(directory));
	// Legacy FNE archives carry default-library directives such as LIBCIMT and
	// DAOUUID.  Their companion archives live beside the EasyLanguage runtime,
	// so expose every validated dependency directory to LINK as well.
	for (const auto& directory : dependencyDirectories) {
		if (!directory.empty() && std::filesystem::is_directory(directory)) linkerArguments.push_back(L"/LIBPATH:" + Quote(directory));
	}
	for (const auto& library : { modernRuntimeLibrary, modernCppRuntimeLibrary, modernVcruntimeLibrary, modernUcrtLibrary }) {
		linkerArguments.push_back(Quote(library));
	}
	for (const auto& library : supportLibraries) linkerArguments.push_back(Quote(library));
	for (const auto& library : generatedImportLibraries) linkerArguments.push_back(Quote(library));
	if (useLegacyX86Runtime) linkerArguments.push_back(Quote(vc6RuntimeLibrary));
	// The VC6/MFC compatibility archive is intentionally linked for the same
	// ABI used by classic FNEs.  Its old object files do not consistently carry
	// all of their import-library directives, so provide the platform imports
	// explicitly as part of the host runtime contract.
	for (const auto& importLibrary : {
		L"kernel32.lib", L"user32.lib", L"gdi32.lib", L"winspool.lib",
		L"comdlg32.lib", L"advapi32.lib", L"shell32.lib", L"ole32.lib",
		L"oleaut32.lib", L"olepro32.lib", L"uuid.lib", L"odbc32.lib",
		L"odbccp32.lib", L"wininet.lib", L"winmm.lib", L"comctl32.lib"
	}) {
		const std::filesystem::path artifact = FindLibraryArtifact(
			systemLibraryDirectories, PathToUtf8(std::filesystem::path(importLibrary)));
		if (!artifact.empty()) linkerArguments.push_back(Quote(artifact));
	}
	if (!RunProcess(linker, linkerArguments, outputDirectory, logPath, processOutput, error,
		resourceCompiler.empty() ? std::filesystem::path() : resourceCompiler.parent_path())) {
		result.message = error;
		return false;
	}
	if (!IsRegularFile(outputPath)) {
		result.message = "linker_reported_success_without_output:" + PathToUtf8(outputPath);
		return false;
	}
	if (!options.keepObject) {
		std::filesystem::remove(result.objectPath, filesystemError);
		filesystemError.clear();
		std::filesystem::remove(result.sourcePath, filesystemError);
		for (const auto& library : generatedImportLibraries) std::filesystem::remove(library, filesystemError);
		std::filesystem::remove(outputDirectory / (outputPath.stem().wstring() + L".def"), filesystemError);
		for (const auto& import : generated.imports) {
			std::filesystem::remove(outputDirectory / (outputPath.stem().wstring() + L".import." + std::to_wstring(import.commandIndex) + L".def"), filesystemError);
		}
	}
	result.ok = true;
	result.message =
		"compiled:" + PathToUtf8(outputPath) +
		";compile_mode=semantic" +
		";arch=" + (targetX64 ? std::string("x64") : std::string("x86")) +
		";methods=" + std::to_string(generated.reachableMethodCount) +
		";commands=" + std::to_string(generated.reachableCommandCount) +
		";libraries=" + std::to_string(generated.reachableLibraries.size()) +
		(usesBlackMoonCoreAdapter ? ";core_archive=blackmoon_kernel_adapter" : std::string()) +
		";source=" + PathToUtf8(result.sourcePath) +
		";object=" + PathToUtf8(result.objectPath);
	return true;
}

}  // namespace ecompiler
