#pragma once
#include <array>
#include <string_view>

namespace e2txt {
// e5.95.exe 原生通用事件表（0x66dda8），索引为 -(表内位置 + 1)。
inline constexpr std::array<std::string_view, 12> kCommonWindowEvents = {
    "鼠标左键被按下", "鼠标左键被放开", "被双击", "鼠标右键被按下",
    "鼠标右键被放开", "鼠标位置被移动", "获得焦点", "失去焦点",
    "按下某键", "放开某键", "字符输入", "滚轮被滚动"
};
} // namespace e2txt
