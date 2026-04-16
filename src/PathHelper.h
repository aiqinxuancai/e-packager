#pragma once

#include <string>
#include <vector>
#include <optional>
#include <filesystem>
#include <Windows.h>

std::string GetBasePath();
std::vector<std::filesystem::path> GetRegisteredEplOpenCommandBaseDirs();

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


std::string GetLinkerCommandKrnlnFileName(const std::string& s);
