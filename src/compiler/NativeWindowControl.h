#pragma once

#include <string_view>

namespace ecompiler {

// 独立窗口编译器实际创建的控件必须能映射到一个明确的 Win32 窗口类。
// 未列出的支持库窗口组件（例如数据提供者、表格和网络组件）不进入窗口模型。
struct NativeWindowControlMapping {
	std::string_view easyName;
	std::string_view token;
};

// 这些类型虽然会出现在支持库类型表中，但不是本编译器可以安全创建的
// 独立 Win32 窗口。单独维护排除表，避免将来扩充映射表时意外重新启用它们。
inline constexpr std::string_view kUnsupportedWindowControlNames[] = {
	"数据报",
	"客户",
	"服务器",
	"端口",
	"表格",
	"数据源",
	"通用提供者",
	"数据库提供者",
	"图形按钮",
	"外部数据库",
	"外部数据提供者",
};

inline constexpr bool IsExplicitlyUnsupportedWindowControl(const std::string_view easyName) noexcept
{
	for (const auto name : kUnsupportedWindowControlNames) {
		if (name == easyName) return true;
	}
	return false;
}

inline constexpr NativeWindowControlMapping kNativeWindowControlMappings[] = {
	{ "编辑框", "edit" },
	{ "图片框", "image" },
	{ "外形框", "shape" },
	{ "画板", "canvas" },
	{ "分组框", "group" },
	{ "标签", "label" },
	{ "按钮", "button" },
	{ "选择框", "checkbox" },
	{ "单选框", "radio" },
	{ "组合框", "combo" },
	{ "列表框", "list" },
	{ "选择列表框", "checklist" },
	{ "横向滚动条", "hscroll" },
	{ "纵向滚动条", "vscroll" },
	{ "进度条", "progress" },
	{ "滑块条", "trackbar" },
	{ "选择夹", "tab" },
	{ "影像框", "animate" },
	{ "日期框", "date" },
	{ "月历", "month" },
	{ "驱动器框", "drive" },
	{ "目录框", "directory" },
	{ "文件框", "file" },
	{ "颜色选择器", "color" },
	{ "超级链接框", "hyperlink" },
	{ "调节器", "spin" },
};

inline constexpr std::string_view NativeWindowControlToken(const std::string_view easyName) noexcept
{
	if (IsExplicitlyUnsupportedWindowControl(easyName)) return {};
	for (const auto& mapping : kNativeWindowControlMappings) {
		if (mapping.easyName == easyName) return mapping.token;
	}
	return {};
}

inline constexpr bool HasNativeWin32Class(const std::string_view easyName) noexcept
{
	return !NativeWindowControlToken(easyName).empty();
}

}  // namespace ecompiler
