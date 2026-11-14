#include "LongTextConstantCodec.h"
#include "SourceTextCodec.h"
#include <string_view>

namespace e2txt::source_text {
namespace {
struct ConstantField {
    std::string name;
    size_t begin = 0;
    size_t end = 0;
};
std::string Trim(std::string_view text)
{
    const auto begin = text.find_first_not_of(" \t");
    if (begin == std::string_view::npos) return {};
    return std::string(text.substr(begin, text.find_last_not_of(" \t") - begin + 1));
}
// 只定位常量名与外层 ASCII 引号值；备注中的逗号和引号无需重新格式化。
bool FindValue(const std::string& line, ConstantField& field)
{
    size_t at = line.find_first_not_of(" \t");
    const std::string_view directive = ".常量";
    if (at == std::string::npos || !std::string_view(line).substr(at).starts_with(directive)) return false;
    at += directive.size();
    if (at == line.size() || (line[at] != ' ' && line[at] != '\t')) return false;
    const auto comma = line.find(',', at);
    if (comma == std::string::npos) return false;
    field.name = Trim(std::string_view(line).substr(at, comma - at));
    field.begin = line.find_first_not_of(" \t", comma + 1);
    if (field.begin == std::string::npos || line[field.begin] != '"') return false;
    field.end = line.find('"', field.begin + 1);
    if (field.end == std::string::npos) return false;
    ++field.end;
    return !field.name.empty();
}

template<class Convert>
bool ConvertTable(const std::string& input, std::string& output, Convert&& convert)
{
    output.clear();
    for (size_t at = 0; at < input.size();) {
        auto end = input.find_first_of("\r\n", at);
        if (end == std::string::npos) end = input.size();
        std::string line = input.substr(at, end - at);
        ConstantField field;
        if (FindValue(line, field) && !convert(line, field)) return false;
        output += line;
        at = end;
        if (at < input.size()) {
            output.push_back(input[at++]);
            if (input[at - 1] == '\r' && at < input.size() && input[at] == '\n') output.push_back(input[at++]);
        }
    }
    return true;
}
} // namespace

void RenderLongTextConstants(const std::string& canonical, std::string& displayed,
    std::vector<LongTextConstant>& values)
{
    values.clear();
    ConvertTable(canonical, displayed, [&](std::string& line, const ConstantField& field) {
        const auto literal = std::string_view(line).substr(field.begin + 1, field.end - field.begin - 2);
        const std::string_view prefix = "“#e2txt_long_text#";
        const std::string_view suffix = "”";
        if (!literal.starts_with(prefix) || !literal.ends_with(suffix)) return true;
        auto value = DecodePayload(std::string(literal.substr(prefix.size(), literal.size() - prefix.size() - suffix.size())));
        line.replace(field.begin, field.end - field.begin, "\"<文本长度: " + std::to_string(value.size()) + ">\"");
        values.push_back({field.name, std::move(value)});
        return true;
    });
}

bool RestoreLongTextConstants(const std::string& displayed, const ReadLongTextConstant& read,
    std::string& canonical, std::string& error)
{
    return ConvertTable(displayed, canonical, [&](std::string& line, const ConstantField& field) {
        const auto literal = line.substr(field.begin + 1, field.end - field.begin - 2);
        if (!literal.starts_with("<文本长度:") || !literal.ends_with(">")) return true;
        std::string value;
        if (!read(field.name, literal, value, error)) return false;
        line.replace(field.begin, field.end - field.begin, "\"" + EncodeLiteral(value, true) + "\"");
        return true;
    });
}
} // namespace e2txt::source_text
