
#include "PathHelper.h"
#include <cwctype>
#include <optional>
#include <vector>
#include <regex>
#include <fstream>
#include <algorithm>
#include <utility>
#include <regex>
#include <filesystem>

namespace {

void PushUniquePath(std::vector<std::filesystem::path>& paths, const std::filesystem::path& path);

std::string TrimAsciiWhitespaceCopy(std::string value) {
    auto isAsciiSpace = [](unsigned char ch) {
        return ch == ' ' || ch == '\t' || ch == '\r' || ch == '\n' || ch == '\f' || ch == '\v';
    };

    while (!value.empty() && isAsciiSpace(static_cast<unsigned char>(value.front()))) {
        value.erase(value.begin());
    }
    while (!value.empty() && isAsciiSpace(static_cast<unsigned char>(value.back()))) {
        value.pop_back();
    }
    return value;
}

std::filesystem::path NormalizeModuleLookupFilePath(const std::string& modulePathText) {
    std::string normalizedText = TrimAsciiWhitespaceCopy(modulePathText);
    if (normalizedText.size() >= 2 &&
        normalizedText.front() == '"' &&
        normalizedText.back() == '"') {
        normalizedText = normalizedText.substr(1, normalizedText.size() - 2);
    }
    if (!normalizedText.empty() && normalizedText.front() == '$') {
        normalizedText.erase(normalizedText.begin());
    }

    std::filesystem::path filePath = Utf8PathToPath(normalizedText);
    if (filePath.extension().empty()) {
        filePath += L".ec";
    }
    return filePath;
}

std::filesystem::path NormalizeLookupSourceDir(const std::filesystem::path& sourcePath) {
    if (sourcePath.empty()) {
        return {};
    }
    return sourcePath.has_extension() ? sourcePath.parent_path() : sourcePath;
}

void AppendModuleLookupCandidates(
    std::vector<std::filesystem::path>& candidates,
    const std::filesystem::path& baseDir,
    const std::filesystem::path& relativePath,
    const std::filesystem::path& fileNameOnly) {
    if (baseDir.empty()) {
        return;
    }

    const auto appendModuleDir = [&](const std::filesystem::path& moduleDir) {
        PushUniquePath(candidates, moduleDir / relativePath);
        if (fileNameOnly != relativePath) {
            PushUniquePath(candidates, moduleDir / fileNameOnly);
        }
    };

    PushUniquePath(candidates, baseDir / relativePath);
    if (fileNameOnly != relativePath) {
        PushUniquePath(candidates, baseDir / fileNameOnly);
    }

    appendModuleDir(baseDir / L"ecom");
    appendModuleDir(baseDir / L"模块");

    std::filesystem::path current = baseDir;
    while (!current.empty()) {
        appendModuleDir(current / L"ecom");
        appendModuleDir(current / L"模块");
        if (current == current.root_path()) {
            break;
        }
        current = current.parent_path();
    }
}

std::wstring ExpandEnvironmentStringsCopy(const std::wstring& value) {
    if (value.empty()) {
        return value;
    }

    const DWORD required = ExpandEnvironmentStringsW(value.c_str(), nullptr, 0);
    if (required == 0) {
        return value;
    }

    std::wstring expanded(required, L'\0');
    const DWORD written = ExpandEnvironmentStringsW(value.c_str(), expanded.data(), required);
    if (written == 0 || written > expanded.size()) {
        return value;
    }
    if (!expanded.empty() && expanded.back() == L'\0') {
        expanded.pop_back();
    }
    return expanded;
}

std::wstring Utf8ToWideCopy(const std::string& value) {
    if (value.empty()) {
        return std::wstring();
    }

    const int required = MultiByteToWideChar(
        CP_UTF8,
        MB_ERR_INVALID_CHARS,
        value.data(),
        static_cast<int>(value.size()),
        nullptr,
        0);
    if (required <= 0) {
        return std::wstring();
    }

    std::wstring wide(static_cast<size_t>(required), L'\0');
    if (MultiByteToWideChar(
            CP_UTF8,
            MB_ERR_INVALID_CHARS,
            value.data(),
            static_cast<int>(value.size()),
            wide.data(),
            required) <= 0) {
        return std::wstring();
    }
    return wide;
}

std::string WideToUtf8Copy(const std::wstring& value) {
    if (value.empty()) {
        return std::string();
    }

    const int required = WideCharToMultiByte(
        CP_UTF8,
        0,
        value.data(),
        static_cast<int>(value.size()),
        nullptr,
        0,
        nullptr,
        nullptr);
    if (required <= 0) {
        return std::string();
    }

    std::string utf8(static_cast<size_t>(required), '\0');
    if (WideCharToMultiByte(
            CP_UTF8,
            0,
            value.data(),
            static_cast<int>(value.size()),
            utf8.data(),
            required,
            nullptr,
            nullptr) <= 0) {
        return std::string();
    }
    return utf8;
}

std::optional<std::wstring> ReadRegistryDefaultString(const HKEY root, const wchar_t* subKey, const REGSAM wowFlags = 0) {
    HKEY key = nullptr;
    const LONG openResult = RegOpenKeyExW(root, subKey, 0, KEY_QUERY_VALUE | wowFlags, &key);
    if (openResult != ERROR_SUCCESS || key == nullptr) {
        return std::nullopt;
    }

    DWORD valueType = 0;
    DWORD valueSize = 0;
    const LONG sizeResult = RegQueryValueExW(key, nullptr, nullptr, &valueType, nullptr, &valueSize);
    if (sizeResult != ERROR_SUCCESS || valueSize == 0 || (valueType != REG_SZ && valueType != REG_EXPAND_SZ)) {
        RegCloseKey(key);
        return std::nullopt;
    }

    std::wstring value(valueSize / sizeof(wchar_t), L'\0');
    const LONG readResult = RegQueryValueExW(
        key,
        nullptr,
        nullptr,
        &valueType,
        reinterpret_cast<LPBYTE>(value.data()),
        &valueSize);
    RegCloseKey(key);
    if (readResult != ERROR_SUCCESS) {
        return std::nullopt;
    }

    value.resize(valueSize / sizeof(wchar_t));
    while (!value.empty() && value.back() == L'\0') {
        value.pop_back();
    }
    if (value.empty()) {
        return std::nullopt;
    }
    if (valueType == REG_EXPAND_SZ) {
        value = ExpandEnvironmentStringsCopy(value);
    }
    return value;
}

std::wstring TrimLeftWhitespaceCopy(std::wstring value) {
    value.erase(
        value.begin(),
        std::find_if(value.begin(), value.end(), [](const wchar_t ch) { return !std::iswspace(ch); }));
    return value;
}

std::wstring ExtractExecutablePathFromCommand(std::wstring commandText) {
    commandText = TrimLeftWhitespaceCopy(std::move(commandText));
    if (commandText.empty()) {
        return std::wstring();
    }

    if (commandText.front() == L'"') {
        const size_t endQuote = commandText.find(L'"', 1);
        if (endQuote == std::wstring::npos) {
            return commandText.substr(1);
        }
        return commandText.substr(1, endQuote - 1);
    }

    std::wstring lowered = commandText;
    std::transform(lowered.begin(), lowered.end(), lowered.begin(), towlower);
    const size_t exePos = lowered.find(L".exe");
    if (exePos != std::wstring::npos) {
        return commandText.substr(0, exePos + 4);
    }

    size_t end = 0;
    while (end < commandText.size() && !std::iswspace(commandText[end])) {
        ++end;
    }
    return commandText.substr(0, end);
}

void PushUniquePath(std::vector<std::filesystem::path>& paths, const std::filesystem::path& path) {
    if (path.empty()) {
        return;
    }

    const std::filesystem::path normalized = path.lexically_normal();
    for (const auto& existing : paths) {
        if (existing.lexically_normal() == normalized) {
            return;
        }
    }
    paths.push_back(normalized);
}

} // namespace


std::string GetBasePath() {
    char buffer[MAX_PATH];
    GetModuleFileName(NULL, buffer, MAX_PATH);
    std::string::size_type pos = std::string(buffer).find_last_of("\\/");
    return std::string(buffer).substr(0, pos);
}

std::filesystem::path Utf8PathToPath(const std::string& utf8Path) {
#ifdef _WIN32
    const std::wstring widePath = Utf8ToWideCopy(utf8Path);
    if (!widePath.empty() || utf8Path.empty()) {
        return std::filesystem::path(widePath);
    }
#endif
    return std::filesystem::path(utf8Path);
}

std::string PathToUtf8(const std::filesystem::path& path) {
#ifdef _WIN32
    const std::string utf8 = WideToUtf8Copy(path.wstring());
    if (!utf8.empty() || path.empty()) {
        return utf8;
    }
#endif
    return path.string();
}

std::string WideToUtf8Text(const std::wstring& text) {
    return WideToUtf8Copy(text);
}

std::string Utf8Literal(const std::u8string_view text) {
    return std::string(reinterpret_cast<const char*>(text.data()), text.size());
}

std::vector<std::filesystem::path> GetRegisteredEplOpenCommandBaseDirs() {
    std::vector<std::filesystem::path> baseDirs;
    const std::pair<HKEY, const wchar_t*> registryLocations[] = {
        { HKEY_CLASSES_ROOT, L"E.Document\\Shell\\Open\\Command" },
        { HKEY_LOCAL_MACHINE, L"SOFTWARE\\Classes\\E.Document\\Shell\\Open\\Command" },
    };
    const REGSAM registryViews[] = {
        0,
        KEY_WOW64_64KEY,
        KEY_WOW64_32KEY,
    };

    for (const auto& [root, subKey] : registryLocations) {
        for (const auto view : registryViews) {
            if (root == HKEY_CLASSES_ROOT && view != 0) {
                continue;
            }

            const std::optional<std::wstring> commandText = ReadRegistryDefaultString(root, subKey, view);
            if (!commandText.has_value()) {
                continue;
            }

            std::wstring executablePath = ExtractExecutablePathFromCommand(*commandText);
            if (executablePath.empty()) {
                continue;
            }
            executablePath = ExpandEnvironmentStringsCopy(executablePath);

            std::filesystem::path exePath(executablePath);
            PushUniquePath(baseDirs, exePath.parent_path());
        }
    }

    return baseDirs;
}

std::vector<std::filesystem::path> BuildModuleFileLookupCandidates(
    const std::filesystem::path& sourcePath,
    const std::string& modulePathText) {
    std::vector<std::filesystem::path> candidates;
    const std::filesystem::path filePath = NormalizeModuleLookupFilePath(modulePathText);
    if (filePath.empty()) {
        return candidates;
    }

    if (filePath.is_absolute()) {
        PushUniquePath(candidates, filePath);
    }

    const std::filesystem::path relativePath = filePath.is_absolute() ? filePath.filename() : filePath;
    const std::filesystem::path fileNameOnly = filePath.filename().empty() ? relativePath : filePath.filename();
    if (relativePath.empty()) {
        return candidates;
    }

    AppendModuleLookupCandidates(
        candidates,
        NormalizeLookupSourceDir(sourcePath),
        relativePath,
        fileNameOnly);

    std::error_code ec;
    AppendModuleLookupCandidates(
        candidates,
        std::filesystem::current_path(ec),
        relativePath,
        fileNameOnly);
    AppendModuleLookupCandidates(
        candidates,
        Utf8PathToPath(GetBasePath()),
        relativePath,
        fileNameOnly);
    for (const auto& registeredBaseDir : GetRegisteredEplOpenCommandBaseDirs()) {
        AppendModuleLookupCandidates(candidates, registeredBaseDir, relativePath, fileNameOnly);
    }

    return candidates;
}

std::string ExtractBetweenDashes(const std::string& text) {
    std::string delimiter = " - ";

    // 找到第一个 " - " 的位置
    size_t start = text.find(delimiter);
    if (start == std::string::npos) {
        // 没有找到 " - "，返回空字符串
        return "";
    }
    start += delimiter.length(); // 跳过 " - "，从第一个 " - " 之后的字符开始

    // 从 start 位置开始，找到第二个 " - " 的位置
    size_t end = text.find(delimiter, start);
    if (end == std::string::npos) {
        // 没有找到第二个 " - "，返回空字符串
        return "";
    }

    // 取两个 " - " 之间的字符串
    return text.substr(start, end - start);
}

/// <summary>
/// 查找文件中指定字节序列的偏移量
/// </summary>
/// <param name="filename"></param>
/// <param name="search_bytes"></param>
/// <returns></returns>
std::optional<size_t> FindByteInFile(const std::string& filename, const std::vector<char>& search_bytes) {
    // 从文件中读取数据
    std::ifstream file(Utf8PathToPath(filename), std::ios::binary);
    std::vector<char> file_contents((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());

    // 查找字节序列
    auto it = std::search(file_contents.begin(), file_contents.end(), search_bytes.begin(), search_bytes.end());

    if (it != file_contents.end()) {
        // 找到了，返回位置
        return std::distance(file_contents.begin(), it);
    }
    else {
        // 没有找到，返回 std::nullopt
        return std::nullopt;
    }
}




/// <summary>
/// 解析链接器命令中 /out: 参数的文件名
/// </summary>
/// <param name="s"></param>
/// <returns></returns>
std::string GetLinkerCommandOutFileName(const std::string& s) {
    std::regex reg("/out:\"([^\"]*)\"");  
    std::smatch match;

    if (std::regex_search(s, match, reg) && match.size() > 1) {
        std::string path = match.str(1); 
        std::filesystem::path fs_path(path);
        return fs_path.filename().string(); 
    }
    else {
        return ""; 
    }
}



// 从命令行字符串中提取包含特定目标关键字的路径
std::string ExtractPathFromCommand(const std::string& commandLine, const std::string& target) {
    std::string foundPath;
    size_t pos = commandLine.find(target);
    if (pos != std::string::npos) {
        // 在目标字符串位置之前找到最近的双引号
        size_t start = commandLine.rfind('"', pos);
        // 在目标字符串位置之后找到下一个双引号
        size_t end = commandLine.find('"', pos + target.length());

        // 如果找到了双引号，提取路径
        if (start != std::string::npos && end != std::string::npos) {
            foundPath = commandLine.substr(start + 1, end - start - 1);
        }
    }
    return foundPath;
}

std::string GetLinkerCommandKrnlnFileName(const std::string& s) {
    std::string target = "\\static_lib\\krnln_static.lib";
    std::string foundPath = ExtractPathFromCommand(s, target);
    return foundPath;
}

