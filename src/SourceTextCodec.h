#pragma once

#include <cstddef>
#include <string>
#include <vector>

namespace e2txt::source_text {

// 内部文本字面量编码集中维护；目录原生文本不把此前缀当成用户转义指令。
std::string EncodeLiteral(const std::string& text, bool longText = false);
std::string DecodePayload(const std::string& text);

// 只转换可执行语句中的文本，声明备注与注释保持原文。
// 不能无歧义表示的原生值明确返回 false，调用方保留该页的旧格式标记。
bool RenderIdeProgram(const std::string& canonical, std::string& displayed);

struct ParsedProgramText {
    std::string canonical;
    // 内部物理行到目录原始物理行的一对一来源；多行字面量属于其起始行。
    std::vector<std::size_t> sourceLines;
};

// 目录原生文本转为内部可解析文本；不规范化字面量内部的 CR/LF/制表符。
ParsedProgramText ParseIdeProgram(const std::string& displayed);

} // namespace e2txt::source_text
