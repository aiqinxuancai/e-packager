#include "SourceIdentityCodec.h"
#include <Windows.h>
#include <algorithm>
#include <cctype>
#include <set>
#include <string_view>
#include <vector>

namespace e2txt::source_text {
namespace {
struct Token { std::string text; size_t begin, end; int kind; };
struct Line { std::string canonical, displayed; std::vector<Token> tokens; bool changed = false; };
struct Method { std::string name; std::vector<Line> lines; };
struct Flow { std::string end; bool masked; };

std::string Trim(std::string s)
{
    const auto begin = s.find_first_not_of(" \t\r\n");
    return begin == std::string::npos ? "" : s.substr(begin, s.find_last_not_of(" \t\r\n") - begin + 1);
}
bool At(const std::string& s, size_t i, std::string_view token) { return std::string_view(s).substr(i).starts_with(token); }
std::vector<Token> Tokens(const std::string& s)
{
    std::vector<Token> out;
    for (size_t i = 0; i < s.size();) {
        if (std::isspace(static_cast<unsigned char>(s[i]))) { ++i; continue; }
        const auto start = i;
        int kind = 0;
        if (At(s, i, "“") || s[i] == '"') {
            const bool smart = s[i] != '"';
            const std::string end = smart ? "”" : "\"";
            i += smart ? std::string_view("“").size() : 1;
            while (i < s.size() && !At(s, i, end))
                i += IsDBCSLeadByteEx(CP_ACP, static_cast<BYTE>(s[i])) && i + 1 < s.size() ? 2 : 1;
            if (i < s.size()) i += end.size();
            kind = 1;
        }
        else if (s[i] == '\'' && !(i == 1 && s[0] == '.')) { i = s.size(); kind = 2; }
        else if (std::isdigit(static_cast<unsigned char>(s[i]))) {
            while (i < s.size() && (std::isalnum(static_cast<unsigned char>(s[i])) || s[i] == '.')) ++i;
            kind = 1;
        }
        else if (std::isalpha(static_cast<unsigned char>(s[i])) || s[i] == '_' || static_cast<unsigned char>(s[i]) >= 128) {
            while (i < s.size()) {
                const auto c = static_cast<unsigned char>(s[i]);
                if (IsDBCSLeadByteEx(CP_ACP, c) && i + 1 < s.size()) { i += 2; continue; }
                if (!std::isalnum(c) && c != '_') break;
                ++i;
            }
            if (i == start) ++i;
        }
        else ++i;
        out.push_back({s.substr(start, i - start), start, i, kind});
    }
    return out;
}
std::string ControlDisplay(std::string line)
{
    if (line.starts_with("' .判断开始 (")) return ".判断开始断 (" + line.substr(std::string_view("' .判断开始 (").size());
    if (line.starts_with("' .")) {
        const auto name = line.substr(3, line.find(' ', 3) - 3);
        static const std::set<std::string> calls = {"如果", "如果真", "判断", "判断循环首", "判断循环尾", "循环判断首", "循环判断尾", "计次循环首", "计次循环尾", "变量循环首", "变量循环尾"};
        if (calls.contains(name)) return ".' " + line.substr(3);
    }
    return line;
}
std::string ControlCanonical(std::string line)
{
    if (line.starts_with(".判断开始断 (")) return "' .判断开始 (" + line.substr(std::string_view(".判断开始断 (").size());
    if (line.starts_with(".' ")) return "' ." + line.substr(3);
    return line;
}
std::string Fingerprint(const std::vector<Token>& tokens, bool shape)
{
    std::string out;
    for (const auto& token : tokens) {
        const auto value = shape && token.kind != 0 ? "#" : token.text;
        out += std::to_string(token.kind) + ":" + std::to_string(value.size()) + ":" + value + ";";
    }
    return out;
}
std::vector<std::string> Lines(const std::string& text)
{
    std::vector<std::string> result;
    for (size_t start = 0; start < text.size();) {
        auto end = text.find('\n', start);
        if (end == std::string::npos) end = text.size();
        result.push_back(Trim(text.substr(start, end - start)));
        start = end + (end < text.size());
    }
    return result;
}
std::vector<Method> Parse(const std::vector<std::string>& source, const std::vector<std::string>& display)
{
    std::vector<Method> result(1);
    for (size_t i = 0; i < source.size(); ++i) {
        if (source[i].starts_with(".子程序 ")) {
            auto name = Trim(source[i].substr(std::string_view(".子程序 ").size()));
            result.push_back({Trim(name.substr(0, name.find(','))), {}});
        }
        result.back().lines.push_back({source[i], display[i], Tokens(display[i]), source[i] != display[i]});
    }
    return result;
}
bool Reconstruct(const Line& old, const std::string& edited, std::string& result)
{
    const auto current = Tokens(edited);
    if (Fingerprint(current, true) != Fingerprint(old.tokens, true)) return false;
    // 仅替换字面量及注释，原生身份和流程结构仍取自已唯一对应的语句。
    const bool control = old.canonical.starts_with("' .") && (old.displayed.starts_with(".' ") || old.displayed.starts_with(".判断开始断 ("));
    const auto lexical = control ? ControlDisplay(old.canonical) : old.canonical;
    auto canonicalTokens = Tokens(lexical);
    std::vector<Token> originalValues, currentValues;
    for (const auto& t : canonicalTokens) if (t.kind != 0) originalValues.push_back(t);
    for (const auto& t : current) if (t.kind != 0) currentValues.push_back(t);
    if (originalValues.size() != currentValues.size()) return false;
    result = lexical;
    for (size_t i = originalValues.size(); i-- > 0;)
        result.replace(originalValues[i].begin, originalValues[i].end - originalValues[i].begin, currentValues[i].text);
    if (control) result = ControlCanonical(result);
    return true;
}
bool Boundary(const std::string& text)
{
    return text == ".如果真结束" || text == ".如果结束" || text == ".否则" ||
        text == ".默认" || text == ".判断结束";
}
std::string EmptyDeclarationType(const std::string& text)
{
    const auto space = text.find(' ');
    const auto kind = text.substr(0, space);
    if (kind != ".参数" && kind != ".局部变量" && kind != ".程序集变量" &&
        kind != ".全局变量" && kind != ".成员" && kind != ".子程序" && kind != ".程序集") return {};
    const auto comma = text.find(',');
    if (comma == std::string::npos) return kind;
    const auto end = text.find(',', comma + 1);
    return Trim(text.substr(comma + 1, end - comma - 1)).empty() ? kind : std::string{};
}
bool HasHiddenIdentity(const Line& line)
{
    return line.changed && !Boundary(line.displayed) && ControlDisplay(line.canonical) != line.displayed;
}
std::string RestoreFlowMask(const std::string& displayed, std::string restored, std::vector<Flow>& stack)
{
    // 结束标记的停用状态来自当前流程结构；不能按相同的结束行文字跨块匹配。
    if (displayed.starts_with(".子程序 ")) stack.clear();
    const bool masked = displayed.starts_with(".' ") || displayed.starts_with(".判断开始断 (");
    auto code = ControlCanonical(displayed);
    if (masked) code.erase(0, 2);
    const std::pair<const char*, const char*> pairs[] = {
        {".如果真 (", ".如果真结束"}, {".如果 (", ".如果结束"},
        {".判断开始 (", ".判断结束"}, {".判断循环首 (", ".判断循环尾"},
        {".循环判断首 (", ".循环判断尾"}, {".计次循环首 (", ".计次循环尾"},
        {".变量循环首 (", ".变量循环尾"}
    };
    for (const auto& [begin, end] : pairs) if (code.starts_with(begin)) {
        stack.push_back({end, masked});
        return restored;
    }
    if (stack.empty()) return restored;
    const auto top = stack.back();
    if (code == ".否则" && top.end == ".如果结束") return top.masked ? "' " + code : code;
    if (code == ".默认" && top.end == ".判断结束") return top.masked ? "' " + code : code;
    if (code == top.end || code.starts_with(top.end + " (")) {
        stack.pop_back();
        if (Boundary(code)) return top.masked ? "' " + code : code;
    }
    return restored;
}
}

bool RestoreIdentityProgram(const std::string& origin, const std::string& displayedOrigin,
    const std::string& edited, std::string& canonical, std::string& error)
{
    const auto source = Lines(origin), display = Lines(displayedOrigin);
    if (source.size() != display.size()) { error = "native_identity_line_count_mismatch"; return false; }
    if (edited == displayedOrigin) { canonical = origin; return true; }
    const auto methods = Parse(source, display);
    const Method* method = &methods.front();
    std::vector<Flow> flow;
    canonical.clear();
    for (size_t start = 0; start < edited.size();) {
        auto end = edited.find('\n', start);
        if (end == std::string::npos) end = edited.size();
        auto line = edited.substr(start, end - start);
        const bool cr = !line.empty() && line.back() == '\r';
        if (cr) line.pop_back();
        const auto indent = line.find_first_not_of(" \t");
        if (indent != std::string::npos) {
            const auto body = line.substr(indent);
            if (body.starts_with(".子程序 ")) {
                auto name = Trim(body.substr(std::string_view(".子程序 ").size()));
                name = Trim(name.substr(0, name.find(',')));
                method = nullptr;
                for (const auto& item : methods) if (item.name == name) {
                    if (method) { error = "ambiguous_identity_method:" + name; return false; }
                    method = &item;
                }
            }
            std::set<std::string> candidates;
            const auto tokens = Tokens(body);
            if (method) for (const bool shape : {false, true}) {
                for (const auto& old : method->lines) {
                    if (!old.changed || Boundary(old.displayed) || Fingerprint(old.tokens, shape) != Fingerprint(tokens, shape)) continue;
                    std::string rebuilt;
                    if (Reconstruct(old, body, rebuilt)) candidates.insert(rebuilt);
                }
                if (!candidates.empty()) break;
            }
            if (candidates.size() > 1) { error = "ambiguous_native_identity:" + (method ? method->name : ""); return false; }
            if (candidates.empty()) {
                // 空白不是可跨作用域搬运的符号名。声明改名或语句移至另一方法时，
                // 无法证明身份对应就要求显式填写，不能静默回退为默认类型或新引用。
                const auto emptyType = EmptyDeclarationType(body);
                for (const auto& owner : methods) for (const auto& old : owner.lines) {
                    if (!HasHiddenIdentity(old)) continue;
                    const bool moved = &owner != method && Fingerprint(old.tokens, true) == Fingerprint(tokens, true);
                    const bool declaration = !emptyType.empty() && (method == nullptr || &owner == method) &&
                        EmptyDeclarationType(old.displayed) == emptyType;
                    if (moved || declaration) {
                        error = "unmatched_native_identity:" + owner.name;
                        return false;
                    }
                }
            }
            const auto restored = candidates.empty() ? ControlCanonical(body) : *candidates.begin();
            line = line.substr(0, indent) + RestoreFlowMask(body, restored, flow);
        }
        canonical += line;
        if (cr) canonical += '\r';
        if (end < edited.size()) canonical += '\n';
        start = end + (end < edited.size());
    }
    return true;
}
}
