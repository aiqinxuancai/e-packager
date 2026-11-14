#pragma once
#include <string>

namespace e2txt::source_text {
// 目录显示隐藏调用限定符；绑定来源独立保存，回包时恢复到语义编译输入。
bool RenderCallBindings(const std::string& canonical, std::string& displayed);
// 仅按同一方法的语句语法树对应绑定；不能唯一对应时要求显式限定调用。
bool RestoreCallBindings(const std::string& origin, const std::string& edited,
    std::string& canonical, std::string& error);
} // namespace e2txt::source_text
