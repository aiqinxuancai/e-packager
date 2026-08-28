// ============================================================================
// bm_string.h - 字符串工具(替代 MFC CString)
// ============================================================================
// 原黑月大量使用 MFC 的 CString/CStringArray。本项目不使用 MFC,
// 改用 std::string 配合以下工具函数实现等价功能:
//   1. 格式化字符串(bm::format),替代 CString::Format
//   2. 字符串分割(bm::split),替代 SplitString
//   3. 路径拼接、查找等辅助函数
// ============================================================================
#ifndef __BM_STRING_H__
#define __BM_STRING_H__

#include <string>
#include <vector>

namespace bm {

// 格式化字符串(类似 printf,返回 std::string)
// 用法:std::string s = bm::format("值=%d, 文本=%s", 123, "abc");
std::string format(const char* fmt, ...);

// 按分隔符分割字符串,返回各段。trim 是否去除每段首尾空白。
std::vector<std::string> split(const std::string& str, char delimiter, bool trim = false);

// 去除首尾空白
std::string trim(const std::string& s);

// 不区分大小写比较
bool equalsIgnoreCase(const std::string& a, const std::string& b);

// 查找子串(不区分大小写),找到返回位置,否则返回 std::string::npos
size_t findIgnoreCase(const std::string& haystack, const std::string& needle);

// 从右侧查找字符位置,未找到返回 std::string::npos
size_t reverseFind(const std::string& s, char ch);

// 取左部 n 个字符
std::string left(const std::string& s, size_t n);

// 取右部 n 个字符
std::string right(const std::string& s, size_t n);

// 取中间子串(从 pos 开始,长度 n)
std::string mid(const std::string& s, size_t pos, size_t n);

// 宽字符转多字节(UTF-16 -> ANSI/GBK)
std::string wideToAnsi(const std::wstring& wstr);

// 多字节转宽字符(ANSI/GBK -> UTF-16)
std::wstring ansiToWide(const std::string& str);

} // namespace bm

#endif // __BM_STRING_H__
