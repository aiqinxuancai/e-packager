#pragma once

#include <cstdint>
#include <string>

namespace e2txt {
// 原生系统类型和核心支持库类型的稳定名称。
inline std::string GetBuiltinTypeName(std::int32_t typeValue)
{
	switch (typeValue) {
	case 0: return "";
	case -1: return "";
	case static_cast<std::int32_t>(0x80000000u): return "通用型";
	case static_cast<std::int32_t>(0x80000101u): return "字节型";
	case static_cast<std::int32_t>(0x80000201u): return "短整数型";
	case static_cast<std::int32_t>(0x80000301u): return "整数型";
	case static_cast<std::int32_t>(0x80000401u): return "长整数型";
	case static_cast<std::int32_t>(0x80000501u): return "小数型";
	case static_cast<std::int32_t>(0x80000601u): return "双精度小数型";
	case static_cast<std::int32_t>(0x80000002u): return "逻辑型";
	case static_cast<std::int32_t>(0x80000003u): return "日期时间型";
	case static_cast<std::int32_t>(0x80000004u): return "文本型";
	case static_cast<std::int32_t>(0x80000005u): return "字节集";
	case static_cast<std::int32_t>(0x80000006u): return "子程序指针";
	case static_cast<std::int32_t>(0x80000008u): return "条件语句型";
	case 65537: return "窗口";
	case 65539: return "菜单";
	case 65540: return "字体";
	case 65541: return "编辑框";
	case 65542: return "图片框";
	case 65543: return "外形框";
	case 65544: return "画板";
	case 65545: return "分组框";
	case 65546: return "标签";
	case 65547: return "按钮";
	case 65548: return "选择框";
	case 65549: return "单选框";
	case 65550: return "组合框";
	case 65551: return "列表框";
	case 65552: return "选择列表框";
	case 65553: return "横向滚动条";
	case 65554: return "纵向滚动条";
	case 65555: return "进度条";
	case 65556: return "滑块条";
	case 65557: return "选择夹";
	case 65558: return "影像框";
	case 65559: return "日期框";
	case 65560: return "月历";
	case 65561: return "驱动器框";
	case 65562: return "目录框";
	case 65563: return "文件框";
	case 65564: return "颜色选择器";
	case 65565: return "超级链接框";
	case 65566: return "调节器";
	case 65567: return "通用对话框";
	case 65568: return "时钟";
	case 65569: return "打印机";
	case 65570: return "字段信息";
	case 65572: return "数据报";
	case 65573: return "客户";
	case 65574: return "服务器";
	case 65575: return "端口";
	case 65576: return "打印设置信息";
	case 65577: return "表格";
	case 65578: return "数据源";
	case 65579: return "通用提供者";
	case 65580: return "数据库提供者";
	case 65581: return "图形按钮";
	case 65582: return "外部数据库";
	case 65583: return "外部数据提供者";
	case 65584: return "对象";
	case 65585: return "变体型";
	case 65586: return "变体类型";
	default: return std::string();
	}
}

}
