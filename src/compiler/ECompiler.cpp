#include "ECompiler.h"

#include "BlackMoonCompiler.h"
#include "CompilerModel.h"
#include "CppEmitter.h"
#include "../PathHelper.h"
#include "../e2txt.h"
#include "../EFolderCodec.h"

#include <Windows.h>

#include <algorithm>
#include <cwctype>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <system_error>
#include <vector>

namespace ecompiler {
namespace {

std::filesystem::path DefaultCompilerPath()
{
	return L"C:\\Users\\aiqin\\OneDrive\\e5.6\\linker\\VC2022Linker\\bin\\cl.exe";
}

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

bool DiscoverBuildEnvironment(
	std::filesystem::path& matchingCompiler,
	std::vector<std::filesystem::path>& includeDirectories,
	std::vector<std::filesystem::path>& libraryDirectories,
	std::string& error)
{
	matchingCompiler.clear();
	includeDirectories.clear();
	libraryDirectories.clear();
	std::filesystem::path vcTools;
	wchar_t configured[MAX_PATH] {};
	if (GetEnvironmentVariableW(L"VCToolsInstallDir", configured, MAX_PATH) > 0) vcTools = configured;
	if (vcTools.empty() || !std::filesystem::is_directory(vcTools)) {
		const std::filesystem::path visualStudioRoot = L"C:\\Program Files\\Microsoft Visual Studio";
		std::error_code filesystemError;
		std::vector<std::filesystem::path> candidates;
		for (const auto& version : std::filesystem::directory_iterator(visualStudioRoot, filesystemError)) {
			if (!version.is_directory(filesystemError)) continue;
			for (const auto& edition : std::filesystem::directory_iterator(version.path(), filesystemError)) {
				if (!edition.is_directory(filesystemError)) continue;
				const auto candidate = LatestVersionDirectory(edition.path() / L"VC" / L"Tools" / L"MSVC");
				if (!candidate.empty() && IsRegularFile(candidate / L"include" / L"vector")) candidates.push_back(candidate);
			}
		}
		if (!candidates.empty()) {
			std::sort(candidates.begin(), candidates.end(), std::greater<>());
			vcTools = candidates.front();
		}
	}
	const std::filesystem::path windowsKit = LatestVersionDirectory(L"C:\\Program Files (x86)\\Windows Kits\\10\\Include");
	if (vcTools.empty() || windowsKit.empty()) {
		error = "visual_cpp_or_windows_sdk_not_found";
		return false;
	}
	for (const auto& candidate : {
		vcTools / L"bin" / L"Hostx86" / L"x86" / L"cl.exe",
		vcTools / L"bin" / L"Hostx64" / L"x86" / L"cl.exe",
	}) {
		if (IsRegularFile(candidate)) {
			matchingCompiler = candidate;
			break;
		}
	}
	if (matchingCompiler.empty()) {
		error = "matching_x86_compiler_not_found:" + PathToUtf8(vcTools);
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
	const std::filesystem::path kitLib = windowsKit.parent_path().parent_path() / L"Lib" / kitVersion;
	libraryDirectories = {
		vcTools / L"lib" / L"x86",
		kitLib / L"ucrt" / L"x86",
		kitLib / L"um" / L"x86",
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
	std::ofstream output(path, std::ios::binary | std::ios::trunc);
	if (!output) {
		error = "open_generated_source_failed:" + PathToUtf8(path);
		return false;
	}
	output.write("\xEF\xBB\xBF", 3);
	for (std::size_t index = 0; index < text.size(); ++index) {
		if (text[index] == '\n' && (index == 0 || text[index - 1] != '\r')) output.put('\r');
		output.put(text[index]);
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
	std::string& error)
{
	std::ostringstream text;
	text << "LIBRARY \"" << DefQuotedName(libraryName) << "\"\r\nEXPORTS\r\n";
	for (const auto& item : exports) {
		if (item.name.empty() || item.symbol.empty()) continue;
		const std::string decorated = item.usesCdecl
			? item.symbol
			: ("_" + item.symbol + "@" + std::to_string(item.stackBytes));
		text << "    " << item.name << "=" << decorated << "\r\n";
	}
	return WriteTextFile(path, text.str(), error);
}

bool WriteImportDefinition(
	const std::filesystem::path& path,
	const GeneratedSource::ImportedFunction& item,
	std::string& error)
{
	std::ostringstream text;
	text << "LIBRARY \"" << DefQuotedName(item.moduleName) << "\"\r\nEXPORTS\r\n";
	const std::string decoratedAlias = item.usesCdecl
		? item.symbol
		: (item.symbol + "@" + std::to_string(item.stackBytes));
	text << "    " << decoratedAlias << "=" << item.entryName << "\r\n";
	return WriteTextFile(path, text.str(), error);
}

bool IsPlatformImportModule(const std::string& moduleName)
{
	std::filesystem::path modulePath = Utf8PathToPath(moduleName);
	std::string normalized = modulePath.filename().string();
	if (normalized.empty()) normalized = moduleName;
	std::transform(normalized.begin(), normalized.end(), normalized.begin(), [](const unsigned char value) {
		return static_cast<char>(std::tolower(value));
	});
	if (std::filesystem::path(normalized).extension().empty()) normalized += ".dll";
	return normalized == "kernel32.dll" || normalized == "user32.dll" || normalized == "gdi32.dll" ||
		normalized == "advapi32.dll" || normalized == "shell32.dll" || normalized == "ole32.dll" ||
		normalized == "oleaut32.dll" || normalized == "comdlg32.dll" || normalized == "winmm.dll" ||
		normalized == "odbc32.dll" || normalized == "odbccp32.dll" || normalized == "ws2_32.dll";
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
	const BOOL started = CreateProcessW(
		executable.c_str(), mutableCommand.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW,
		nullptr, workingDirectory.c_str(), &startup, &process);
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

bool ReadInputBundle(
	const std::filesystem::path& inputPath,
	e2txt::ProjectBundle& bundle,
	std::filesystem::path& inputRoot,
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
	if (!generator.GenerateBundle(PathToUtf8(inputPath), bundle, &error)) {
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
	const Library& library)
{
	std::vector<std::wstring> candidateNames;
	if (library.dependency.fileName == "krnln") candidateNames.push_back(L"krnln_static.lib");
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
		if (IsRegularFile(candidate)) return candidate;
	}
	std::error_code error;
	for (const auto& entry : std::filesystem::directory_iterator(directory, error)) {
		if (!entry.is_regular_file(error)) continue;
		std::wstring fileName = entry.path().filename().wstring();
		std::transform(fileName.begin(), fileName.end(), fileName.begin(), towlower);
		for (std::wstring expected : candidateNames) {
			std::transform(expected.begin(), expected.end(), expected.begin(), towlower);
			if (fileName == expected) return entry.path();
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

bool ResolveToolchain(
	const Options& options,
	std::filesystem::path& compiler,
	std::filesystem::path& linker,
	std::filesystem::path& vcLibrary,
	std::filesystem::path& productRoot,
	std::string& error)
{
#if !defined(_M_IX86)
	(void)options; (void)compiler; (void)linker; (void)vcLibrary; (void)productRoot;
	error = "independent compiler backend requires Win32 e-packager";
	return false;
#else
	compiler = options.compilerPath.empty() ? DefaultCompilerPath() : options.compilerPath;
	linker = options.linkerPath.empty() ? compiler.parent_path() / L"link.exe" : options.linkerPath;
	vcLibrary = options.libraryPath.empty() ? compiler.parent_path().parent_path() / L"lib" : options.libraryPath;
	productRoot = ProductRootFromCompiler(compiler);
	if (!IsRegularFile(compiler)) { error = "compiler_not_found:" + PathToUtf8(compiler); return false; }
	if (!IsRegularFile(linker)) { error = "linker_not_found:" + PathToUtf8(linker); return false; }
	if (!std::filesystem::is_directory(vcLibrary)) { error = "linker_library_directory_not_found:" + PathToUtf8(vcLibrary); return false; }
	return true;
#endif
}

}  // namespace

bool Compile(
	const std::filesystem::path& inputPath,
	const std::filesystem::path& outputPath,
	const Options& options,
	Result& result)
{
	if (options.backend == Backend::BlackMoon) {
		return blackmoon_backend::Compile(inputPath, outputPath, options, result);
	}
	result = {};
	result.outputPath = outputPath;
	std::string error;
	std::filesystem::path compiler;
	std::filesystem::path linker;
	std::filesystem::path vcLibrary;
	std::filesystem::path productRoot;
	if (!ResolveToolchain(options, compiler, linker, vcLibrary, productRoot, error)) {
		result.message = error;
		return false;
	}
	e2txt::ProjectBundle bundle;
	std::filesystem::path inputRoot;
	if (!ReadInputBundle(inputPath, bundle, inputRoot, error)) {
		result.message = error;
		return false;
	}
	if (!bundle.formFiles.empty() || !bundle.windowBindings.empty()) {
		result.message = "window_project_not_supported_by_independent_compiler";
		return false;
	}
	Program program;
	if (!BuildCompilerModel(std::move(bundle), inputRoot, { productRoot / L"lib" }, options.conditionMacros, program, error)) {
		result.message = "compiler_model_failed:" + error;
		return false;
	}
	program.buildDll = options.buildDll || outputPath.extension() == L".dll";
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
	std::vector<std::filesystem::path> includeDirectories;
	std::vector<std::filesystem::path> systemLibraryDirectories;
	std::filesystem::path matchingCompiler;
	if (!DiscoverBuildEnvironment(matchingCompiler, includeDirectories, systemLibraryDirectories, error)) {
		result.message = error;
		return false;
	}
	if (options.compilerPath.empty()) compiler = matchingCompiler;
	std::vector<std::filesystem::path> generatedImportLibraries;
	const std::filesystem::path libraryManager = productRoot / L"linker" / L"VC6linker" / L"Bin" / L"LIB.EXE";
	for (const auto& import : generated.imports) {
		if (import.moduleName.empty() || import.entryName.empty()) continue;
		if (IsPlatformImportModule(import.moduleName)) continue;
		const std::filesystem::path importDef = outputDirectory /
			(outputPath.stem().wstring() + L".import." + std::to_wstring(import.commandIndex) + L".def");
		const std::filesystem::path importLib = outputDirectory /
			(outputPath.stem().wstring() + L".import." + std::to_wstring(import.commandIndex) + L".lib");
		if (!WriteImportDefinition(importDef, import, error)) {
			result.message = error;
			return false;
		}
		generatedImportLibraries.push_back(importLib);
		const std::filesystem::path importLog = outputDirectory /
			(outputPath.stem().wstring() + L".import." + std::to_wstring(import.commandIndex) + L".log");
		std::string importOutput;
		if (!IsRegularFile(libraryManager) || !RunProcess(libraryManager,
			{L"/NOLOGO", L"/MACHINE:I386", L"/DEF:" + Quote(importDef), L"/OUT:" + Quote(importLib)},
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
		L"/nologo", L"/c", L"/O2", L"/Gy", L"/Zl", L"/GS-", L"/GR-", L"/EHsc", L"/arch:IA32", L"/MT", L"/std:c++20",
		L"/source-charset:utf-8", L"/execution-charset:.936", L"/Fo" + Quote(result.objectPath), Quote(result.sourcePath),
	};
	for (const auto& directory : includeDirectories) compilerArguments.push_back(L"/I" + Quote(directory));
	if (!RunProcess(compiler, compilerArguments, outputDirectory, logPath, processOutput, error)) {
		result.message = error;
		return false;
	}
	const std::filesystem::path staticLibrary = productRoot / L"static_lib";
	const std::filesystem::path vc6RuntimeLibrary = productRoot / L"linker" / L"VC6linker" / L"Lib" / L"MSVCRT.LIB";
	const std::filesystem::path mfcLibrary = vcLibrary / L"NAFXCW.LIB";
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
	std::vector<std::filesystem::path> supportLibraries;
	std::vector<std::filesystem::path> dependencyDirectories = {
		staticLibrary, vcLibrary, program.inputRoot,
	};
	for (const auto& library : program.libraries) {
		if (!library.metadata.filePath.empty()) dependencyDirectories.push_back(library.metadata.filePath.parent_path());
	}
	bool usesCoreLibrary = false;
	for (const auto& library : program.libraries) {
		if (library.dependency.fileName != "krnln") continue;
		usesCoreLibrary = true;
		std::filesystem::path path = FindStaticLibrary(staticLibrary, library);
		if (path.empty()) path = FindStaticLibrary(library.metadata.filePath.parent_path(), library);
		if (path.empty()) path = FindStaticLibrary(program.inputRoot, library);
		if (path.empty()) {
			result.message = "core_static_archive_not_found:" + PathToUtf8(staticLibrary);
			return false;
		}
		AppendUniquePath(supportLibraries, path);
		break;
	}
	for (const std::size_t libraryIndex : generated.reachableLibraries) {
		if (libraryIndex >= program.libraries.size()) continue;
		if (program.libraries[libraryIndex].dependency.fileName == "krnln") continue;
		std::filesystem::path path = FindStaticLibrary(staticLibrary, program.libraries[libraryIndex]);
		if (path.empty()) path = FindStaticLibrary(program.libraries[libraryIndex].metadata.filePath.parent_path(), program.libraries[libraryIndex]);
		if (path.empty()) path = FindStaticLibrary(program.inputRoot, program.libraries[libraryIndex]);
		if (path.empty()) {
			result.message = "support_library_static_archive_not_found:" + program.libraries[libraryIndex].dependency.fileName + ":" + PathToUtf8(staticLibrary);
			return false;
		}
		AppendUniquePath(supportLibraries, path);
	}
	// FNEs expose their link-time dependencies through NL_GET_DEPENDENT_LIBS.
	// Resolve those names from the product static-lib tree (or the VC library
	// directory) so adding a new support library does not require backend code
	// changes.  Standard platform imports are supplied below in the same way.
	for (const std::size_t libraryIndex : generated.reachableLibraries) {
		if (libraryIndex >= program.libraries.size()) continue;
		for (const std::string& dependencyName : program.libraries[libraryIndex].metadata.dependentLibraries) {
			const auto artifact = FindLibraryArtifact(dependencyDirectories, dependencyName);
			if (!artifact.empty()) AppendUniquePath(supportLibraries, artifact);
		}
	}
	if (usesCoreLibrary) {
		// The stock core archive contains its database/media bridge entry points
		// but does not publish those two transitive archives through the FNE
		// notification table.  They are archive-level dependencies, independent
		// of which individual source command is used.
		for (const auto& fileName : { L"odbcdb_static.lib", L"mp3_static.lib" }) {
			const std::filesystem::path dependency = staticLibrary / fileName;
			if (!IsRegularFile(dependency)) {
				result.message = "core_runtime_dependency_not_found:" + PathToUtf8(dependency);
				return false;
			}
			AppendUniquePath(supportLibraries, dependency);
		}
	}
	std::vector<std::wstring> linkerArguments = {
		L"/NOLOGO", L"/FORCE:MULTIPLE", L"/SUBSYSTEM:CONSOLE", L"/MACHINE:I386", L"/INCREMENTAL:NO", L"/OPT:REF",
		L"/NODEFAULTLIB:LIBCMT", L"/INCLUDE:_LegacyVc6Swprintf", L"/ALTERNATENAME:_swprintf=_LegacyVc6Swprintf", L"/ALTERNATENAME:__swprintf=_LegacyVc6Swprintf", L"/ALTERNATENAME:___eapp_info=_eapp_info_data", L"/LIBPATH:" + Quote(vcLibrary), L"/OUT:" + Quote(outputPath),
		Quote(result.objectPath), Quote(mfcLibrary),
	};
	if (program.buildDll) {
		linkerArguments.push_back(L"/DLL");
		linkerArguments.push_back(L"/SUBSYSTEM:WINDOWS");
		const std::filesystem::path definitionPath = outputDirectory / (outputPath.stem().wstring() + L".def");
		if (!WriteDllDefinition(definitionPath, outputPath.stem().string(), generated.exports, error)) {
			result.message = error;
			return false;
		}
		linkerArguments.push_back(L"/DEF:" + Quote(definitionPath));
	}
	for (const auto& directory : systemLibraryDirectories) linkerArguments.push_back(L"/LIBPATH:" + Quote(directory));
	for (const auto& library : { modernRuntimeLibrary, modernCppRuntimeLibrary, modernVcruntimeLibrary, modernUcrtLibrary }) {
		linkerArguments.push_back(Quote(library));
	}
	for (const auto& library : supportLibraries) linkerArguments.push_back(Quote(library));
	for (const auto& library : generatedImportLibraries) linkerArguments.push_back(Quote(library));
	linkerArguments.push_back(Quote(vc6RuntimeLibrary));
	// The VC6/MFC compatibility archive is intentionally linked for the same
	// ABI used by classic FNEs.  Its old object files do not consistently carry
	// all of their import-library directives, so provide the platform imports
	// explicitly as part of the host runtime contract.
	for (const auto& importLibrary : {
		L"kernel32.lib", L"user32.lib", L"gdi32.lib", L"winspool.lib",
		L"comdlg32.lib", L"advapi32.lib", L"shell32.lib", L"ole32.lib",
		L"oleaut32.lib", L"olepro32.lib", L"uuid.lib", L"odbc32.lib",
		L"odbccp32.lib", L"wininet.lib", L"winmm.lib", L"comctl32.lib"
	}) linkerArguments.push_back(importLibrary);
	if (!RunProcess(linker, linkerArguments, outputDirectory, logPath, processOutput, error)) {
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
		";methods=" + std::to_string(generated.reachableMethodCount) +
		";commands=" + std::to_string(generated.reachableCommandCount) +
		";libraries=" + std::to_string(generated.reachableLibraries.size()) +
		";source=" + PathToUtf8(result.sourcePath) +
		";object=" + PathToUtf8(result.objectPath);
	return true;
}

}  // namespace ecompiler
