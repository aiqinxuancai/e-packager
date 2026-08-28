#include "CompilerModel.h"

#include "../PathHelper.h"

#include <algorithm>
#include <charconv>
#include <cstdlib>
#include <functional>
#include <fstream>
#include <set>
#include <string_view>
#include <utility>

namespace ecompiler {
namespace {

constexpr std::uint32_t kLibraryEnumState = 1u << 22;
constexpr std::uint32_t kUserTypeMask = 0x40000000u;

std::size_t SystemTypeSize(std::uint32_t type);

std::string Trim(std::string value)
{
	while (!value.empty() && static_cast<unsigned char>(value.front()) <= 0x20) value.erase(value.begin());
	while (!value.empty() && static_cast<unsigned char>(value.back()) <= 0x20) value.pop_back();
	return value;
}

std::string StripUtf8Bom(std::string value)
{
	if (value.size() >= 3 && static_cast<unsigned char>(value[0]) == 0xEF &&
		static_cast<unsigned char>(value[1]) == 0xBB && static_cast<unsigned char>(value[2]) == 0xBF) {
		value.erase(0, 3);
	}
	return value;
}

bool StartsWith(const std::string& text, const std::string_view prefix)
{
	return text.size() >= prefix.size() && text.compare(0, prefix.size(), prefix) == 0;
}

std::string StripComment(const std::string& line)
{
	bool chineseText = false;
	bool asciiText = false;
	const std::string leftQuote = "“";
	const std::string rightQuote = "”";
	for (std::size_t i = 0; i < line.size();) {
		if (!asciiText && line.compare(i, leftQuote.size(), leftQuote) == 0) {
			chineseText = true;
			i += leftQuote.size();
			continue;
		}
		if (chineseText && line.compare(i, rightQuote.size(), rightQuote) == 0) {
			chineseText = false;
			i += rightQuote.size();
			continue;
		}
		if (!chineseText && line[i] == '"') {
			asciiText = !asciiText;
			++i;
			continue;
		}
		if (!chineseText && !asciiText && line[i] == '\'') return line.substr(0, i);
		++i;
	}
	return line;
}

std::vector<std::string> SplitLines(const std::string& text)
{
	std::vector<std::string> lines;
	std::size_t begin = 0;
	while (begin <= text.size()) {
		const std::size_t end = text.find_first_of("\r\n", begin);
		lines.push_back(text.substr(begin, end == std::string::npos ? std::string::npos : end - begin));
		if (end == std::string::npos) break;
		begin = end + 1;
		if (text[end] == '\r' && begin < text.size() && text[begin] == '\n') ++begin;
	}
	return lines;
}

std::vector<std::string> SplitFields(const std::string& text)
{
	std::vector<std::string> result;
	std::string field;
	int parentheses = 0;
	int brackets = 0;
	bool quoted = false;
	bool chineseQuoted = false;
	for (std::size_t index = 0; index < text.size();) {
		if (!quoted && text.compare(index, 3, "\xE2\x80\x9C") == 0) {
			chineseQuoted = true; field.append("\xE2\x80\x9C"); index += 3; continue;
		}
		if (chineseQuoted && text.compare(index, 3, "\xE2\x80\x9D") == 0) {
			chineseQuoted = false; field.append("\xE2\x80\x9D"); index += 3; continue;
		}
		const char ch = text[index++];
		if (!chineseQuoted && ch == '"') quoted = !quoted;
		if (!quoted && !chineseQuoted && ch == '(') ++parentheses;
		if (!quoted && !chineseQuoted && ch == ')') --parentheses;
		if (!quoted && !chineseQuoted && ch == '[') ++brackets;
		if (!quoted && !chineseQuoted && ch == ']') --brackets;
		if (!quoted && !chineseQuoted && parentheses == 0 && brackets == 0 && ch == ',') {
			result.push_back(Trim(std::move(field)));
			field.clear();
			continue;
		}
		field.push_back(ch);
	}
	result.push_back(Trim(std::move(field)));
	return result;
}

std::string DecodeConstantLiteral(std::string value)
{
	value = Trim(std::move(value));
	if (value.size() >= 2 && value.front() == '"' && value.back() == '"') {
		value = value.substr(1, value.size() - 2);
	}
    if (value.size() >= 6 && value.compare(0, 3, "\xE2\x80\x9C") == 0 &&
        value.compare(value.size() - 3, 3, "\xE2\x80\x9D") == 0) {
        value = value.substr(3, value.size() - 6);
    }
    const std::string escapedTextPrefix = "#e2txt_text#";
    const std::string escapedLongTextPrefix = "#e2txt_long_text#";
    if (value.compare(0, escapedTextPrefix.size(), escapedTextPrefix) == 0) value.erase(0, escapedTextPrefix.size());
    else if (value.compare(0, escapedLongTextPrefix.size(), escapedLongTextPrefix) == 0) value.erase(0, escapedLongTextPrefix.size());
	std::string decoded;
	decoded.reserve(value.size());
	for (std::size_t index = 0; index < value.size(); ++index) {
		if (value[index] != '\\' || index + 1 >= value.size()) {
			decoded.push_back(value[index]);
			continue;
		}
		const char escaped = value[++index];
		switch (escaped) {
		case 'r': decoded.push_back('\r'); break;
		case 'n': decoded.push_back('\n'); break;
		case 't': decoded.push_back('\t'); break;
		case '\\': decoded.push_back('\\'); break;
		case '"': decoded.push_back('"'); break;
		case 'x': {
			if (index + 2 < value.size()) {
				const auto begin = value.data() + index + 1;
				const auto end = begin + 2;
				unsigned int byte = 0;
				const auto parsed = std::from_chars(begin, end, byte, 16);
				if (parsed.ec == std::errc() && parsed.ptr == end) {
					decoded.push_back(static_cast<char>(byte));
					index += 2;
					break;
				}
			}
			decoded.push_back('x');
			break;
		}
		default: decoded.push_back(escaped); break;
		}
	}
	return decoded;
}

bool ParseNumberLiteral(const std::string& text, double& value)
{
	const std::string normalized = Trim(text);
	if (normalized.empty()) return false;
	char* end = nullptr;
	value = std::strtod(normalized.c_str(), &end);
	return end != normalized.c_str() && end != nullptr && *end == '\0';
}

bool IsExplicitTextLiteral(std::string value)
{
	value = Trim(std::move(value));
	if (value.size() >= 2 && value.front() == '"' && value.back() == '"') value = value.substr(1, value.size() - 2);
	if (value.size() >= 6 && value.compare(0, 3, "\xE2\x80\x9C") == 0 &&
		value.compare(value.size() - 3, 3, "\xE2\x80\x9D") == 0) return true;
	return value.compare(0, std::string("#e2txt_text#").size(), "#e2txt_text#") == 0 ||
		value.compare(0, std::string("#e2txt_long_text#").size(), "#e2txt_long_text#") == 0;
}

bool ExtractArguments(const std::string& line, std::vector<std::string>& outArguments)
{
	outArguments.clear();
	const std::size_t left = line.find('(');
	const std::size_t right = line.rfind(')');
	if (left == std::string::npos || right == std::string::npos || right < left) return false;
	return e2txt::SplitSourceCallArguments(line.substr(left + 1, right - left - 1), outArguments);
}

bool ParseExpression(
	const std::string& text,
	const std::string& sourceFile,
	const std::size_t sourceLine,
	std::unique_ptr<e2txt::SourceExpressionNode>& outExpression,
	std::string& error)
{
	auto parsed = e2txt::ParseSourceExpression(Trim(text));
	if (!parsed.IsValid()) {
		error = sourceFile + ":" + std::to_string(sourceLine) + ": expression_parse_failed:" + parsed.error;
		return false;
	}
	outExpression = std::move(parsed.root);
	return true;
}

bool ParseArgumentExpressions(
	const std::string& line,
	const std::string& sourceFile,
	const std::size_t sourceLine,
	std::vector<std::unique_ptr<e2txt::SourceExpressionNode>>& outExpressions,
	std::string& error)
{
	std::vector<std::string> arguments;
	if (!ExtractArguments(line, arguments)) {
		error = sourceFile + ":" + std::to_string(sourceLine) + ": invalid_statement_arguments";
		return false;
	}
	outExpressions.clear();
	for (const std::string& argument : arguments) {
		if (Trim(argument).empty()) {
			auto missing = std::make_unique<e2txt::SourceExpressionNode>();
			missing->kind = e2txt::SourceExpressionKind::Missing;
			outExpressions.push_back(std::move(missing));
			continue;
		}
		std::unique_ptr<e2txt::SourceExpressionNode> expression;
		if (!ParseExpression(argument, sourceFile, sourceLine, expression, error)) return false;
		outExpressions.push_back(std::move(expression));
	}
	return true;
}

std::string MarkerName(const std::string& line)
{
	if (line.empty()) return {};
	if (line.front() == '.') {
		const std::size_t end = line.find_first_of(" \t(");
		return line.substr(0, end);
	}
	// e5.95 sources may omit the leading dot on flow-control directives.
	// Normalize only the reserved directive vocabulary; ordinary calls remain
	// expressions and are never mistaken for control flow.
	static constexpr std::string_view kBareMarkers[] = {
		"如果真", "如果真结束", "如果", "否则", "如果结束",
		"判断开始", "判断", "默认", "判断结束",
		"判断循环首", "判断循环尾", "循环判断首", "循环判断尾",
		"计次循环首", "计次循环尾", "变量循环首", "变量循环尾",
	};
	for (const std::string_view marker : kBareMarkers) {
		if (line.size() < marker.size() || line.compare(0, marker.size(), marker) != 0) continue;
		if (line.size() == marker.size() || line[marker.size()] == ' ' || line[marker.size()] == '\t' || line[marker.size()] == '(')
			return "." + std::string(marker);
	}
	return {};
}

bool ParseStatements(
	const std::vector<std::string>& lines,
	std::size_t& index,
	const std::size_t end,
	const std::set<std::string>& stops,
	const std::string& sourceFile,
	std::vector<Statement>& outStatements,
	std::string& error);

bool TryParseMachineCode(
	const std::string& line,
	std::vector<std::uint8_t>& outBytes,
	std::string& error,
	const std::string& sourceFile,
	std::size_t sourceLine);

bool ParseCondition(
	const std::string& line,
	const std::string& sourceFile,
	const std::size_t sourceLine,
	std::unique_ptr<e2txt::SourceExpressionNode>& outCondition,
	std::string& error)
{
	std::vector<std::unique_ptr<e2txt::SourceExpressionNode>> arguments;
	if (!ParseArgumentExpressions(line, sourceFile, sourceLine, arguments, error) || arguments.size() != 1) {
		if (error.empty()) error = sourceFile + ":" + std::to_string(sourceLine) + ": condition_requires_one_argument";
		return false;
	}
	outCondition = std::move(arguments.front());
	return true;
}

bool ParseLoop(
	const StatementKind kind,
	const std::string& endMarker,
	const std::vector<std::string>& lines,
	std::size_t& index,
	const std::size_t end,
	const std::string& sourceFile,
	std::vector<Statement>& outStatements,
	std::string& error)
{
	Statement statement;
	statement.kind = kind;
	statement.sourceLine = index + 1;
	if (!ParseArgumentExpressions(Trim(StripComment(lines[index])), sourceFile, index + 1, statement.arguments, error)) return false;
	++index;
	if (!ParseStatements(lines, index, end, { endMarker }, sourceFile, statement.body, error)) return false;
	if (index >= end || MarkerName(Trim(StripComment(lines[index]))) != endMarker) {
		error = sourceFile + ":" + std::to_string(statement.sourceLine) + ": loop_end_not_found:" + endMarker;
		return false;
	}
	if (kind == StatementKind::DoWhile) {
		std::vector<std::unique_ptr<e2txt::SourceExpressionNode>> tailArguments;
		if (!ParseArgumentExpressions(Trim(StripComment(lines[index])), sourceFile, index + 1, tailArguments, error) || tailArguments.size() != 1) {
			if (error.empty()) error = sourceFile + ":" + std::to_string(index + 1) + ": loop_condition_required";
			return false;
		}
		statement.expression = std::move(tailArguments.front());
	}
	++index;
	outStatements.push_back(std::move(statement));
	return true;
}

bool ParseStatements(
	const std::vector<std::string>& lines,
	std::size_t& index,
	const std::size_t end,
	const std::set<std::string>& stops,
	const std::string& sourceFile,
	std::vector<Statement>& outStatements,
	std::string& error)
{
	while (index < end) {
		const std::size_t sourceLine = index + 1;
		const std::string line = Trim(StripComment(lines[index]));
		if (line.empty()) {
			++index;
			continue;
		}
		const std::string marker = MarkerName(line);
		if (stops.contains(marker)) return true;
		if (line == ".子程序" || line == ".版本" || line == ".支持库") {
			++index;
			continue;
		}
		if (StartsWith(line, ".局部变量 ") || StartsWith(line, ".参数 ")) {
			++index;
			continue;
		}
		if (marker == ".如果真") {
			Statement statement;
			statement.kind = StatementKind::IfTrue;
			statement.sourceLine = sourceLine;
			if (!ParseCondition(line, sourceFile, sourceLine, statement.expression, error)) return false;
			++index;
			if (!ParseStatements(lines, index, end, { ".如果真结束" }, sourceFile, statement.body, error)) return false;
			if (index >= end || MarkerName(Trim(StripComment(lines[index]))) != ".如果真结束") {
				error = sourceFile + ":" + std::to_string(sourceLine) + ": if_true_end_not_found";
				return false;
			}
			++index;
			outStatements.push_back(std::move(statement));
			continue;
		}
		if (marker == ".如果") {
			Statement statement;
			statement.kind = StatementKind::IfElse;
			statement.sourceLine = sourceLine;
			if (!ParseCondition(line, sourceFile, sourceLine, statement.expression, error)) return false;
			++index;
			if (!ParseStatements(lines, index, end, { ".否则", ".如果结束" }, sourceFile, statement.body, error)) return false;
			if (index < end && MarkerName(Trim(StripComment(lines[index]))) == ".否则") {
				++index;
				if (!ParseStatements(lines, index, end, { ".如果结束" }, sourceFile, statement.elseBody, error)) return false;
			}
			if (index >= end || MarkerName(Trim(StripComment(lines[index]))) != ".如果结束") {
				error = sourceFile + ":" + std::to_string(sourceLine) + ": if_end_not_found";
				return false;
			}
			++index;
			outStatements.push_back(std::move(statement));
			continue;
		}
		if (marker == ".判断开始") {
			Statement statement;
			statement.kind = StatementKind::Switch;
			statement.sourceLine = sourceLine;
			StatementBranch first;
			if (!ParseCondition(line, sourceFile, sourceLine, first.condition, error)) return false;
			++index;
			if (!ParseStatements(lines, index, end, { ".判断", ".默认", ".判断结束" }, sourceFile, first.body, error)) return false;
			statement.branches.push_back(std::move(first));
			while (index < end) {
				const std::string branchLine = Trim(StripComment(lines[index]));
				const std::string branchMarker = MarkerName(branchLine);
				if (branchMarker == ".判断") {
					StatementBranch branch;
					if (!ParseCondition(branchLine, sourceFile, index + 1, branch.condition, error)) return false;
					++index;
					if (!ParseStatements(lines, index, end, { ".判断", ".默认", ".判断结束" }, sourceFile, branch.body, error)) return false;
					statement.branches.push_back(std::move(branch));
					continue;
				}
				if (branchMarker == ".默认") {
					++index;
					if (!ParseStatements(lines, index, end, { ".判断结束" }, sourceFile, statement.elseBody, error)) return false;
				}
				break;
			}
			if (index >= end || MarkerName(Trim(StripComment(lines[index]))) != ".判断结束") {
				error = sourceFile + ":" + std::to_string(sourceLine) + ": switch_end_not_found";
				return false;
			}
			++index;
			outStatements.push_back(std::move(statement));
			continue;
		}
		if (marker == ".判断循环首") {
			if (!ParseLoop(StatementKind::While, ".判断循环尾", lines, index, end, sourceFile, outStatements, error)) return false;
			if (outStatements.back().arguments.size() != 1) {
				error = sourceFile + ":" + std::to_string(sourceLine) + ": while_requires_one_condition";
				return false;
			}
			outStatements.back().expression = std::move(outStatements.back().arguments.front());
			outStatements.back().arguments.clear();
			continue;
		}
		if (marker == ".循环判断首") {
			if (!ParseLoop(StatementKind::DoWhile, ".循环判断尾", lines, index, end, sourceFile, outStatements, error)) return false;
			continue;
		}
		if (marker == ".计次循环首") {
			if (!ParseLoop(StatementKind::CountLoop, ".计次循环尾", lines, index, end, sourceFile, outStatements, error)) return false;
			continue;
		}
		if (marker == ".变量循环首") {
			if (!ParseLoop(StatementKind::ForLoop, ".变量循环尾", lines, index, end, sourceFile, outStatements, error)) return false;
			continue;
		}
		if (StartsWith(line, "返回")) {
			Statement statement;
			statement.kind = StatementKind::Return;
			statement.sourceLine = sourceLine;
			std::vector<std::unique_ptr<e2txt::SourceExpressionNode>> arguments;
			if (line.find('(') != std::string::npos) {
				if (!ParseArgumentExpressions(line, sourceFile, sourceLine, arguments, error) || arguments.size() > 1) return false;
				if (!arguments.empty()) statement.expression = std::move(arguments.front());
			}
			else {
				const std::string expressionText = Trim(line.substr(std::string("返回").size()));
				if (!expressionText.empty() && !ParseExpression(expressionText, sourceFile, sourceLine, statement.expression, error)) return false;
			}
			++index;
			outStatements.push_back(std::move(statement));
			continue;
		}
		if (StartsWith(line, "跳出循环")) {
			outStatements.push_back(Statement { StatementKind::Break, sourceLine });
			++index;
			continue;
		}
		if (StartsWith(line, "到循环尾")) {
			outStatements.push_back(Statement { StatementKind::Continue, sourceLine });
			++index;
			continue;
		}
		// 编译期机器码指令保留为专用语句，不能按普通 FNE 运行时调用处理。
		if ((StartsWith(line, "置入代码") || StartsWith(line, ".置入代码")) && line.find('(') != std::string::npos) {
			Statement statement;
			statement.kind = StatementKind::MachineCode;
			statement.sourceLine = sourceLine;
			if (!TryParseMachineCode(line, statement.machineCode, error, sourceFile, sourceLine)) return false;
			++index;
			outStatements.push_back(std::move(statement));
			continue;
		}
		std::size_t assignment = 0;
		std::size_t assignmentLength = 0;
		if (e2txt::FindSourceTopLevelAssignment(line, assignment, assignmentLength)) {
			Statement statement;
			statement.kind = StatementKind::Assignment;
			statement.sourceLine = sourceLine;
			if (!ParseExpression(line.substr(0, assignment), sourceFile, sourceLine, statement.target, error) ||
				!ParseExpression(line.substr(assignment + assignmentLength), sourceFile, sourceLine, statement.expression, error)) return false;
			++index;
			outStatements.push_back(std::move(statement));
			continue;
		}
		if (!marker.empty()) {
			error = sourceFile + ":" + std::to_string(sourceLine) + ": unsupported_directive:" + marker;
			return false;
		}
		Statement statement;
		statement.kind = StatementKind::Expression;
		statement.sourceLine = sourceLine;
		if (!ParseExpression(line, sourceFile, sourceLine, statement.expression, error)) return false;
		++index;
		outStatements.push_back(std::move(statement));
	}
	return true;
}

TypeRef ResolveTypeName(const Program& program, const std::string& typeName, const bool isArray)
{
	std::string normalized = Trim(typeName);
	bool array = isArray;
	if (normalized.size() >= 2 && normalized.compare(normalized.size() - 2, 2, "[]") == 0) {
		normalized.erase(normalized.size() - 2);
		array = true;
	}
	const auto found = program.typeByName.find(normalized.empty() ? std::string("整数型") : normalized);
	if (found == program.typeByName.end()) return {};
	TypeRef result = found->second;
	result.isArray = array;
	return result;
}

Variable ParseVariableDeclaration(const std::string& line, const std::size_t sourceLine)
{
	const std::size_t space = line.find(' ');
	const std::vector<std::string> fields = SplitFields(space == std::string::npos ? std::string() : line.substr(space + 1));
	Variable variable;
	if (!fields.empty()) variable.name = fields[0];
	if (fields.size() >= 2) variable.typeName = fields[1];
	const std::string attributes = fields.size() >= 3 ? Trim(fields[2]) : std::string();
	variable.byReference = attributes.find("参考") != std::string::npos || attributes.find("传址") != std::string::npos;
	variable.nullable = attributes.find("可空") != std::string::npos || attributes.find("默认空") != std::string::npos;
	variable.type.isArray = attributes.find("数组") != std::string::npos ||
		(fields.size() >= 4 && !fields[3].empty() && fields[3] != "\"\"");
	if (fields.size() >= 4 && !fields[3].empty() && fields[3] != "\"\"") {
		std::string bounds = fields[3];
		if (bounds.size() >= 2 && bounds.front() == '"' && bounds.back() == '"') bounds = bounds.substr(1, bounds.size() - 2);
		for (const std::string& bound : SplitFields(bounds)) {
			const std::string normalizedBound = Trim(bound);
			char* end = nullptr;
			const long value = std::strtol(normalizedBound.c_str(), &end, 10);
			if (end != nullptr && end != normalizedBound.c_str() && *end == '\0' && value >= 0 && value <= 0x1000000)
				variable.arrayDimensions.push_back(static_cast<int>(value));
		}
	}
	variable.sourceLine = sourceLine;
	return variable;
}

std::string UnquoteDeclarationText(std::string value)
{
	value = Trim(std::move(value));
	if (value.size() >= 2 && value.front() == '"' && value.back() == '"') {
		value = value.substr(1, value.size() - 2);
	}
	return value;
}

std::string ExtractDirectiveValue(const std::string& text, const std::string& directive)
{
	const std::string needle = "($" + directive + "=";
	const std::size_t begin = text.find(needle);
	if (begin == std::string::npos) return {};
	const std::size_t valueBegin = begin + needle.size();
	const std::size_t valueEnd = text.find(')', valueBegin);
	const std::string value = text.substr(valueBegin, valueEnd == std::string::npos ? std::string::npos : valueEnd - valueBegin);
	return Trim(value);
}

bool IsConditionalCommentEnabled(const std::string& comment, const std::unordered_set<std::string>& activeMacros)
{
	std::size_t position = 0;
	while (position < comment.size() && static_cast<unsigned char>(comment[position]) <= 0x20) ++position;
	while (position + 2 < comment.size() && comment[position] == '$' && comment[position + 1] == '(') {
		const std::size_t close = comment.find(')', position + 2);
		if (close == std::string::npos) return false;
		bool expressionEnabled = false;
		std::size_t begin = position + 2;
		while (begin <= close) {
			const std::size_t separator = comment.find(',', begin);
			const std::size_t end = separator == std::string::npos || separator > close ? close : separator;
			std::string macro = Trim(comment.substr(begin, end - begin));
			bool negated = !macro.empty() && macro.front() == '!';
			if (negated) macro.erase(macro.begin());
			std::transform(macro.begin(), macro.end(), macro.begin(), [](const unsigned char value) {
				return static_cast<char>(std::toupper(value));
			});
			if (!macro.empty()) expressionEnabled = expressionEnabled || (negated ? !activeMacros.contains(macro) : activeMacros.contains(macro));
			if (separator == std::string::npos || separator >= close) break;
			begin = separator + 1;
		}
		if (!expressionEnabled) return false;
		position = close + 1;
		while (position < comment.size() && static_cast<unsigned char>(comment[position]) <= 0x20) ++position;
	}
	return true;
}

bool TryParseMachineCode(const std::string& line, std::vector<std::uint8_t>& outBytes, std::string& error, const std::string& sourceFile, const std::size_t sourceLine)
{
	outBytes.clear();
	std::unique_ptr<e2txt::SourceExpressionNode> expression;
	if (!ParseExpression(line, sourceFile, sourceLine, expression, error)) return false;
	if (expression == nullptr || expression->kind != e2txt::SourceExpressionKind::Call || expression->children.size() != 2 ||
		expression->children.front()->kind != e2txt::SourceExpressionKind::Name || expression->children.front()->text != "置入代码") {
		error = sourceFile + ":" + std::to_string(sourceLine) + ": machine_code_call_invalid";
		return false;
	}
	const auto& data = *expression->children[1];
	if (data.kind != e2txt::SourceExpressionKind::ByteSetLiteral) {
		error = sourceFile + ":" + std::to_string(sourceLine) + ": machine_code_requires_byte_set_literal";
		return false;
	}
	for (const auto& item : data.children) {
		if (item == nullptr) { error = sourceFile + ":" + std::to_string(sourceLine) + ": machine_code_byte_missing"; return false; }
		char* end = nullptr;
		long value = item->kind == e2txt::SourceExpressionKind::NumberLiteral ? std::strtol(item->text.c_str(), &end, 0) : -1;
		if (end == item->text.c_str() || end == nullptr || *end != '\0' || value < 0 || value > 255) {
			error = sourceFile + ":" + std::to_string(sourceLine) + ": machine_code_byte_out_of_range";
			return false;
		}
		outBytes.push_back(static_cast<std::uint8_t>(value));
	}
	return true;
}

bool RegisterDllCommands(Program& program, std::string& error)
{
	const auto lines = SplitLines(program.bundle.dllDeclareText);
	std::size_t current = static_cast<std::size_t>(-1);
	for (std::size_t index = 0; index < lines.size(); ++index) {
		const std::string line = Trim(StripUtf8Bom(StripComment(lines[index])));
		if (line.empty()) continue;
		if (StartsWith(line, ".DLL命令 ")) {
			const auto fields = SplitFields(line.substr(std::string(".DLL命令 ").size()));
			if (fields.empty() || Trim(fields[0]).empty()) {
				error = ".DLL声明.txt:" + std::to_string(index + 1) + ": dll_command_name_missing";
				return false;
			}
			if (program.dllCommandByName.contains(Trim(fields[0]))) {
				error = ".DLL声明.txt:" + std::to_string(index + 1) + ": duplicate_dll_command:" + Trim(fields[0]);
				return false;
			}
			DllCommand command;
			command.name = Trim(fields[0]);
			const std::string returnTypeName = fields.size() >= 2 ? Trim(fields[1]) : std::string();
			command.returnType = returnTypeName.empty() ? TypeRef { kTypeNull, false, true } : ResolveTypeName(program, returnTypeName, false);
			if (!command.returnType.valid) {
				error = ".DLL声明.txt:" + std::to_string(index + 1) + ": unknown_dll_return_type:" + returnTypeName;
				return false;
			}
			command.fileName = fields.size() >= 3 ? UnquoteDeclarationText(fields[2]) : std::string();
			command.entryName = fields.size() >= 4 ? UnquoteDeclarationText(fields[3]) : command.name;
			const std::string comment = fields.size() >= 6 ? Trim(fields[5]) : std::string();
			command.usesCdecl = comment.find("($cdecl)") != std::string::npos || comment.find("($cdecl") != std::string::npos;
			if (comment.find("($stdcall)") != std::string::npos) command.usesCdecl = false;
			command.sourceLine = index + 1;
			current = program.dllCommands.size();
			program.dllCommandByName.emplace(command.name, current);
			program.dllCommands.push_back(std::move(command));
			continue;
		}
		if (StartsWith(line, ".参数 ")) {
			if (current == static_cast<std::size_t>(-1)) {
				error = ".DLL声明.txt:" + std::to_string(index + 1) + ": dll_parameter_without_command";
				return false;
			}
			Variable parameter = ParseVariableDeclaration(line, index + 1);
			if (parameter.name.empty()) {
				error = ".DLL声明.txt:" + std::to_string(index + 1) + ": dll_parameter_name_missing";
				return false;
			}
			program.dllCommands[current].parameters.push_back(std::move(parameter));
			continue;
		}
		if (line.front() == '.') current = static_cast<std::size_t>(-1);
	}
	return true;
}

bool RegisterProjectConstants(Program& program, std::string& error)
{
	const auto lines = SplitLines(program.bundle.constantText);
	for (std::size_t index = 0; index < lines.size(); ++index) {
		const std::string line = Trim(StripUtf8Bom(StripComment(lines[index])));
		if (!StartsWith(line, ".常量 ")) continue;
		const auto fields = SplitFields(line.substr(std::string(".常量 ").size()));
		if (fields.empty() || Trim(fields[0]).empty()) continue;
		Constant constant;
		constant.name = "#" + Trim(fields[0]);
		const std::string literal = fields.size() >= 2 ? Trim(fields[1]) : std::string();
		const std::string decoded = DecodeConstantLiteral(literal);
		if (decoded == "真" || decoded == "假") {
			constant.type = kTypeBool;
			constant.numberValue = decoded == "真" ? 1.0 : 0.0;
		}
		else {
			double number = 0;
			if (!IsExplicitTextLiteral(literal) && ParseNumberLiteral(decoded, number)) {
				constant.type = kTypeDouble;
				constant.numberValue = number;
			}
			else {
				constant.type = kTypeText;
				constant.textValue = decoded;
			}
		}
		if (!program.constants.emplace(constant.name, std::move(constant)).second) {
			error = ".常量.txt:" + std::to_string(index + 1) + ": duplicate_constant:" + fields[0];
			return false;
		}
	}
	return true;
}

bool RegisterProjectGlobals(Program& program, std::string& error)
{
	const auto lines = SplitLines(program.bundle.globalText);
	for (std::size_t index = 0; index < lines.size(); ++index) {
		const std::string line = Trim(StripUtf8Bom(StripComment(lines[index])));
		if (!StartsWith(line, ".全局变量 ")) continue;
		Variable variable = ParseVariableDeclaration(line, index + 1);
		if (variable.name.empty()) continue;
		if (std::any_of(program.globals.begin(), program.globals.end(), [&](const Variable& item) {
			return item.name == variable.name;
		})) {
			error = ".全局变量.txt:" + std::to_string(index + 1) + ": duplicate_global_variable:" + variable.name;
			return false;
		}
		program.globals.push_back(std::move(variable));
	}
	return true;
}

bool RegisterProjectTypes(Program& program, std::string& error)
{
	struct PendingMember {
		std::string name;
		std::string typeName;
		bool isArray = false;
		std::int32_t defaultValue = 0;
		std::size_t sourceLine = 0;
	};
	std::unordered_map<std::size_t, std::vector<PendingMember>> pendingMembers;
	const auto lines = SplitLines(program.bundle.dataTypeText);
	std::size_t current = static_cast<std::size_t>(-1);
	// Register all names first.  A member may refer to a type declared later
	// in the page, so resolving while scanning would make valid forward
	// references depend on declaration order.
	for (std::size_t index = 0; index < lines.size(); ++index) {
		const std::string line = Trim(StripUtf8Bom(StripComment(lines[index])));
		if (StartsWith(line, ".数据类型 ")) {
			std::string declaration = Trim(line.substr(std::string(".数据类型 ").size()));
			std::string name = declaration;
			const std::size_t comma = name.find(',');
			if (comma != std::string::npos) name = Trim(name.substr(0, comma));
			if (name.empty()) {
				error = ".数据类型.txt:" + std::to_string(index + 1) + ": data_type_name_missing";
				return false;
			}
			if (program.typeByName.contains(name)) {
				error = ".数据类型.txt:" + std::to_string(index + 1) + ": duplicate_data_type:" + name;
				return false;
			}
			TypeInfo type;
			type.type = { kUserTypeMask | static_cast<std::uint32_t>(program.types.size() + 1), false, true };
			type.name = name;
			type.libraryIndex = static_cast<std::size_t>(-1);
			type.isEnum = declaration.find("枚举") != std::string::npos;
			const std::size_t typeIndex = program.types.size();
			program.typeByCode.emplace(type.type.code, typeIndex);
			program.typeByName.emplace(name, type.type);
			program.types.push_back(std::move(type));
			current = typeIndex;
			continue;
		}
		if (current == static_cast<std::size_t>(-1) || !StartsWith(line, ".成员 ")) continue;
		const auto fields = SplitFields(line.substr(std::string(".成员 ").size()));
		if (fields.size() < 2 || Trim(fields[0]).empty()) continue;
		std::int32_t defaultValue = 0;
		if (fields.size() >= 4) {
			const std::string text = Trim(fields[3]);
			if (!text.empty() && text != "\"\"") {
				char* end = nullptr;
				const long parsed = std::strtol(text.c_str(), &end, 10);
				if (end != text.c_str() && end != nullptr && *end == '\0') defaultValue = static_cast<std::int32_t>(parsed);
			}
		}
		pendingMembers[current].push_back(PendingMember {
			Trim(fields[0]), Trim(fields[1]),
			!program.types[current].isEnum && fields.size() >= 4 && !Trim(fields[3]).empty() && fields[3] != "\"\"",
			defaultValue, index + 1,
		});
	}

	for (auto& [typeIndex, members] : pendingMembers) {
		for (const PendingMember& pending : members) {
			const TypeRef memberType = ResolveTypeName(program, pending.typeName, pending.isArray);
			if (!memberType.valid) {
				error = ".数据类型.txt:" + std::to_string(pending.sourceLine) + ": unknown_data_type_member_type:" + pending.typeName;
				return false;
			}
			program.types[typeIndex].elements.push_back(TypeElement {
				pending.name, memberType, 0, pending.defaultValue,
			});
			if (program.types[typeIndex].isEnum) {
				Constant constant;
				constant.name = "#" + program.types[typeIndex].name + "." + pending.name;
				constant.type = kTypeInt;
				constant.numberValue = pending.defaultValue;
				program.constants.try_emplace(constant.name, std::move(constant));
			}
		}
	}

	std::function<bool(std::size_t, std::set<std::size_t>&)> calculate;
	calculate = [&](const std::size_t typeIndex, std::set<std::size_t>& active) -> bool {
		TypeInfo& type = program.types[typeIndex];
		if ((type.type.code & kUserTypeMask) == 0 || type.size != 0) return true;
		if (type.isEnum) {
			type.elements.clear();
			type.size = 4;
			return true;
		}
		if (!active.insert(typeIndex).second) {
			error = "recursive_data_type:" + type.name;
			return false;
		}
		std::size_t offset = 0;
		for (TypeElement& element : type.elements) {
			element.offset = offset;
			const TypeRef memberType = element.type;
			std::size_t size = memberType.isArray ? 4 : SystemTypeSize(memberType.code);
			if (size == 0) {
				const auto nested = program.typeByCode.find(memberType.code);
				if (nested == program.typeByCode.end() || !calculate(nested->second, active)) return false;
				size = 4;
			}
			offset += size;
		}
		type.size = (std::max)(offset, std::size_t(1));
		active.erase(typeIndex);
		return true;
	};
	for (std::size_t typeIndex = 0; typeIndex < program.types.size(); ++typeIndex) {
		if ((program.types[typeIndex].type.code & kUserTypeMask) == 0) continue;
		std::set<std::size_t> active;
		if (!calculate(typeIndex, active)) return false;
	}
	return true;
}

std::filesystem::path ResolveSupportLibraryPath(
	const e2txt::Dependency& dependency,
	const std::filesystem::path& inputRoot,
	const std::vector<std::filesystem::path>& searchDirectories)
{
	std::vector<std::filesystem::path> candidates;
	if (!dependency.resolvedPath.empty()) candidates.push_back(Utf8PathToPath(dependency.resolvedPath));
	if (!dependency.path.empty()) {
		const std::filesystem::path configured = Utf8PathToPath(dependency.path);
		candidates.push_back(configured);
		if (configured.is_relative()) candidates.push_back(inputRoot / configured);
	}
	const std::string libraryName = !dependency.fileName.empty() ? dependency.fileName : dependency.name;
	if (!libraryName.empty()) {
		const std::filesystem::path configuredName = Utf8PathToPath(libraryName);
		std::vector<std::filesystem::path> variants;
		if (configuredName.has_extension()) {
			variants.push_back(configuredName);
		}
		else {
			variants.push_back(configuredName.wstring() + L".fne");
			variants.push_back(configuredName.wstring() + L".fnr");
			variants.push_back(configuredName);
		}
		for (const auto& variant : variants) {
			candidates.push_back(inputRoot / variant);
			for (const auto& directory : searchDirectories) candidates.push_back(directory / variant);
		}
	}
	std::error_code ec;
	for (const auto& candidate : candidates) {
		if (std::filesystem::is_regular_file(candidate, ec)) return std::filesystem::absolute(candidate, ec);
		ec.clear();
	}
	return {};
}

std::size_t SystemTypeSize(const std::uint32_t type)
{
	switch (type) {
	case kTypeByte: return 1;
	case kTypeShort: return 2;
	case kTypeInt:
	case kTypeFloat:
	case kTypeBool:
	case kTypeText:
	case kTypeBinary:
	case kTypeSubroutine: return 4;
	case kTypeInt64:
	case kTypeDouble:
	case kTypeDateTime: return 8;
	default: return 0;
	}
}

bool LoadLibraries(Program& program, std::string& error)
{
#if !defined(_M_IX86)
	(void)program;
	error = "independent compiler backend requires Win32 e-packager";
	return false;
#else
	// A source page can retain a .支持库 declaration even when an edited or
	// reconstructed .e no longer carries that dependency in its binary header.
	// Recover those declarations before loading metadata so source and bundle
	// inputs follow the same dependency path.
	std::unordered_set<std::string> knownNames;
	for (const auto& dependency : program.bundle.dependencies) {
		knownNames.insert(dependency.fileName.empty() ? dependency.name : dependency.fileName);
	}
	for (const auto& sourceFile : program.bundle.sourceFiles) {
		for (const std::string& rawLine : SplitLines(sourceFile.content)) {
			const std::string line = Trim(StripComment(rawLine));
			if (!StartsWith(line, ".支持库 ")) continue;
			const std::string name = Trim(line.substr(std::string(".支持库 ").size()));
			if (name.empty() || !knownNames.insert(name).second) continue;
			e2txt::Dependency dependency;
			dependency.kind = e2txt::DependencyKind::ELib;
			dependency.fileName = name;
			dependency.name = name;
			program.bundle.dependencies.push_back(std::move(dependency));
		}
	}
	// If a hand-edited/reconstructed project lost its dependency header, use
	// source-referenced type and command names to discover matching FNEs from
	// the configured library directories. This is metadata-driven and does not
	// name any particular support library.
	std::string sourceText;
	for (const auto& sourceFile : program.bundle.sourceFiles) sourceText += sourceFile.content + "\n";
	std::string sourceTextLower = sourceText;
	std::transform(sourceTextLower.begin(), sourceTextLower.end(), sourceTextLower.begin(), [](const unsigned char value) {
		return static_cast<char>(std::tolower(value));
	});
	auto containsIdentifier = [](const std::string& text, const std::string& token) {
		if (token.empty()) return false;
		for (std::size_t offset = text.find(token); offset != std::string::npos; offset = text.find(token, offset + 1)) {
			const auto isIdentifier = [](const char value) {
				return std::isalnum(static_cast<unsigned char>(value)) != 0 || value == '_';
			};
			const bool leftBoundary = offset == 0 || !isIdentifier(text[offset - 1]);
			const std::size_t end = offset + token.size();
			const bool rightBoundary = end >= text.size() || !isIdentifier(text[end]);
			if (leftBoundary && rightBoundary) return true;
		}
		return false;
	};
	std::unordered_set<std::string> inspectedPaths;
	for (const auto& searchDirectory : program.supportLibrarySearchDirectories) {
		std::error_code iteratorError;
		for (const auto& entry : std::filesystem::directory_iterator(searchDirectory, iteratorError)) {
			if (!entry.is_regular_file(iteratorError) || entry.path().extension() != L".fne") continue;
			const std::string pathKey = PathToUtf8(entry.path().lexically_normal());
			if (!inspectedPaths.insert(pathKey).second) continue;
			const std::string stem = entry.path().stem().string();
			std::string stemLower = stem;
			std::transform(stemLower.begin(), stemLower.end(), stemLower.begin(), [](const unsigned char value) {
				return static_cast<char>(std::tolower(value));
			});
			const std::string supportDirective = ".支持库 " + stemLower;
			bool referenced = !stemLower.empty() &&
				(sourceTextLower.find(supportDirective) != std::string::npos || containsIdentifier(sourceTextLower, stemLower));
			const std::filesystem::path sidecar = entry.path().parent_path() / (entry.path().stem().wstring() + L".txt");
			std::ifstream sidecarInput(sidecar, std::ios::binary);
			if (!referenced && sidecarInput) {
				std::ostringstream sidecarBuffer;
				sidecarBuffer << sidecarInput.rdbuf();
				for (const std::string& rawLine : SplitLines(sidecarBuffer.str())) {
					const std::string line = Trim(StripUtf8Bom(StripComment(rawLine)));
					const bool typeLine = StartsWith(line, ".数据类型 ");
					const bool commandLine = StartsWith(line, ".命令 ");
					if (!typeLine && !commandLine) continue;
					const std::size_t begin = line.find(' ') + 1;
					const std::size_t comma = line.find(',', begin);
					const std::string name = Trim(line.substr(begin, comma == std::string::npos ? std::string::npos : comma - begin));
					if (name.empty()) continue;
					const std::string needle = commandLine ? name + " (" : name;
					if (sourceText.find(needle) != std::string::npos || sourceText.find(name + "(") != std::string::npos) { referenced = true; break; }
				}
			}
			if (!referenced) continue;
			support_library_public_info::LibraryMetadata metadata;
			std::string metadataError;
			if (!support_library_public_info::LoadSupportLibraryMetadata(entry.path(), metadata, metadataError)) continue;
			const std::string dependencyName = entry.path().stem().string();
			if (!knownNames.insert(dependencyName).second) continue;
			e2txt::Dependency dependency;
			dependency.kind = e2txt::DependencyKind::ELib;
			dependency.fileName = dependencyName;
			dependency.name = metadata.name.empty() ? dependencyName : metadata.name;
			dependency.guid = metadata.guid;
			dependency.versionText = std::to_string(metadata.majorVersion) + "." + std::to_string(metadata.minorVersion);
			dependency.resolvedPath = pathKey;
			program.bundle.dependencies.push_back(std::move(dependency));
		}
	}
	for (std::size_t index = 0; index < program.bundle.dependencies.size(); ++index) {
		const auto& dependency = program.bundle.dependencies[index];
		if (dependency.kind != e2txt::DependencyKind::ELib) continue;
		const std::filesystem::path path = ResolveSupportLibraryPath(
			dependency, program.inputRoot, program.supportLibrarySearchDirectories);
		if (path.empty()) {
			error = "support_library_not_found:" + dependency.fileName + ":" + dependency.name;
			return false;
		}
		Library library;
		library.ordinal = program.libraries.size();
		library.dependency = dependency;
		if (!support_library_public_info::LoadSupportLibraryMetadata(path, library.metadata, error)) {
			error = "support_library_metadata_failed:" + PathToUtf8(path) + ":" + error;
			return false;
		}
		program.libraries.push_back(std::move(library));
	}
	if (program.libraries.empty()) {
		error = "support_library_dependency_missing";
		return false;
	}
	return true;
#endif
}

void RegisterSystemTypes(Program& program)
{
	const std::pair<const char*, std::uint32_t> types[] = {
		{ "通用型", kTypeAll }, { "字节型", kTypeByte }, { "短整数型", kTypeShort },
		{ "整数型", kTypeInt }, { "长整数型", kTypeInt64 }, { "小数型", kTypeFloat },
		{ "双精度小数型", kTypeDouble }, { "逻辑型", kTypeBool }, { "日期时间型", kTypeDateTime },
		{ "文本型", kTypeText }, { "字节集", kTypeBinary }, { "字节集型", kTypeBinary },
		{ "子程序指针", kTypeSubroutine },
	};
	for (const auto& [name, code] : types) program.typeByName.emplace(name, TypeRef { code, false, true });
}

bool RegisterLibraryTypes(Program& program, std::string& error)
{
	for (std::size_t libraryIndex = 0; libraryIndex < program.libraries.size(); ++libraryIndex) {
		const auto& metadata = program.libraries[libraryIndex].metadata;
		for (const auto& source : metadata.dataTypes) {
			TypeInfo type;
			type.type = { static_cast<std::uint32_t>(((libraryIndex + 1) << 16) | (source.index + 1)), false, true };
			type.name = source.name;
			type.libraryIndex = libraryIndex;
			type.dataTypeIndex = source.index;
			type.isEnum = (source.state & kLibraryEnumState) != 0;
			type.memberCommandIndexes = source.commandIndexes;
			const std::size_t typeIndex = program.types.size();
			program.typeByCode.emplace(type.type.code, typeIndex);
			if (!type.name.empty() && !program.typeByName.contains(type.name)) program.typeByName.emplace(type.name, type.type);
			program.types.push_back(std::move(type));
		}
	}
	std::function<bool(std::size_t, std::set<std::size_t>&)> calculate;
	calculate = [&](const std::size_t typeIndex, std::set<std::size_t>& active) -> bool {
		TypeInfo& type = program.types[typeIndex];
		if (type.size != 0) return true;
		if (type.isEnum) {
			type.size = 4;
			return true;
		}
		if (!active.insert(typeIndex).second) {
			error = "recursive_support_library_type:" + type.name;
			return false;
		}
		const auto& source = program.libraries[type.libraryIndex].metadata.dataTypes[type.dataTypeIndex];
		std::size_t offset = 0;
		for (const auto& sourceElement : source.elements) {
			TypeElement element;
			element.name = sourceElement.name;
			element.type = { program.NormalizeLibraryType(type.libraryIndex, sourceElement.dataType), sourceElement.isArray, true };
			element.offset = offset;
			element.defaultValue = sourceElement.defaultValue;
			std::size_t elementSize = sourceElement.isArray ? 4 : SystemTypeSize(element.type.code);
			if (elementSize == 0) {
				const auto nested = program.typeByCode.find(element.type.code);
				if (nested == program.typeByCode.end()) {
					error = "unknown_support_library_element_type:" + type.name + "." + element.name;
					return false;
				}
				if (!calculate(nested->second, active)) return false;
				// 复合类型成员在易语言运行时中保存对象数据指针。
				elementSize = 4;
			}
			offset += elementSize;
			type.elements.push_back(std::move(element));
		}
		type.size = (std::max)(offset, std::size_t(1));
		active.erase(typeIndex);
		return true;
	};
	for (std::size_t index = 0; index < program.types.size(); ++index) {
		std::set<std::size_t> active;
		if (!calculate(index, active)) return false;
	}
	return true;
}

bool ResolveVariables(Program& program, std::string& error)
{
	for (Variable& variable : program.globals) {
		variable.type = ResolveTypeName(program, variable.typeName, variable.type.isArray);
		if (!variable.type.valid) {
			error = ".全局变量.txt:" + std::to_string(variable.sourceLine) + ": unknown_global_type:" + variable.typeName;
			return false;
		}
	}
	for (Assembly& assembly : program.assemblies) {
		for (Variable& variable : assembly.variables) {
			variable.type = ResolveTypeName(program, variable.typeName, variable.type.isArray);
			if (!variable.type.valid) {
				error = assembly.sourceFile + ":" + std::to_string(variable.sourceLine) + ": unknown_type:" + variable.typeName;
				return false;
			}
		}
	}
	for (Method& method : program.methods) {
		for (Variable& variable : method.parameters) {
			variable.type = ResolveTypeName(program, variable.typeName, variable.type.isArray);
			if (!variable.type.valid) {
				error = method.sourceFile + ":" + std::to_string(variable.sourceLine) + ": unknown_parameter_type:" + variable.typeName;
				return false;
			}
		}
		for (Variable& variable : method.locals) {
			variable.type = ResolveTypeName(program, variable.typeName, variable.type.isArray);
			if (!variable.type.valid) {
				error = method.sourceFile + ":" + std::to_string(variable.sourceLine) + ": unknown_local_type:" + variable.typeName;
				return false;
			}
		}
	}
	for (DllCommand& command : program.dllCommands) {
		for (Variable& parameter : command.parameters) {
			parameter.type = ResolveTypeName(program, parameter.typeName, parameter.type.isArray);
			if (!parameter.type.valid) {
				error = ".DLL声明.txt:" + std::to_string(parameter.sourceLine) + ": unknown_dll_parameter_type:" + parameter.typeName;
				return false;
			}
		}
	}
	return true;
}

bool ParseSources(Program& program, std::string& error)
{
	for (const auto& sourceFile : program.bundle.sourceFiles) {
		const std::string fileName = sourceFile.relativePath.empty() ? sourceFile.logicalName : sourceFile.relativePath;
		const std::vector<std::string> lines = SplitLines(sourceFile.content);
		std::size_t assemblyIndex = static_cast<std::size_t>(-1);
		for (std::size_t index = 0; index < lines.size();) {
			const std::string line = Trim(StripComment(lines[index]));
			if (StartsWith(line, ".程序集 ")) {
				Assembly assembly;
				const std::string declaration = Trim(line.substr(std::string(".程序集 ").size()));
				assembly.isClass = declaration.find(',') != std::string::npos;
				assembly.name = declaration;
				const std::size_t comma = assembly.name.find(',');
				if (comma != std::string::npos) assembly.name = Trim(assembly.name.substr(0, comma));
				assembly.sourceFile = fileName;
				assemblyIndex = program.assemblies.size();
				program.assemblies.push_back(std::move(assembly));
				++index;
				continue;
			}
			if (StartsWith(line, ".程序集变量 ")) {
				if (assemblyIndex == static_cast<std::size_t>(-1)) {
					error = fileName + ":" + std::to_string(index + 1) + ": assembly_variable_without_assembly";
					return false;
				}
				program.assemblies[assemblyIndex].variables.push_back(ParseVariableDeclaration(line, index + 1));
				++index;
				continue;
			}
			if (!StartsWith(line, ".子程序 ")) {
				++index;
				continue;
			}
			if (assemblyIndex == static_cast<std::size_t>(-1)) {
				error = fileName + ":" + std::to_string(index + 1) + ": method_without_assembly";
				return false;
			}
			const std::vector<std::string> fields = SplitFields(line.substr(std::string(".子程序 ").size()));
			if (fields.empty() || fields[0].empty()) {
				error = fileName + ":" + std::to_string(index + 1) + ": method_name_missing";
				return false;
			}
			Method method;
			method.id = program.methods.size();
			method.assemblyIndex = assemblyIndex;
			method.name = fields[0];
			method.sourceFile = fileName;
			method.sourceLine = index + 1;
			const std::string returnTypeName = fields.size() >= 2 ? fields[1] : std::string();
		method.returnType = returnTypeName.empty() ? TypeRef { kTypeNull, false, true } : ResolveTypeName(program, returnTypeName, false);
		method.isPublic = fields.size() >= 3 && Trim(fields[2]) == "公开";
		const std::string methodComment = fields.size() >= 4 ? Trim(fields[3]) : std::string();
		const std::size_t bodyBegin = index + 1;
		std::size_t bodyEnd = bodyBegin;
		while (bodyEnd < lines.size() && !StartsWith(Trim(StripComment(lines[bodyEnd])), ".子程序 ")) ++bodyEnd;
		if (!IsConditionalCommentEnabled(methodComment, program.conditionMacros)) {
			index = bodyEnd;
				continue;
			}
			method.exportName = method.name;
			if (const std::string customName = ExtractDirectiveValue(methodComment, "name"); !customName.empty()) method.exportName = customName;
			method.usesCdecl = methodComment.find("($cdecl)") != std::string::npos || methodComment.find("($cdecl") != std::string::npos;
			if (methodComment.find("($stdcall)") != std::string::npos) method.usesCdecl = false;
			if (!method.returnType.valid) {
				error = fileName + ":" + std::to_string(index + 1) + ": unknown_return_type:" + returnTypeName;
				return false;
			}
			for (std::size_t declarationIndex = bodyBegin; declarationIndex < bodyEnd; ++declarationIndex) {
				const std::string declaration = Trim(StripComment(lines[declarationIndex]));
				if (StartsWith(declaration, ".参数 ")) method.parameters.push_back(ParseVariableDeclaration(declaration, declarationIndex + 1));
				else if (StartsWith(declaration, ".局部变量 ")) method.locals.push_back(ParseVariableDeclaration(declaration, declarationIndex + 1));
			}
			std::size_t parseIndex = bodyBegin;
			if (!ParseStatements(lines, parseIndex, bodyEnd, {}, fileName, method.body, error)) return false;
			if (!program.methodByName.emplace(method.name, method.id).second) {
				error = "duplicate_method_name:" + method.name;
				return false;
			}
			program.assemblies[assemblyIndex].methodIds.push_back(method.id);
			program.methods.push_back(std::move(method));
			index = bodyEnd;
		}
	}
	if (!program.methodByName.contains("_启动子程序")) {
		error = "startup_method_not_found:_启动子程序";
		return false;
	}
	return true;
}

bool RegisterClassTypes(Program& program, std::string& error)
{
	for (std::size_t assemblyIndex = 0; assemblyIndex < program.assemblies.size(); ++assemblyIndex) {
		Assembly& assembly = program.assemblies[assemblyIndex];
		if (!assembly.isClass || assembly.name.empty()) continue;
		if (program.typeByName.contains(assembly.name)) {
			error = "duplicate_class_type:" + assembly.name;
			return false;
		}
		TypeInfo type;
		type.type = { kUserTypeMask | static_cast<std::uint32_t>(program.types.size() + 1), false, true };
		type.name = assembly.name;
		for (const Variable& variable : assembly.variables) {
			if (!variable.type.valid) continue;
			type.elements.push_back(TypeElement { variable.name, variable.type, 0, 0 });
		}
		const std::size_t typeIndex = program.types.size();
		program.typeByCode.emplace(type.type.code, typeIndex);
		program.typeByName.emplace(type.name, type.type);
		program.types.push_back(std::move(type));
		for (const std::size_t methodId : assembly.methodIds) {
			if (methodId >= program.methods.size()) continue;
			program.methods[methodId].ownerType = program.types[typeIndex].type;
			program.types[typeIndex].memberMethodIds.push_back(methodId);
		}
	}
	return true;
}

void PopulateClassTypeFields(Program& program)
{
	for (std::size_t typeIndex = 0; typeIndex < program.types.size(); ++typeIndex) {
		TypeInfo& type = program.types[typeIndex];
		if (type.name.empty()) continue;
		const auto assembly = std::find_if(program.assemblies.begin(), program.assemblies.end(), [&](const Assembly& value) {
			return value.isClass && value.name == type.name;
		});
		if (assembly == program.assemblies.end()) continue;
		type.elements.clear();
		for (const Variable& variable : assembly->variables) {
			if (variable.type.valid) type.elements.push_back(TypeElement { variable.name, variable.type, 0, 0 });
		}
	}
}

void RegisterCommands(Program& program)
{
	for (std::size_t libraryIndex = 0; libraryIndex < program.libraries.size(); ++libraryIndex) {
		const auto& commands = program.libraries[libraryIndex].metadata.commands;
		for (std::size_t commandIndex = 0; commandIndex < commands.size(); ++commandIndex) {
			const auto& command = commands[commandIndex];
			if (command.category == -1 || command.name.empty()) continue;
			program.globalCommands[command.name].push_back({ libraryIndex, commandIndex });
		}
	}
}

bool RegisterConstants(Program& program, std::string& error)
{
	for (const Library& library : program.libraries) {
		for (const auto& dataType : library.metadata.dataTypes) {
			if ((dataType.state & kLibraryEnumState) == 0 || dataType.name.empty()) continue;
			for (const auto& element : dataType.elements) {
				if (element.name.empty()) continue;
				Constant constant;
				constant.name = "#" + dataType.name + "." + element.name;
				constant.type = kTypeInt;
				constant.numberValue = element.defaultValue;
				program.constants.try_emplace(constant.name, std::move(constant));
			}
		}
		for (const auto& source : library.metadata.constants) {
			Constant constant;
			constant.name = "#" + source.name;
			constant.numberValue = source.numberValue;
			constant.textValue = source.textValue;
			switch (source.type) {
			case 0: constant.type = kTypeNull; break;
			case 1: constant.type = kTypeDouble; break;
			case 2: constant.type = kTypeBool; break;
			case 3: constant.type = kTypeText; break;
			default:
				error = "unsupported_support_library_constant_type:" + source.name + ":" + std::to_string(source.type);
				return false;
			}
			program.constants.try_emplace(constant.name, std::move(constant));
		}
	}
	return true;
}

}  // namespace

const TypeInfo* Program::FindType(const std::uint32_t code) const
{
	const auto found = typeByCode.find(code & ~kTypeArrayFlag);
	return found == typeByCode.end() ? nullptr : &types[found->second];
}

std::uint32_t Program::NormalizeLibraryType(const std::size_t libraryIndex, const std::uint32_t code) const
{
	if (code == kTypeNull || (code & 0x80000000u) != 0 || (code & 0xFFFF0000u) != 0) return code;
	return static_cast<std::uint32_t>(((libraryIndex + 1) << 16) | (code & 0xFFFFu));
}

bool BuildCompilerModel(
	e2txt::ProjectBundle bundle,
	const std::filesystem::path& inputRoot,
	const std::vector<std::filesystem::path>& supportLibrarySearchDirectories,
	const std::vector<std::string>& conditionMacros,
	Program& outProgram,
	std::string& outError)
{
	outProgram = {};
	outProgram.bundle = std::move(bundle);
	outProgram.inputRoot = inputRoot;
	outProgram.supportLibrarySearchDirectories = supportLibrarySearchDirectories;
	for (std::string macro : conditionMacros) {
		std::transform(macro.begin(), macro.end(), macro.begin(), [](const unsigned char value) {
			return static_cast<char>(std::toupper(value));
		});
		if (!macro.empty()) outProgram.conditionMacros.insert(std::move(macro));
	}
	outError.clear();
	RegisterSystemTypes(outProgram);
	if (!LoadLibraries(outProgram, outError)) return false;
	if (!RegisterLibraryTypes(outProgram, outError)) return false;
	if (!RegisterProjectTypes(outProgram, outError)) return false;
	if (!RegisterProjectGlobals(outProgram, outError)) return false;
	if (!RegisterProjectConstants(outProgram, outError)) return false;
	if (!RegisterDllCommands(outProgram, outError)) return false;
	if (!ParseSources(outProgram, outError)) return false;
	if (!RegisterClassTypes(outProgram, outError)) return false;
	if (!ResolveVariables(outProgram, outError)) return false;
	PopulateClassTypeFields(outProgram);
	RegisterCommands(outProgram);
	if (!RegisterConstants(outProgram, outError)) return false;
	// ResolveVariables runs before library commands are registered, so class
	// declarations that refer to support-library types need one final pass.
	if (!ResolveVariables(outProgram, outError)) return false;
	return true;
}

}  // namespace ecompiler
