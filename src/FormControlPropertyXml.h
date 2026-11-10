#pragma once

#include "FormControlPropertyCodec.h"
#include "SimpleXmlDocument.h"

#include <string>
#include <utility>
#include <vector>

namespace e2txt {

// 把窗口 XML 节点转换为属性 codec 使用的轻量节点。
inline FormControlPropertyXmlNode ToFormControlPropertyXmlNode(const SimpleXmlNode& node)
{
	FormControlPropertyXmlNode converted;
	converted.name = node.name;
	converted.attributes.assign(node.attributes.begin(), node.attributes.end());
	converted.children.reserve(node.children.size());
	for (const auto& child : node.children) {
		converted.children.push_back(ToFormControlPropertyXmlNode(child));
	}
	return converted;
}

// 收集控件节点下“标签名.属性”形式的属性子节点；事件绑定与事件定义目录不是属性。
inline std::vector<FormControlPropertyXmlNode> CollectFormControlPropertyXmlNodes(
	const SimpleXmlNode& node,
	const std::string& tagName)
{
	const std::string prefix = tagName + ".";
	std::vector<FormControlPropertyXmlNode> nodes;
	for (const auto& child : node.children) {
		if (child.name.size() > prefix.size() && child.name.compare(0, prefix.size(), prefix) == 0 &&
			child.name != prefix + "事件" && child.name != prefix + "事件定义") {
			nodes.push_back(ToFormControlPropertyXmlNode(child));
		}
	}
	return nodes;
}

// 复制控件节点的 XML 属性，供 codec 按属性名匹配。
inline std::vector<std::pair<std::string, std::string>> CopyFormControlXmlAttributes(const SimpleXmlNode& node)
{
	return { node.attributes.begin(), node.attributes.end() };
}

}  // namespace e2txt
