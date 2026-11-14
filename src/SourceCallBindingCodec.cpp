#include "SourceCallBindingCodec.h"
#include "SourceExpressionParser.h"
#include <Windows.h>
#include <algorithm>
#include <map>
#include <set>
#include <string_view>
#include <vector>

namespace e2txt::source_text {
namespace {
struct Call {
    std::string target, name;
    size_t offset, length;
};
struct Statement {
    std::string exact, shape;
    std::vector<Call> calls;
    size_t line;
    bool valid;
};
struct Method {
    std::string name;
    std::vector<Statement> statements;
};
struct Edit { size_t offset, length; std::string text; };

std::string Bare(const std::string& name)
{
    const auto at = name.rfind("::");
    return at == std::string::npos ? name : name.substr(at + 2);
}
std::string Trim(std::string value)
{
    const auto start = value.find_first_not_of(" \t\r\n");
    return start == std::string::npos ? "" : value.substr(start, value.find_last_not_of(" \t\r\n") - start + 1);
}
std::string WithoutComment(const std::string& value)
{
    bool quoted = false, ascii = false;
    for (size_t i = 0; i < value.size();) {
        const auto rest = std::string_view(value).substr(i);
        if (!ascii && rest.starts_with("“")) { quoted = true; i += std::string_view("“").size(); continue; }
        if (!ascii && rest.starts_with("”")) { quoted = false; i += std::string_view("”").size(); continue; }
        if (!quoted && value[i] == '"') ascii = !ascii;
        if (!quoted && !ascii && value[i] == '\'') return value.substr(0, i);
        i += IsDBCSLeadByteEx(CP_ACP, static_cast<BYTE>(value[i])) && i + 1 < value.size() ? 2 : 1;
    }
    return value;
}
void Fingerprint(const SourceExpressionNode& node, bool shape, bool callee, std::string& out)
{
    std::string value = callee && node.kind == SourceExpressionKind::Name ? Bare(node.text) : node.text;
    if (shape && (node.kind == SourceExpressionKind::TextLiteral || node.kind == SourceExpressionKind::NumberLiteral ||
        node.kind == SourceExpressionKind::LogicalLiteral || node.kind == SourceExpressionKind::DateTimeLiteral ||
        node.kind == SourceExpressionKind::ByteSetLiteral)) value.clear();
    out += std::to_string(static_cast<int>(node.kind)) + ":" + std::to_string(value.size()) + ":" + value + "[";
    for (size_t i = 0; i < node.children.size(); ++i)
        Fingerprint(*node.children[i], shape, node.kind == SourceExpressionKind::Call && i == 0, out);
    out += "]";
}
void Calls(const SourceExpressionNode& node, size_t base, std::vector<Call>& calls)
{
    if (node.kind == SourceExpressionKind::Call && !node.children.empty()) {
        const auto& callee = *node.children[0];
        if (callee.kind == SourceExpressionKind::Name && callee.sourceOffset != std::string::npos)
            calls.push_back({callee.text, Bare(callee.text), base + callee.sourceOffset, callee.sourceLength});
    }
    for (const auto& child : node.children) Calls(*child, base, calls);
}
std::vector<Method> Parse(const std::string& source)
{
    std::vector<Method> result(1);
    size_t line = 0;
    for (size_t start = 0; start < source.size();) {
        ++line;
        auto end = source.find('\n', start);
        if (end == std::string::npos) end = source.size();
        std::string text = WithoutComment(source.substr(start, end - start));
        size_t indent = text.find_first_not_of(" \t\r");
        if (indent != std::string::npos) {
            text = Trim(text.substr(indent));
            if (text.starts_with(".子程序 ") || text.starts_with(".子程序\t")) {
                auto name = Trim(text.substr(std::string_view(".子程序").size()));
                result.push_back({Trim(name.substr(0, name.find(','))), {}});
            }
            else {
                bool declaration = false;
                for (const std::string_view prefix : {".版本", ".支持库", ".程序集", ".参数", ".局部变量"})
                    declaration |= text.starts_with(prefix);
                if (!declaration && !text.empty()) {
                    // 流程语句的点号属于语句标记，不参与表达式解析；保留原始偏移。
                    if (text[0] == '.') { text.erase(0, 1); ++indent; }
                    const auto parsed = ParseSourceExpression(text);
                    Statement statement{{}, {}, {}, line, parsed.IsValid()};
                    if (statement.valid) {
                        Fingerprint(*parsed.root, false, false, statement.exact);
                        Fingerprint(*parsed.root, true, false, statement.shape);
                        Calls(*parsed.root, start + indent, statement.calls);
                    }
                    else statement.exact = statement.shape = "unparsed:" + text;
                    result.back().statements.push_back(std::move(statement));
                }
            }
        }
        start = end + (end < source.size());
    }
    return result;
}
std::string Apply(const std::string& text, std::vector<Edit>& edits)
{
    std::sort(edits.begin(), edits.end(), [](const Edit& a, const Edit& b) { return a.offset > b.offset; });
    auto result = text;
    for (const auto& edit : edits) result.replace(edit.offset, edit.length, edit.text);
    return result;
}
std::string Body(const Method& method, bool shape)
{
    std::string result;
    for (const auto& statement : method.statements) result += (shape ? statement.shape : statement.exact) + "\n";
    return result;
}
std::string Signature(const Statement& statement)
{
    std::string result;
    for (const auto& call : statement.calls) result += call.target + "\n";
    return result;
}
bool Sensitive(const Statement& statement, const std::set<std::string>& names)
{
    for (const auto& call : statement.calls)
        if (call.target == call.name && names.contains(call.name)) return true;
    return false;
}
} // namespace

bool RenderCallBindings(const std::string& canonical, std::string& displayed)
{
    std::vector<Edit> edits;
    for (const auto& method : Parse(canonical))
        for (const auto& statement : method.statements)
            for (const auto& call : statement.calls)
                if (call.name != call.target) edits.push_back({call.offset, call.length, call.name});
    displayed = Apply(canonical, edits);
    return !edits.empty();
}

bool RestoreCallBindings(const std::string& origin, const std::string& edited,
    std::string& canonical, std::string& error)
{
    const auto before = Parse(origin), after = Parse(edited);
    std::set<std::string> names;
    for (const auto& method : before)
        for (const auto& statement : method.statements)
            for (const auto& call : statement.calls)
                if (call.target != call.name) names.insert(call.name);
    if (names.empty()) { error = "call_binding_origin_has_no_bindings"; return false; }
    std::vector<Edit> edits;
    for (const auto& method : after) {
        const Method* old = nullptr;
        // 优先稳定的方法名；改名只接受唯一相同的语法树，不跨方法猜测同名调用。
        for (const auto& candidate : before) if (candidate.name == method.name) {
            if (old) { error = "duplicate_call_binding_method:" + method.name; return false; }
            old = &candidate;
        }
        if (!old) {
            for (const auto& candidate : before) if (Body(candidate, true) == Body(method, true)) {
                if (old) { old = nullptr; break; }
                old = &candidate;
            }
        }
        const bool sameBody = old && Body(*old, false) == Body(method, false);
        std::set<size_t> used;
        for (size_t i = 0; i < method.statements.size(); ++i) {
            const auto& statement = method.statements[i];
            if (!statement.valid) {
                // 原有不可解析载荷可原样保留；新增载荷不得绕过绑定恢复。
                if (old && std::any_of(old->statements.begin(), old->statements.end(), [&](const auto& s) { return s.exact == statement.exact; })) continue;
                error = "call_binding_unparsed_statement:" + method.name + ":" + std::to_string(statement.line);
                return false;
            }
            if (!Sensitive(statement, names)) continue;
            const Statement* matched = nullptr;
            size_t chosen = 0;
            if (sameBody) { chosen = i; matched = &old->statements[i]; }
            else if (old) {
                for (const bool shape : {false, true}) {
                    std::vector<size_t> candidates;
                    for (size_t j = 0; j < old->statements.size(); ++j)
                        if (!used.contains(j) && (shape ? old->statements[j].shape == statement.shape : old->statements[j].exact == statement.exact)) candidates.push_back(j);
                    if (candidates.empty()) continue;
                    const auto signature = Signature(old->statements[candidates[0]]);
                    for (const auto j : candidates) if (Signature(old->statements[j]) != signature) {
                        error = "ambiguous_call_binding:" + method.name + ":" + std::to_string(statement.line) + ":use_explicit_library_qualifier";
                        return false;
                    }
                    chosen = candidates[0]; matched = &old->statements[chosen]; break;
                }
            }
            if (!matched || matched->calls.size() != statement.calls.size()) {
                error = "unmatched_call_binding:" + method.name + ":" + std::to_string(statement.line) + ":use_explicit_library_qualifier";
                return false;
            }
            used.insert(chosen);
            for (size_t j = 0; j < statement.calls.size(); ++j) {
                const auto& call = statement.calls[j];
                const auto& previous = matched->calls[j];
                if (call.name != previous.name) { error = "inconsistent_call_binding"; return false; }
                if (call.target == call.name && previous.target != previous.name)
                    edits.push_back({call.offset, call.length, previous.target});
            }
        }
    }
    canonical = Apply(edited, edits);
    return true;
}
} // namespace e2txt::source_text
