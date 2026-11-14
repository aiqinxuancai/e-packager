#pragma once
#include <string>

namespace e2txt::source_text {
// 使用同一原生语法树的内部与 IDE 显示视图恢复身份；歧义拒绝，不猜测符号 ID。
bool RestoreIdentityProgram(const std::string& origin, const std::string& displayedOrigin,
    const std::string& edited, std::string& canonical, std::string& error);
}
