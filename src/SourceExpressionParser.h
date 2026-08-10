#pragma once

#include <cstddef>
#include <memory>
#include <string>
#include <vector>

namespace e2txt {

// 易语言表达式节点类别。
enum class SourceExpressionKind {
	Missing,
	NumberLiteral,
	TextLiteral,
	LogicalLiteral,
	ByteSetLiteral,
	AddressOf,
	Name,
	Call,
	Member,
	Index,
	Unary,
	Binary,
	Group,
};

// 易语言表达式抽象语法树节点。
struct SourceExpressionNode {
	SourceExpressionKind kind = SourceExpressionKind::Missing;
	std::string text;
	std::vector<std::unique_ptr<SourceExpressionNode>> children;
};

// 单行表达式解析结果。
struct SourceExpressionParseResult {
	std::unique_ptr<SourceExpressionNode> root;
	std::size_t errorOffset = 0;
	std::string error;

	bool IsValid() const;
};

// 解析易语言表达式，支持调用、成员访问、数组下标、字面量和常见运算符。
SourceExpressionParseResult ParseSourceExpression(const std::string& text);

// 在不进入文本字面量、括号、数组和字节集常量时查找顶层赋值号。
bool FindSourceTopLevelAssignment(
	const std::string& text,
	std::size_t& outPosition,
	std::size_t& outLength);

// 将调用或流程指令的括号参数拆成表达式，保留末尾省略参数。
bool SplitSourceCallArguments(
	const std::string& text,
	std::vector<std::string>& outArguments);

}  // namespace e2txt
