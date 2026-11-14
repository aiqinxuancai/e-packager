#include "SourceTextCodec.h"
#include <Windows.h>
#include <algorithm>
#include <cstdint>
#include <cstring>
#include <string_view>

namespace e2txt::source_text {
namespace {
constexpr const char* kTextLiteralLeftQuote = "“";
constexpr const char* kTextLiteralRightQuote = "”";
constexpr const char* kEscapedTextLiteralPrefix = "#e2txt_text#";
constexpr const char* kEscapedLongTextLiteralPrefix = "#e2txt_long_text#";
} // namespace

char ToHexDigit(const std::uint8_t value)
{
	return static_cast<char>(value < 10 ? ('0' + value) : ('A' + (value - 10)));
}

bool NeedsEscapedTextLiteral(const std::string& text)
{
	if (text.starts_with(kEscapedTextLiteralPrefix) || text.starts_with(kEscapedLongTextLiteralPrefix)) {
		return true;
	}
	for (const unsigned char ch : text) {
		// 易语言中文引号内的反斜杠是普通字符，路径无需进入转义传输格式。
		// 真正需要转义时仍编码整个载荷，保留控制字符及历史前缀的无损回包。
		if (ch == '"' || ch == '\r' || ch == '\n' || ch == '\t' || ch < 0x20) {
			return true;
		}
	}
	return false;
}

std::string EscapeTextLiteralPayload(const std::string& text)
{
	std::string out;
	out.reserve(text.size());
	for (const unsigned char ch : text) {
		switch (ch) {
		case '\\':
			out += "\\\\";
			break;
		case '\r':
			out += "\\r";
			break;
		case '\n':
			out += "\\n";
			break;
		case '\t':
			out += "\\t";
			break;
		case '"':
			out += "\\x22";
			break;
		default:
			if (ch < 0x20) {
				out += "\\x";
				out.push_back(ToHexDigit(static_cast<std::uint8_t>((ch >> 4) & 0x0F)));
				out.push_back(ToHexDigit(static_cast<std::uint8_t>(ch & 0x0F)));
			}
			else {
				out.push_back(static_cast<char>(ch));
			}
			break;
		}
	}
	return out;
}

std::string EncodeLiteral(const std::string& rawText, const bool isLongText)
{
	if (!isLongText && !NeedsEscapedTextLiteral(rawText)) {
		return std::string(kTextLiteralLeftQuote) + rawText + kTextLiteralRightQuote;
	}

	const char* prefix = isLongText ? kEscapedLongTextLiteralPrefix : kEscapedTextLiteralPrefix;
	return std::string(kTextLiteralLeftQuote) + prefix + EscapeTextLiteralPayload(rawText) + kTextLiteralRightQuote;
}

int ParseHexNibble(const char ch)
{
	if (ch >= '0' && ch <= '9') {
		return ch - '0';
	}
	if (ch >= 'a' && ch <= 'f') {
		return ch - 'a' + 10;
	}
	if (ch >= 'A' && ch <= 'F') {
		return ch - 'A' + 10;
	}
	return -1;
}

std::string DecodePayload(const std::string& text)
{
	std::string out;
	out.reserve(text.size());
	for (size_t i = 0; i < text.size(); ++i) {
		if (text[i] != '\\') {
			out.push_back(text[i]);
			continue;
		}
		if (i + 1 >= text.size()) {
			out.push_back('\\');
			break;
		}

		const char next = text[++i];
		switch (next) {
		case '\\':
			out.push_back('\\');
			break;
		case 'r':
			out.push_back('\r');
			break;
		case 'n':
			out.push_back('\n');
			break;
		case 't':
			out.push_back('\t');
			break;
		case 'x':
			if (i + 2 < text.size()) {
				const int high = ParseHexNibble(text[i + 1]);
				const int low = ParseHexNibble(text[i + 2]);
				if (high >= 0 && low >= 0) {
					out.push_back(static_cast<char>((high << 4) | low));
					i += 2;
					break;
				}
			}
			out += "\\x";
			break;
		default:
			out.push_back(next);
			break;
		}
	}
	return out;
}

namespace {
bool At(const std::string& text, size_t at, std::string_view token)
{
    return std::string_view(text).substr(at).starts_with(token);
}

// 本地源码采用系统 ANSI 编码；引号只能在字符边界识别，不能跨两个汉字匹配。
size_t NextCharacter(const std::string& text, size_t at)
{
    return at + (IsDBCSLeadByteEx(CP_ACP, static_cast<BYTE>(text[at])) && at + 1 < text.size() ? 2 : 1);
}

size_t FindClosingQuote(const std::string& text, size_t start)
{
    for (size_t at = start; at < text.size(); at = NextCharacter(text, at))
        if (At(text, at, kTextLiteralRightQuote)) return at;
    return std::string::npos;
}

bool Declaration(const std::string& text, size_t at)
{
    while (at < text.size() && (text[at] == ' ' || text[at] == '\t')) ++at;
    for (const std::string_view name : {".版本", ".支持库", ".程序集变量", ".程序集", ".子程序", ".参数", ".局部变量"}) {
        if (At(text, at, name)) {
            const auto end = at + name.size();
            if (end == text.size() || text[end] == ' ' || text[end] == '\t' || text[end] == '\r' || text[end] == '\n')
                return true;
        }
    }
    return false;
}

size_t LineBreaks(std::string_view text)
{
    size_t count = 0;
    for (size_t i = 0; i < text.size(); ++i) {
        if (text[i] == '\r') {
            ++count;
            if (i + 1 < text.size() && text[i + 1] == '\n') ++i;
        }
        else if (text[i] == '\n') ++count;
    }
    return count;
}

// 同一词法扫描器用于两个方向：字符串内部字节原样计数，注释和声明不解释。
bool Transform(const std::string& input, bool render, ParsedProgramText& output)
{
    output = {};
    output.sourceLines.push_back(1);
    size_t sourceLine = 1;
    size_t maskQuote = std::string::npos;
    bool lineStart = true, declaration = false, comment = false, asciiQuote = false;
    for (size_t i = 0; i < input.size();) {
        if (lineStart) {
            declaration = Declaration(input, i);
            comment = false;
            asciiQuote = false;
            lineStart = false;
            auto prefix = i;
            while (prefix < input.size() && (input[prefix] == ' ' || input[prefix] == '\t')) ++prefix;
            maskQuote = At(input, prefix, ".' ") ? prefix + 1 : std::string::npos;
            // 整行载荷仍有自己的语法；不能把它误当作普通表达式的字符串。
            if (At(input, prefix, "#e2txt_body_line#") || At(input, prefix, "' #e2txt_body_line#")) {
                if (render) return false;
                declaration = true;
            }
        }
        const char ch = input[i];
        if (ch == '\r' || ch == '\n') {
            if (ch == '\r' && i + 1 < input.size() && input[i + 1] == '\n') ++i;
            ++i;
            ++sourceLine;
            output.canonical += "\r\n";
            output.sourceLines.push_back(sourceLine);
            lineStart = true;
            continue;
        }
        if (!declaration && !comment && !asciiQuote && At(input, i, kTextLiteralLeftQuote)) {
            const size_t start = i + std::strlen(kTextLiteralLeftQuote);
            const size_t end = FindClosingQuote(input, start);
            if (end == std::string::npos) {
                // 未闭合源码仍交由预检报告；不要补引号或吞掉其余语句。
                if (render) return false;
                declaration = true;
            }
            else {
                const std::string payload = input.substr(start, end - start);
                sourceLine += LineBreaks(payload);
                if (render) {
                    if (payload.starts_with(kEscapedLongTextLiteralPrefix)) return false;
                    const std::string value = payload.starts_with(kEscapedTextLiteralPrefix)
                        ? DecodePayload(payload.substr(std::strlen(kEscapedTextLiteralPrefix))) : payload;
                    // NUL 和嵌入的结束引号没有可逆的裸字面量形式，不能静默损坏。
                    if (value.find('\0') != std::string::npos || FindClosingQuote(value, 0) != std::string::npos)
                        return false;
                    output.canonical += std::string(kTextLiteralLeftQuote) + value + kTextLiteralRightQuote;
                }
                else {
                    output.canonical += EncodeLiteral(payload);
                }
                i = end + std::strlen(kTextLiteralRightQuote);
                continue;
            }
        }
        if (!declaration && !comment) {
            if (ch == '"') asciiQuote = !asciiQuote;
            else if (ch == '\'' && !asciiQuote && i != maskQuote) comment = true;
        }
        const auto next = NextCharacter(input, i);
        output.canonical.append(input, i, next - i);
        i = next;
    }
    return true;
}
} // namespace

bool RenderIdeProgram(const std::string& canonical, std::string& displayed)
{
    ParsedProgramText result;
    if (!Transform(canonical, true, result)) return false;
    displayed = std::move(result.canonical);
    return true;
}

ParsedProgramText ParseIdeProgram(const std::string& displayed)
{
    ParsedProgramText result;
    (void)Transform(displayed, false, result);
    return result;
}

} // namespace e2txt::source_text

