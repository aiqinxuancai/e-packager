#pragma once

#include <cstddef>
#include <string>
#include <unordered_map>
#include <vector>

namespace e2txt {

// 易语言窗口文件使用的轻量 XML 节点。
struct SimpleXmlNode {
	std::string name;
	std::unordered_map<std::string, std::string> attributes;
	std::vector<SimpleXmlNode> children;
};

// XML 解析错误，lineIndex 使用从零开始的行号。
struct SimpleXmlParseError {
	std::string code;
	std::size_t lineIndex = 0;
};

// 解析窗口 XML；只接受单一根节点和属性形式的窗口文档。
bool ParseSimpleXmlDocument(
	const std::string& text,
	SimpleXmlNode& outRoot,
	SimpleXmlParseError* outError = nullptr);

}  // namespace e2txt
