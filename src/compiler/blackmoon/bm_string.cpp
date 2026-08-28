// ============================================================================
// bm_string.cpp - 字符串工具实现
// ============================================================================
#include "bm_string.h"

#include <cstdarg>
#include <cstdio>
#include <algorithm>
#include <windows.h>

namespace bm {

// 格式化字符串:先用 vsnprintf 计算所需长度,再分配缓冲区写入。
std::string format(const char* fmt, ...)
{
    va_list args;

    // 第一次调用计算所需长度
    va_start(args, fmt);
    int len = vsnprintf(nullptr, 0, fmt, args);
    va_end(args);

    if (len < 0) {
        return std::string();
    }

    // 第二次调用实际写入
    std::string result(len + 1, '\0');
    va_start(args, fmt);
    vsnprintf(&result[0], result.size(), fmt, args);
    va_end(args);

    // 去掉末尾多余的 '\0'
    result.resize(len);
    return result;
}

// 按分隔符分割字符串
std::vector<std::string> split(const std::string& str, char delimiter, bool trim /*= false*/)
{
    std::vector<std::string> tokens;
    std::string current;

    for (char ch : str) {
        if (ch == delimiter) {
            if (trim) {
                current = bm::trim(current);
            }
            tokens.push_back(current);
            current.clear();
        } else {
            current += ch;
        }
    }
    if (trim) {
        current = bm::trim(current);
    }
    tokens.push_back(current);

    return tokens;
}

// 去除首尾空白
std::string trim(const std::string& s)
{
    size_t start = s.find_first_not_of(" \t\r\n");
    if (start == std::string::npos) {
        return std::string();
    }
    size_t end = s.find_last_not_of(" \t\r\n");
    return s.substr(start, end - start + 1);
}

// 不区分大小写比较
bool equalsIgnoreCase(const std::string& a, const std::string& b)
{
    if (a.size() != b.size()) {
        return false;
    }
    return _stricmp(a.c_str(), b.c_str()) == 0;
}

// 不区分大小写查找子串
size_t findIgnoreCase(const std::string& haystack, const std::string& needle)
{
    auto it = std::search(
        haystack.begin(), haystack.end(),
        needle.begin(), needle.end(),
        [](char a, char b) { return tolower(static_cast<unsigned char>(a)) ==
                                   tolower(static_cast<unsigned char>(b)); });
    if (it == haystack.end()) {
        return std::string::npos;
    }
    return static_cast<size_t>(it - haystack.begin());
}

// 从右侧查找字符
size_t reverseFind(const std::string& s, char ch)
{
    return s.find_last_of(ch);
}

// 取左部 n 个字符
std::string left(const std::string& s, size_t n)
{
    if (n >= s.size()) {
        return s;
    }
    return s.substr(0, n);
}

// 取右部 n 个字符
std::string right(const std::string& s, size_t n)
{
    if (n >= s.size()) {
        return s;
    }
    return s.substr(s.size() - n);
}

// 取中间子串
std::string mid(const std::string& s, size_t pos, size_t n)
{
    if (pos >= s.size()) {
        return std::string();
    }
    return s.substr(pos, n);
}

// 宽字符转多字节(使用系统 ANSI 代码页,通常为 GBK)
std::string wideToAnsi(const std::wstring& wstr)
{
    if (wstr.empty()) {
        return std::string();
    }
    int len = WideCharToMultiByte(CP_ACP, 0, wstr.c_str(),
                                  static_cast<int>(wstr.size()),
                                  nullptr, 0, nullptr, nullptr);
    std::string result(len, '\0');
    WideCharToMultiByte(CP_ACP, 0, wstr.c_str(),
                        static_cast<int>(wstr.size()),
                        &result[0], len, nullptr, nullptr);
    return result;
}

// 多字节转宽字符
std::wstring ansiToWide(const std::string& str)
{
    if (str.empty()) {
        return std::wstring();
    }
    int len = MultiByteToWideChar(CP_ACP, 0, str.c_str(),
                                  static_cast<int>(str.size()),
                                  nullptr, 0);
    std::wstring result(len, L'\0');
    MultiByteToWideChar(CP_ACP, 0, str.c_str(),
                        static_cast<int>(str.size()),
                        &result[0], len);
    return result;
}

} // namespace bm
