#pragma once

#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace e2txt {
// 声明注释中的 $(...) 由 IDE 按编译配置选择；不同分支保留独立声明。
inline std::string DeclarationCondition(std::string_view comment)
{
    std::string result;
    size_t offset = 0;
    while ((offset = comment.find("$(", offset)) != std::string_view::npos) {
        const auto end = comment.find(')', offset + 2);
        if (end == std::string_view::npos) break;
        result.append(comment.substr(offset, end + 1 - offset));
        offset = end + 1;
    }
    return result;
}

// 备注中的逗号不属于声明槽位，恢复其完整尾部再解析条件。
inline std::string DeclarationComment(const std::vector<std::string>& fields, size_t start)
{
    std::string text;
    for (size_t index = start; index < fields.size(); ++index) {
        if (index != start) text += ',';
        text += fields[index];
    }
    return text;
}

// 无条件重复、相同分支重复均拒绝；不同条件分支交由 IDE 所选宏配置解析。
class ConditionalDeclarationNames {
public:
    bool Insert(const std::string& name, std::string_view comment)
    {
        const auto condition = DeclarationCondition(comment);
        auto& existing = m_names[name];
        bool valid = true;
        for (const auto& other : existing)
            if (condition.empty() || other.empty() || condition == other) valid = false;
        existing.push_back(condition);
        return valid;
    }
private:
    std::unordered_map<std::string, std::vector<std::string>> m_names;
};
} // namespace e2txt
