#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>

namespace e2txt {

// 拆包显示与语义回包共用的调用身份；参数范围用于选择候选，不负责类型检查。
struct CallableSymbol {
    std::int16_t libraryId = 0;
    std::int32_t methodId = 0;
    std::int32_t returnType = 0;
    bool qualified = false;
    std::int32_t minimumArguments = 0;
    std::int32_t maximumArguments = -1;
};

// 对象成员仅在所属类及其继承链可见，普通/窗口程序集方法放入全局表。
struct CallableBindingTables {
    std::unordered_map<std::string, CallableSymbol> functionsByName;
    std::unordered_map<std::int32_t, std::unordered_map<std::string, CallableSymbol>> methodsByOwnerType;
    std::unordered_map<std::int32_t, std::int32_t> baseTypes;
};

inline bool AcceptsCallableArguments(const CallableSymbol& symbol, const std::optional<size_t> count)
{
    return !count || (*count >= static_cast<size_t>(symbol.minimumArguments) &&
        (symbol.maximumArguments < 0 || *count <= static_cast<size_t>(symbol.maximumArguments)));
}

// 名称已由调用方规范化；支持库/窗口命令由其元数据提供者查询。
// 没有参数匹配候选时仍保留声明身份，历史非法实参交由 IDE 编译诊断。
template<class FindSupport>
std::optional<CallableSymbol> BindCallable(const CallableBindingTables& tables,
    const std::string& name, std::int32_t owner, std::optional<size_t> argumentCount,
    FindSupport&& findSupport)
{
    std::optional<CallableSymbol> declared;
    std::unordered_set<std::int32_t> visited;
    for (; owner > 0 && visited.insert(owner).second;) {
        if (const auto members = tables.methodsByOwnerType.find(owner); members != tables.methodsByOwnerType.end()) {
            if (const auto method = members->second.find(name); method != members->second.end()) {
                if (!declared) declared = method->second;
                if (AcceptsCallableArguments(method->second, argumentCount)) return method->second;
            }
        }
        const auto base = tables.baseTypes.find(owner);
        if (base == tables.baseTypes.end()) break;
        owner = base->second;
    }
    const auto global = tables.functionsByName.find(name);
    if (global != tables.functionsByName.end() && AcceptsCallableArguments(global->second, argumentCount))
        return global->second;
    if (auto support = findSupport()) return support;
    if (declared) return declared;
    if (global != tables.functionsByName.end()) return global->second;
    return std::nullopt;
}

} // namespace e2txt
