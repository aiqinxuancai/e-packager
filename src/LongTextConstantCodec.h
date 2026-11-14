#pragma once
#include <functional>
#include <string>
#include <vector>

namespace e2txt::source_text {
// 长文本常量的内容独立保存；代码表只显示 IDE 的本地编码字节数。
struct LongTextConstant {
    std::string name;
    std::string value;
};
void RenderLongTextConstants(const std::string& canonical, std::string& displayed,
    std::vector<LongTextConstant>& values);
// 按常量名显式绑定完整文本；缺失内容或更改显示长度不得被解释为空格填充。
using ReadLongTextConstant = std::function<bool(const std::string& name,
    const std::string& placeholder, std::string& value, std::string& error)>;
bool RestoreLongTextConstants(const std::string& displayed, const ReadLongTextConstant& read,
    std::string& canonical, std::string& error);
} // namespace e2txt::source_text
