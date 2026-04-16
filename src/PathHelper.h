#pragma once

#include <string>
#include <string_view>
#include <vector>
#include <optional>
#include <filesystem>
#include <Windows.h>

// 获取程序所在目录。
std::string GetBasePath();
// 获取注册表中易语言打开命令对应的安装目录。
std::vector<std::filesystem::path> GetRegisteredEplOpenCommandBaseDirs();

// 提取两个 " - " 之间的文本。
std::string ExtractBetweenDashes(const std::string& text);

/// <summary>
/// 查找文件中指定字节序列的偏移量
/// </summary>
/// <param name="filename"></param>
/// <param name="search_bytes"></param>
/// <returns></returns>
std::optional<size_t> FindByteInFile(const std::string& filename, const std::vector<char>& search_bytes);

/// <summary>
/// 解析链接器命令中 /out: 参数的文件名
/// </summary>
/// <param name="s"></param>
/// <returns></returns>
std::string GetLinkerCommandOutFileName(const std::string& s);

// 解析链接器命令中静态库路径。
std::string GetLinkerCommandKrnlnFileName(const std::string& s);
// 将 UTF-8 路径转换为当前平台原生路径对象。
std::filesystem::path Utf8PathToPath(const std::string& utf8Path);
// 将宽字符串转换为 UTF-8 字符串。
std::string WideToUtf8Text(const std::wstring& text);
// 将当前平台原生路径对象转换为 UTF-8 字符串。
std::string PathToUtf8(const std::filesystem::path& path);
// 将 UTF-8 字面量转换为 std::string。
std::string Utf8Literal(std::u8string_view text);
