#include "CompilerModel.h"

#include "../PathHelper.h"
#include "../EFolderCodec.h"

#include <Windows.h>

#include <algorithm>
#include <charconv>
#include <cstdlib>
#include <functional>
#include <fstream>
#include <limits>
#include <set>
#include <sstream>
#include <string_view>
#include <utility>

namespace ecompiler {
namespace {

constexpr std::uint32_t kLibraryEnumState = 1u << 22;
constexpr std::uint32_t kUserTypeMask = 0x40000000u;

std::size_t SystemTypeSize(std::uint32_t type, TargetArchitecture architecture);

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

bool IsRegularFilePath(const std::filesystem::path& path)
{
	std::error_code error;
	return std::filesystem::is_regular_file(path, error);
}

std::filesystem::path AbsolutePathValue(const std::filesystem::path& path)
{
	std::error_code error;
	const auto absolute = std::filesystem::absolute(path, error);
	return error ? path : absolute;
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

void AppendPageText(std::string& destination, const std::string& source)
{
	if (Trim(source).empty()) return;
	if (!destination.empty() && destination.back() != '\n' && destination.back() != '\r') destination += "\r\n";
	destination += source;
	if (destination.empty() || destination.back() != '\n') destination += "\r\n";
}

std::string RemoveEcomStartupMethod(const std::string& text);

bool ExpandEComDependencies(
	e2txt::ProjectBundle& bundle,
	const std::filesystem::path& inputRoot,
	const e2txt::ReadOptions& readOptions,
	std::string& error)
{
	std::vector<e2txt::Dependency> additionalDependencies;
	std::unordered_set<std::string> knownECom;
	for (const auto& dependency : bundle.dependencies) {
		if (dependency.kind != e2txt::DependencyKind::ECom) continue;
		const std::string identity = dependency.resolvedPath.empty() ? dependency.path : dependency.resolvedPath;
		if (!knownECom.insert(identity).second) continue;
		std::error_code ec;
		e2txt::ProjectBundle module;
		std::string moduleError;
		std::filesystem::path workspacePath;
		std::filesystem::path moduleFilePath;
		if (!dependency.localWorkspace.empty()) {
			workspacePath = Utf8PathToPath(dependency.localWorkspace);
			if (workspacePath.is_relative()) workspacePath = inputRoot / workspacePath;
		}
		bool moduleLoaded = false;
		// Native `.e` decoding already materializes each embedded `.ec` under
		// ecom/<name>. Prefer that semantic workspace: it was decoded by the
		// authoritative x86 helper and therefore retains the module's original
		// support-library command indices. Re-decoding the old `.ec` with a
		// target-architecture FNE can silently rename calls when command tables
		// evolved between core releases.
		bool workspaceHasUnresolvedImportedTypes = false;
		if (!workspacePath.empty() && std::filesystem::is_directory(workspacePath, ec)) {
			// Ordinary unpacking omits imported module pages. Such a workspace
			// retains generated `_Cls_0x...` type references without their class
			// declarations; use the compiler-bundle reader below in that case.
			std::error_code iteratorError;
			for (const auto& entry : std::filesystem::recursive_directory_iterator(workspacePath, iteratorError)) {
				if (!entry.is_regular_file(iteratorError) || entry.path().extension() != L".txt") continue;
				std::ifstream sourceInput(entry.path(), std::ios::binary);
				std::ostringstream sourceBuffer;
				sourceBuffer << sourceInput.rdbuf();
				if (sourceBuffer.str().find("_Cls_0x") != std::string::npos) {
					workspaceHasUnresolvedImportedTypes = true;
					break;
				}
			}
		}
		if (!workspacePath.empty() && !workspaceHasUnresolvedImportedTypes &&
			std::filesystem::is_directory(workspacePath, ec)) {
			e2txt::BundleDirectoryCodec codec;
			moduleLoaded = codec.ReadBundle(PathToUtf8(workspacePath), module, &moduleError);
			if (!moduleLoaded) {
				error = "ecom_module_workspace_read_failed:" + PathToUtf8(workspacePath) + ":" + moduleError;
				return false;
			}
		}
		if (!moduleLoaded) {
			std::filesystem::path path = dependency.resolvedPath.empty() ? Utf8PathToPath(dependency.path) : Utf8PathToPath(dependency.resolvedPath);
			if (!path.is_absolute()) path = inputRoot / path;
			if (!std::filesystem::is_regular_file(path, ec)) {
				const auto candidates = BuildModuleFileLookupCandidates(inputRoot, dependency.path);
				for (const auto& candidate : candidates) {
					if (std::filesystem::is_regular_file(candidate, ec)) {
						path = candidate;
						break;
					}
				}
			}
			moduleFilePath = path;
			if (!std::filesystem::is_regular_file(path, ec)) {
				error = "ecom_module_not_found:" + PathToUtf8(path);
				return false;
			}
			// `.ec` 内部已经保存了上游模块的导入实现。编译模式必须读取
			// 这些隐藏函数和声明，不能把模块再次降级为外部源码依赖。
			e2txt::Generator generator;
			if (!generator.GenerateCompilerBundle(PathToUtf8(path), module, &moduleError, readOptions)) {
				error = "ecom_module_read_failed:" + PathToUtf8(path) + ":" + moduleError;
				return false;
			}
		}
		const std::string moduleSourcePrefix = "ecom/" +
			(!module.projectName.empty()
				? module.projectName
				: (!moduleFilePath.empty() ? PathToUtf8(moduleFilePath.stem()) : workspacePath.filename().string()));
		for (auto source : module.sourceFiles) {
			const std::string sourcePath = source.relativePath.empty() ? source.logicalName : source.relativePath;
			source.relativePath = moduleSourcePrefix + "/" + sourcePath;
			source.content = RemoveEcomStartupMethod(source.content);
			if (source.content.find(".程序集 ") != std::string::npos) {
				const auto sourceLines = SplitLines(source.content);
				std::ostringstream renamed;
				for (const std::string& rawLine : sourceLines) {
					std::string line = rawLine;
					if (StartsWith(Trim(line), ".子程序 ")) {
						const std::string declaration = Trim(line.substr(line.find(".子程序 ") + std::string(".子程序 ").size()));
						if (declaration.rfind("_临时子程序", 0) == 0) line.replace(line.find("_临时子程序"), std::string("_临时子程序").size(), "__ecom_temp_subprogram");
					}
					renamed << line << "\r\n";
				}
				source.content = renamed.str();
			}
			if (!source.content.empty()) bundle.sourceFiles.push_back(std::move(source));
		}
		AppendPageText(bundle.dataTypeText, module.dataTypeText);
		AppendPageText(bundle.dllDeclareText, module.dllDeclareText);
		AppendPageText(bundle.constantText, module.constantText);
		AppendPageText(bundle.globalText, module.globalText);
		for (const auto& nested : module.dependencies) {
			if (nested.kind != e2txt::DependencyKind::ELib) continue;
			const std::string nestedName = nested.fileName.empty() ? nested.name : nested.fileName;
			bool exists = false;
			for (const auto& current : bundle.dependencies) {
				const std::string currentName = current.fileName.empty() ? current.name : current.fileName;
				if (current.kind == e2txt::DependencyKind::ELib && currentName == nestedName) { exists = true; break; }
			}
			if (!exists) additionalDependencies.push_back(nested);
		}
	}
	for (auto& dependency : additionalDependencies) bundle.dependencies.push_back(std::move(dependency));
	return true;
}

std::string RemoveEcomStartupMethod(const std::string& text)
{
	const auto lines = SplitLines(text);
	std::ostringstream output;
	bool skipping = false;
	for (const std::string& line : lines) {
		const std::string trimmed = Trim(StripComment(line));
		if (StartsWith(trimmed, ".子程序 ")) {
			const std::string declaration = Trim(trimmed.substr(std::string(".子程序 ").size()));
			const std::size_t comma = declaration.find(',');
			const std::string name = Trim(declaration.substr(0, comma));
			skipping = name == "_启动子程序";
		}
		if (!skipping) output << line << "\r\n";
	}
	return output.str();
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
			std::size_t size = memberType.isArray
				? (program.targetArchitecture == TargetArchitecture::X64 ? 8u : 4u)
				: SystemTypeSize(memberType.code, program.targetArchitecture);
			if (size == 0) {
				const auto nested = program.typeByCode.find(memberType.code);
				if (nested == program.typeByCode.end() || !calculate(nested->second, active)) return false;
				size = program.targetArchitecture == TargetArchitecture::X64 ? 8u : 4u;
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
	const std::vector<std::filesystem::path>& searchDirectories,
	const TargetArchitecture architecture)
{
	std::vector<std::filesystem::path> candidates;
	const auto isCompatible = [&](const std::filesystem::path& candidate) {
		const std::wstring extension = candidate.extension().wstring();
		if (extension != L".fne" && extension != L".fnr" && extension != L".dll") return true;
		std::ifstream input(candidate, std::ios::binary);
		if (!input) return true;
		std::uint16_t dosMagic = 0;
		std::int32_t peOffset = 0;
		input.read(reinterpret_cast<char*>(&dosMagic), sizeof(dosMagic));
		input.seekg(0x3C, std::ios::beg);
		input.read(reinterpret_cast<char*>(&peOffset), sizeof(peOffset));
		if (!input || dosMagic != 0x5A4D || peOffset < 0 || peOffset > 0x1000000) return true;
		input.seekg(peOffset + 4, std::ios::beg);
		std::uint16_t machine = 0;
		input.read(reinterpret_cast<char*>(&machine), sizeof(machine));
		if (!input) return true;
		return architecture == TargetArchitecture::X64 ? machine == 0x8664 : machine == 0x014c;
	};
	const auto appendCandidate = [&](const std::filesystem::path& candidate) {
		if (!candidate.empty() && isCompatible(candidate)) candidates.push_back(candidate);
	};
	// For an x64 build, an embedded dependency often still points to the
	// installed x86 FNE.  Search architecture-specific roots first and only
	// accept an explicitly resolved path after its PE header matches.
	if (!dependency.path.empty()) {
		const std::filesystem::path configured = Utf8PathToPath(dependency.path);
		appendCandidate(configured);
		if (configured.is_relative()) appendCandidate(inputRoot / configured);
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
			for (const auto& directory : searchDirectories) {
				if (architecture == TargetArchitecture::X64) {
					appendCandidate(directory / L"x64" / variant);
				}
				appendCandidate(directory / variant);
			}
			appendCandidate(inputRoot / L"x64" / variant);
			appendCandidate(inputRoot / variant);
		}
	}
	if (!dependency.resolvedPath.empty()) appendCandidate(Utf8PathToPath(dependency.resolvedPath));
	std::error_code ec;
	for (const auto& candidate : candidates) {
		if (std::filesystem::is_regular_file(candidate, ec)) return std::filesystem::absolute(candidate, ec);
		ec.clear();
	}
	return {};
}

// Support-library text exports are architecture-neutral interface metadata.
// They are useful when a project declares a library for which no target-
// architecture FNE is installed, but they must never be treated as executable
// implementations.  Keep this parser here so the compiler model has one
// fallback path for both source files and unpacked project directories.
struct SidecarRawArgument {
	support_library_public_info::ArgumentMetadata metadata;
	std::string typeName;
};

struct SidecarRawCommand {
	support_library_public_info::CommandMetadata metadata;
	std::string returnTypeName;
	std::vector<SidecarRawArgument> arguments;
};

struct SidecarRawElement {
	support_library_public_info::DataTypeElementMetadata metadata;
	std::string typeName;
};

struct SidecarRawType {
	support_library_public_info::DataTypeMetadata metadata;
	std::vector<SidecarRawElement> elements;
	std::vector<SidecarRawCommand> memberCommands;
};

std::string ConvertUtf8TextToLocal(std::string text)
{
	if (text.empty()) return text;
	const int wideLength = MultiByteToWideChar(
		CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()), nullptr, 0);
	if (wideLength <= 0) return text;
	std::wstring wide(static_cast<std::size_t>(wideLength), L'\0');
	if (MultiByteToWideChar(
		CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()),
		wide.data(), wideLength) <= 0) return text;
	const int localLength = WideCharToMultiByte(
		CP_ACP, 0, wide.data(), wideLength, nullptr, 0, nullptr, nullptr);
	if (localLength <= 0) return text;
	std::string local(static_cast<std::size_t>(localLength), '\0');
	if (WideCharToMultiByte(
		CP_ACP, 0, wide.data(), wideLength, local.data(), localLength,
		nullptr, nullptr) <= 0) return text;
	return local;
}

bool ReadSidecarText(const std::filesystem::path& path, std::string& outText)
{
	outText.clear();
	std::ifstream input(path, std::ios::binary);
	if (!input) return false;
	std::ostringstream buffer;
	buffer << input.rdbuf();
	if (!input.good() && !input.eof()) return false;
	std::string raw = buffer.str();
	const bool hasUtf8Bom = raw.size() >= 3 &&
		static_cast<unsigned char>(raw[0]) == 0xEF &&
		static_cast<unsigned char>(raw[1]) == 0xBB &&
		static_cast<unsigned char>(raw[2]) == 0xBF;
	if (hasUtf8Bom) raw.erase(0, 3);
	// e-packager's elib exports always carry UTF-8 BOM.  For hand-authored
	// sidecars, accept UTF-8 when it is valid and otherwise preserve ACP text.
	outText = hasUtf8Bom ? ConvertUtf8TextToLocal(std::move(raw)) : ConvertUtf8TextToLocal(raw);
	return true;
}

std::string SidecarFieldValue(
	const std::vector<std::string>& fields,
	const std::string_view prefix)
{
	for (const auto& field : fields) {
		if (StartsWith(field, prefix)) return Trim(field.substr(prefix.size()));
	}
	return {};
}

int SidecarCategoryValue(const std::string& value)
{
	if (value.find("成员命令") != std::string::npos) return -1;
	const std::size_t left = value.rfind('(');
	const std::size_t right = value.rfind(')');
	if (left == std::string::npos || right == std::string::npos || right <= left + 1) return 0;
	char* end = nullptr;
	const long number = std::strtol(value.substr(left + 1, right - left - 1).c_str(), &end, 10);
	return end != nullptr && *end == '\0' ? static_cast<int>(number) : 0;
}

std::uint16_t SidecarCommandState(const std::string& attributes)
{
	std::uint16_t state = 0;
	if (attributes.find("隐藏") != std::string::npos) state |= 1u << 2;
	if (attributes.find("不可用") != std::string::npos) state |= 1u << 3;
	if (attributes.find("发布版禁用") != std::string::npos) state |= 1u << 4;
	if (attributes.find("允许追加参数") != std::string::npos) state |= 1u << 5;
	if (attributes.find("返回数组") != std::string::npos) state |= 1u << 6;
	if (attributes.find("对象复制") != std::string::npos) state |= 1u << 7;
	if (attributes.find("对象释放") != std::string::npos) state |= 1u << 8;
	if (attributes.find("对象构造") != std::string::npos) state |= 1u << 9;
	return state;
}

std::uint32_t SidecarArgumentState(const std::string& attributes)
{
	std::uint32_t state = 0;
	if (attributes.find("默认值=") != std::string::npos) state |= 1u << 0;
	if (attributes.find("默认空") != std::string::npos) state |= 1u << 1;
	if (attributes.find("只接收变量") != std::string::npos) state |= 1u << 2;
	if (attributes.find("只接收变量数组") != std::string::npos) state |= 1u << 3;
	if (attributes.find("接收变量或数组") != std::string::npos) state |= 1u << 4;
	if (attributes.find("接收数组数据") != std::string::npos) state |= 1u << 5;
	if (attributes.find("接收任意类型") != std::string::npos) state |= 1u << 6;
	if (attributes.find("接收变量或表达式") != std::string::npos) state |= 1u << 9;
	return state;
}

bool ParseSidecarCommandHeader(
	const std::string& line,
	const std::string_view prefix,
	SidecarRawCommand& outCommand)
{
	if (!StartsWith(line, prefix)) return false;
	const std::vector<std::string> fields = SplitFields(line.substr(prefix.size()));
	if (fields.empty()) return false;
	outCommand = {};
	outCommand.metadata.name = Trim(fields[0]);
	if (outCommand.metadata.name == "<未命名>") outCommand.metadata.name.clear();
	outCommand.returnTypeName = SidecarFieldValue(fields, "返回值=");
	outCommand.metadata.category = static_cast<std::int16_t>(SidecarCategoryValue(SidecarFieldValue(fields, "分类=")));
	outCommand.metadata.englishName = SidecarFieldValue(fields, "英文名=");
	outCommand.metadata.state = SidecarCommandState(SidecarFieldValue(fields, "属性="));
	return true;
}

bool ParseSidecarArgumentLine(const std::string& line, SidecarRawArgument& outArgument)
{
	if (!StartsWith(line, ".参数 ")) return false;
	const std::vector<std::string> fields = SplitFields(line.substr(std::string(".参数 ").size()));
	if (fields.empty()) return false;
	outArgument = {};
	outArgument.metadata.name = Trim(fields[0]);
	if (fields.size() >= 2) outArgument.typeName = Trim(fields[1]);
	std::string attributes;
	for (std::size_t index = 2; index < fields.size(); ++index) {
		if (!attributes.empty()) attributes += ",";
		attributes += fields[index];
	}
	outArgument.metadata.state = SidecarArgumentState(attributes);
	const std::string defaultValue = SidecarFieldValue(fields, "默认值=");
	if (!defaultValue.empty()) {
		char* end = nullptr;
		const long parsed = std::strtol(defaultValue.c_str(), &end, 10);
		if (end != nullptr && end != defaultValue.c_str() && *end == '\0') {
			outArgument.metadata.defaultValue = static_cast<std::int32_t>(parsed);
		}
	}
	return true;
}

bool ParseSidecarTypeHeader(const std::string& line, SidecarRawType& outType)
{
	if (!StartsWith(line, ".数据类型 ")) return false;
	const std::vector<std::string> fields = SplitFields(line.substr(std::string(".数据类型 ").size()));
	if (fields.empty()) return false;
	outType = {};
	outType.metadata.name = Trim(fields[0]);
	outType.metadata.englishName = SidecarFieldValue(fields, "英文名=");
	const std::string attributes = SidecarFieldValue(fields, "属性=");
	const std::string typeKind = SidecarFieldValue(fields, "类型=");
	if (attributes.find("枚举") != std::string::npos || typeKind.find("枚举") != std::string::npos) {
		outType.metadata.state |= (1u << 22);
	}
	return !outType.metadata.name.empty();
}

bool ParseSidecarElementLine(const std::string& line, SidecarRawElement& outElement)
{
	if (!StartsWith(line, ".成员 ")) return false;
	const std::vector<std::string> fields = SplitFields(line.substr(std::string(".成员 ").size()));
	if (fields.size() < 2 || Trim(fields[0]).empty()) return false;
	outElement = {};
	outElement.metadata.name = Trim(fields[0]);
	outElement.typeName = Trim(fields[1]);
	if (outElement.typeName.size() >= 2 &&
		outElement.typeName.compare(outElement.typeName.size() - 2, 2, "[]") == 0) {
		outElement.metadata.isArray = true;
		outElement.typeName.erase(outElement.typeName.size() - 2);
	}
	const std::string attributes = fields.size() >= 3 ? fields[2] : std::string();
	if (attributes.find("数组") != std::string::npos) outElement.metadata.isArray = true;
	const std::string defaultValue = SidecarFieldValue(fields, "默认值=");
	if (!defaultValue.empty()) {
		char* end = nullptr;
		const long parsed = std::strtol(defaultValue.c_str(), &end, 10);
		if (end != nullptr && end != defaultValue.c_str() && *end == '\0') {
			outElement.metadata.defaultValue = static_cast<std::int32_t>(parsed);
		}
	}
	return true;
}

std::uint32_t SidecarTypeCode(
	std::string typeName,
	const std::unordered_map<std::string, std::uint32_t>& customTypes,
	bool& isArray)
{
	typeName = Trim(std::move(typeName));
	isArray = false;
	if (typeName.size() >= 2 && typeName.compare(typeName.size() - 2, 2, "[]") == 0) {
		isArray = true;
		typeName.erase(typeName.size() - 2);
		typeName = Trim(std::move(typeName));
	}
	if (typeName.empty() || typeName == "无返回值") return kTypeNull;
	if (typeName == "通用型") return kTypeAll;
	if (typeName == "字节型") return kTypeByte;
	if (typeName == "短整数型") return kTypeShort;
	if (typeName == "整数型") return kTypeInt;
	if (typeName == "长整数型") return kTypeInt64;
	if (typeName == "小数型") return kTypeFloat;
	if (typeName == "双精度小数型" || typeName == "数值") return kTypeDouble;
	if (typeName == "逻辑型") return kTypeBool;
	if (typeName == "日期时间型") return kTypeDateTime;
	if (typeName == "文本型") return kTypeText;
	if (typeName == "字节集" || typeName == "字节集型") return kTypeBinary;
	if (typeName == "子程序指针" || typeName == "子程序指针型") return kTypeSubroutine;
	if (const auto found = customTypes.find(typeName); found != customTypes.end()) return found->second;
	return kTypeNull;
}

int SidecarCommandMatchScore(
	const SidecarRawCommand& left,
	const SidecarRawCommand& right)
{
	if (left.arguments.size() != right.arguments.size()) return -1;
	if (!left.metadata.englishName.empty() && !right.metadata.englishName.empty() &&
		left.metadata.englishName == right.metadata.englishName) return 90;
	if (!left.metadata.name.empty() && !right.metadata.name.empty() &&
		left.metadata.name == right.metadata.name) return 100;
	if (left.metadata.name.empty() && left.metadata.englishName.empty() &&
		right.metadata.name.empty() && right.metadata.englishName.empty() &&
		left.metadata.state == right.metadata.state && left.metadata.category == right.metadata.category) return 60;
	return -1;
}

bool LoadSupportLibrarySidecarMetadata(
	const std::filesystem::path& sidecarPath,
	const e2txt::Dependency& dependency,
	support_library_public_info::LibraryMetadata& outMetadata,
	std::string& outError)
{
	outMetadata = {};
	outError.clear();
	std::string text;
	if (!ReadSidecarText(sidecarPath, text)) {
		outError = "open_support_library_sidecar_failed:" + PathToUtf8(sidecarPath);
		return false;
	}
	std::vector<SidecarRawCommand> commands;
	std::vector<SidecarRawType> types;
	std::vector<support_library_public_info::ConstantMetadata> constants;
	enum class Section { None, Commands, Types, Constants };
	Section section = Section::None;
	SidecarRawCommand* currentCommand = nullptr;
	SidecarRawCommand* currentMemberCommand = nullptr;
	SidecarRawType* currentType = nullptr;
	for (const std::string& rawLine : SplitLines(text)) {
		const std::string line = Trim(StripComment(rawLine));
		if (line == "[命令]") {
			section = Section::Commands;
			currentCommand = nullptr;
			currentMemberCommand = nullptr;
			currentType = nullptr;
			continue;
		}
		if (line == "[数据类型]") {
			section = Section::Types;
			currentCommand = nullptr;
			currentMemberCommand = nullptr;
			currentType = nullptr;
			continue;
		}
		if (line == "[常量]") {
			section = Section::Constants;
			currentCommand = nullptr;
			currentMemberCommand = nullptr;
			currentType = nullptr;
			continue;
		}
		if (line.empty()) continue;
		if (section == Section::Commands) {
			SidecarRawCommand command;
			if (ParseSidecarCommandHeader(line, ".命令 ", command)) {
				commands.push_back(std::move(command));
				currentCommand = &commands.back();
				continue;
			}
			SidecarRawArgument argument;
			if (currentCommand != nullptr && ParseSidecarArgumentLine(line, argument)) {
				currentCommand->arguments.push_back(std::move(argument));
			}
			continue;
		}
		if (section == Section::Types) {
			SidecarRawType type;
			if (ParseSidecarTypeHeader(line, type)) {
				types.push_back(std::move(type));
				currentType = &types.back();
				currentMemberCommand = nullptr;
				continue;
			}
			if (currentType == nullptr) continue;
			SidecarRawElement element;
			if (ParseSidecarElementLine(line, element)) {
				currentType->elements.push_back(std::move(element));
				currentMemberCommand = nullptr;
				continue;
			}
			SidecarRawCommand memberCommand;
			if (ParseSidecarCommandHeader(line, ".成员命令 ", memberCommand)) {
				currentType->memberCommands.push_back(std::move(memberCommand));
				currentMemberCommand = &currentType->memberCommands.back();
				continue;
			}
			SidecarRawArgument argument;
			if (currentMemberCommand != nullptr && ParseSidecarArgumentLine(line, argument)) {
				currentMemberCommand->arguments.push_back(std::move(argument));
			}
			continue;
		}
		if (section == Section::Constants && StartsWith(line, ".常量 ")) {
			const std::vector<std::string> fields = SplitFields(line.substr(std::string(".常量 ").size()));
			if (fields.empty()) continue;
			support_library_public_info::ConstantMetadata constant;
			constant.name = Trim(fields[0]);
			constant.type = 1;
			const std::string typeName = fields.size() >= 2 ? Trim(fields[1]) : std::string();
			if (typeName.find("文本") != std::string::npos) constant.type = 3;
			else if (typeName.find("逻辑") != std::string::npos) constant.type = 2;
			const std::string value = SidecarFieldValue(fields, "值=");
			if (constant.type == 3) {
				constant.textValue = DecodeConstantLiteral(value);
			}
			else {
				char* end = nullptr;
				const double parsed = std::strtod(value.c_str(), &end);
				if (end != nullptr && end != value.c_str()) constant.numberValue = parsed;
			}
			constant.englishName = SidecarFieldValue(fields, "英文名=");
			constants.push_back(std::move(constant));
		}
	}

	if (commands.empty() && types.empty() && constants.empty()) {
		outError = "support_library_sidecar_empty:" + PathToUtf8(sidecarPath);
		return false;
	}
	std::unordered_map<std::string, std::uint32_t> customTypes;
	for (std::size_t index = 0; index < types.size(); ++index) {
		types[index].metadata.index = index;
		customTypes.emplace(types[index].metadata.name, static_cast<std::uint32_t>(index + 1));
	}
	for (auto& command : commands) {
		bool ignoredArray = false;
		command.metadata.returnType = SidecarTypeCode(command.returnTypeName, customTypes, ignoredArray);
		if (ignoredArray) command.metadata.state |= 1u << 6;
		for (auto& argument : command.arguments) {
			bool ignoredArgumentArray = false;
			argument.metadata.dataType = SidecarTypeCode(argument.typeName, customTypes, ignoredArgumentArray);
		}
		command.metadata.arguments.reserve(command.arguments.size());
		for (const auto& argument : command.arguments) command.metadata.arguments.push_back(argument.metadata);
	}
	for (auto& type : types) {
		for (auto& member : type.memberCommands) {
			bool ignoredArray = false;
			member.metadata.returnType = SidecarTypeCode(member.returnTypeName, customTypes, ignoredArray);
			if (ignoredArray) member.metadata.state |= 1u << 6;
			for (auto& argument : member.arguments) {
				bool ignoredArgumentArray = false;
				argument.metadata.dataType = SidecarTypeCode(argument.typeName, customTypes, ignoredArgumentArray);
			}
			member.metadata.arguments.reserve(member.arguments.size());
			for (const auto& argument : member.arguments) member.metadata.arguments.push_back(argument.metadata);
			int bestScore = -1;
			std::size_t matched = static_cast<std::size_t>(-1);
			for (std::size_t index = 0; index < commands.size(); ++index) {
				const int score = SidecarCommandMatchScore(member, commands[index]);
				if (score > bestScore) {
					bestScore = score;
					matched = index;
				}
			}
			if (matched == static_cast<std::size_t>(-1)) {
				matched = commands.size();
				member.metadata.index = matched;
				commands.push_back(std::move(member));
			}
			type.metadata.commandIndexes.push_back(matched);
		}
		for (auto& element : type.elements) {
			bool elementArray = element.metadata.isArray;
			element.metadata.dataType = SidecarTypeCode(element.typeName, customTypes, elementArray);
			element.metadata.isArray = elementArray;
			type.metadata.elements.push_back(std::move(element.metadata));
		}
	}
	outMetadata.filePath = sidecarPath;
	outMetadata.fileName = dependency.fileName;
	outMetadata.name = dependency.name;
	outMetadata.guid = dependency.guid;
	outMetadata.majorVersion = 0;
	outMetadata.minorVersion = 0;
	outMetadata.commands.reserve(commands.size());
	for (std::size_t index = 0; index < commands.size(); ++index) {
		commands[index].metadata.index = index;
		commands[index].metadata.executeSymbol.clear();
		outMetadata.commands.push_back(std::move(commands[index].metadata));
	}
	outMetadata.dataTypes.reserve(types.size());
	for (auto& type : types) outMetadata.dataTypes.push_back(std::move(type.metadata));
	outMetadata.constants = std::move(constants);
	return true;
}

bool IsMetadataCompatibleWithDependency(
	const support_library_public_info::LibraryMetadata& metadata,
	const e2txt::Dependency& dependency)
{
	const auto normalize = [](std::string value) {
		value = Trim(std::move(value));
		std::transform(value.begin(), value.end(), value.begin(), [](const unsigned char ch) {
			return static_cast<char>(std::tolower(ch));
		});
		return value;
	};
	const std::string expectedGuid = normalize(dependency.guid);
	const std::string actualGuid = normalize(metadata.guid);
	const std::string expectedName = normalize(dependency.name);
	const std::string actualName = normalize(metadata.name);
	if (!expectedName.empty() && !actualName.empty() && expectedName != actualName) {
		// A source-only `.支持库 foo` declaration is initially represented as
		// fileName=foo,name=foo.  In that case the file stem is the identity and
		// the FNE's localized display name may legitimately differ.  An explicit
		// project dependency with a distinct display name must still fail closed.
		const std::filesystem::path expectedFile = Utf8PathToPath(dependency.fileName);
		const std::filesystem::path actualFile = !metadata.fileName.empty()
			? Utf8PathToPath(metadata.fileName)
			: metadata.filePath;
		const bool nameIsFileAlias = !dependency.fileName.empty() &&
			(expectedName == normalize(dependency.fileName) ||
				expectedName == normalize(expectedFile.stem().string()));
		const bool sameFileStem = !expectedFile.stem().empty() &&
			normalize(expectedFile.stem().string()) == normalize(actualFile.stem().string());
		if (!nameIsFileAlias || !sameFileStem) return false;
	}
	// Architecture-specific rebuilds occasionally carry a regenerated GUID
	// while retaining the library's stable visible name.  Use the GUID as the
	// identity check only when no name is available on either side.
	if (expectedName.empty() && actualName.empty() &&
		!expectedGuid.empty() && !actualGuid.empty() && expectedGuid != actualGuid) return false;
	return true;
}

std::filesystem::path ResolveSupportLibrarySidecarPath(
	const e2txt::Dependency& dependency,
	const std::filesystem::path& inputRoot,
	const std::vector<std::filesystem::path>& searchDirectories)
{
	std::vector<std::filesystem::path> candidates;
	const auto append = [&](std::filesystem::path candidate) {
		if (candidate.empty()) return;
		if (candidate.extension() != L".txt") candidate.replace_extension(L".txt");
		if (candidate.is_relative()) candidate = (inputRoot / candidate).lexically_normal();
		for (const auto& existing : candidates) {
			if (existing.lexically_normal() == candidate.lexically_normal()) return;
		}
		candidates.push_back(std::move(candidate));
	};
	if (!dependency.localWorkspace.empty()) append(Utf8PathToPath(dependency.localWorkspace));
	if (!dependency.resolvedPath.empty()) append(Utf8PathToPath(dependency.resolvedPath));
	const std::string libraryName = dependency.fileName.empty() ? dependency.name : dependency.fileName;
	if (!libraryName.empty()) {
		std::filesystem::path name = Utf8PathToPath(libraryName);
		name.replace_extension(L".txt");
		append(name);
		append(std::filesystem::path(L"elib") / name.filename());
		for (const auto& directory : searchDirectories) {
			if (directory.empty()) continue;
			std::filesystem::path direct = directory / name.filename();
			if (direct.extension() != L".txt") direct.replace_extension(L".txt");
			for (const auto& existing : candidates) {
				if (existing.lexically_normal() == direct.lexically_normal()) direct.clear();
			}
			if (!direct.empty()) candidates.push_back(std::move(direct));
			candidates.push_back(directory / L"elib" / name.filename());
		}
	}
	for (const auto& candidate : candidates) {
		if (IsRegularFilePath(candidate)) return AbsolutePathValue(candidate);
	}
	return {};
}

std::string NormalizeMetadataMatchName(std::string value)
{
	value = Trim(std::move(value));
	std::transform(value.begin(), value.end(), value.begin(), [](const unsigned char ch) {
		return static_cast<char>(std::tolower(ch));
	});
	return value;
}

int MetadataCommandMatchScore(
	const support_library_public_info::CommandMetadata& target,
	const support_library_public_info::CommandMetadata& source)
{
	if (target.arguments.size() != source.arguments.size()) return -1;
	const std::string targetEnglish = NormalizeMetadataMatchName(target.englishName);
	const std::string sourceEnglish = NormalizeMetadataMatchName(source.englishName);
	const std::string targetName = NormalizeMetadataMatchName(target.name);
	const std::string sourceName = NormalizeMetadataMatchName(source.name);
	// Chinese display names disambiguate overloaded English identifiers such
	// as GetType/SetType across different object types, so prefer them when
	// both tables provide a name.
	if (!targetName.empty() && targetName == sourceName) return 100;
	if (!targetEnglish.empty() && targetEnglish == sourceEnglish) return 90;
	if (targetEnglish.empty() && sourceEnglish.empty() &&
		targetName.empty() && sourceName.empty() &&
		target.state == source.state && target.category == source.category) return 60;
	return -1;
}

void MergeArchitectureNeutralMetadata(
	support_library_public_info::LibraryMetadata& target,
	const support_library_public_info::LibraryMetadata& source)
{
	const auto findTargetTypeIndex = [&](const support_library_public_info::DataTypeMetadata& sourceType) {
		const std::string sourceEnglish = NormalizeMetadataMatchName(sourceType.englishName);
		const std::string sourceName = NormalizeMetadataMatchName(sourceType.name);
		for (std::size_t index = 0; index < target.dataTypes.size(); ++index) {
			const auto& targetType = target.dataTypes[index];
			if (!sourceEnglish.empty() && sourceEnglish == NormalizeMetadataMatchName(targetType.englishName)) return index;
			if (!sourceName.empty() && sourceName == NormalizeMetadataMatchName(targetType.name)) return index;
		}
		return static_cast<std::size_t>(-1);
	};
	const auto remapTypeCode = [&](const std::uint32_t code) {
		const std::uint32_t arrayFlag = code & kTypeArrayFlag;
		const std::uint32_t base = code & ~kTypeArrayFlag;
		if ((base & 0x80000000u) != 0 || (base & 0x40000000u) != 0 ||
			base == kTypeNull || base > source.dataTypes.size()) return code;
		const std::size_t targetIndex = findTargetTypeIndex(source.dataTypes[static_cast<std::size_t>(base - 1)]);
		return targetIndex == static_cast<std::size_t>(-1)
			? code
			: static_cast<std::uint32_t>(targetIndex + 1) | arrayFlag;
	};
	// Match commands without assuming that x86 and x64 command tables have the
	// same index.  The visible/English name plus arity is the stable interface
	// identity; executeSymbol remains exclusively from the target FNE.
	std::vector<std::size_t> sourceForTarget(target.commands.size(), static_cast<std::size_t>(-1));
	std::unordered_set<std::size_t> usedSourceCommands;
	for (std::size_t targetIndex = 0; targetIndex < target.commands.size(); ++targetIndex) {
		int bestScore = -1;
		std::size_t bestSource = static_cast<std::size_t>(-1);
		for (std::size_t sourceIndex = 0; sourceIndex < source.commands.size(); ++sourceIndex) {
			if (usedSourceCommands.contains(sourceIndex)) continue;
			const int score = MetadataCommandMatchScore(target.commands[targetIndex], source.commands[sourceIndex]);
			if (score > bestScore) {
				bestScore = score;
				bestSource = sourceIndex;
			}
		}
		if (bestSource == static_cast<std::size_t>(-1)) continue;
		sourceForTarget[targetIndex] = bestSource;
		usedSourceCommands.insert(bestSource);
		const auto& sourceCommand = source.commands[bestSource];
		auto& targetCommand = target.commands[targetIndex];
		if (targetCommand.name.empty()) targetCommand.name = sourceCommand.name;
		if (targetCommand.englishName.empty()) targetCommand.englishName = sourceCommand.englishName;
		if (sourceCommand.returnType != kTypeNull || targetCommand.returnType == kTypeNull) {
			// The sidecar is the architecture-neutral declaration.  A rebuilt
			// FNE may publish a placeholder (_SDT_ALL) or omit the array bit;
			// use the declaration's concrete return type when the command identity
			// matches, while retaining the target execute symbol below.
			targetCommand.returnType = remapTypeCode(sourceCommand.returnType);
		}
		// These flags describe source-level ABI semantics and are safe to merge;
		// retain any target-only platform flags outside the public command mask.
		constexpr std::uint16_t kPublicCommandFlags =
			(1u << 2) | (1u << 3) | (1u << 4) | (1u << 5) | (1u << 6) |
			(1u << 7) | (1u << 8) | (1u << 9);
		targetCommand.state = static_cast<std::uint16_t>(
			(targetCommand.state & ~kPublicCommandFlags) | (sourceCommand.state & kPublicCommandFlags));
		if (sourceCommand.arguments.size() > targetCommand.arguments.size()) {
			// Some rebuilt FNEs omit leading/default arguments from the exported
			// table even though the command ABI and source syntax still expose
			// them. The sidecar is the architecture-neutral declaration, so use
			// its longer argument list and keep the target FNE execute symbol.
			targetCommand.arguments = sourceCommand.arguments;
		}
		if (targetCommand.arguments.size() == sourceCommand.arguments.size()) {
			for (std::size_t argumentIndex = 0; argumentIndex < targetCommand.arguments.size(); ++argumentIndex) {
				auto& targetArgument = targetCommand.arguments[argumentIndex];
				const auto& sourceArgument = sourceCommand.arguments[argumentIndex];
				if (targetArgument.name.empty()) targetArgument.name = sourceArgument.name;
				if (sourceArgument.dataType != kTypeNull || targetArgument.dataType == kTypeNull) {
					targetArgument.dataType = remapTypeCode(sourceArgument.dataType);
				}
				// Argument flags are the ABI contract for variable/array passing.
				targetArgument.state |= sourceArgument.state;
				if (targetArgument.defaultValue == 0) targetArgument.defaultValue = sourceArgument.defaultValue;
			}
		}
	}

	for (const auto& sourceType : source.dataTypes) {
		std::size_t targetTypeIndex = static_cast<std::size_t>(-1);
		const std::string sourceEnglish = NormalizeMetadataMatchName(sourceType.englishName);
		const std::string sourceName = NormalizeMetadataMatchName(sourceType.name);
		for (std::size_t index = 0; index < target.dataTypes.size(); ++index) {
			const auto& targetType = target.dataTypes[index];
			if (!sourceEnglish.empty() && sourceEnglish == NormalizeMetadataMatchName(targetType.englishName)) {
				targetTypeIndex = index;
				break;
			}
			if (targetTypeIndex == static_cast<std::size_t>(-1) &&
				!sourceName.empty() && sourceName == NormalizeMetadataMatchName(targetType.name)) {
				targetTypeIndex = index;
			}
		}
		if (targetTypeIndex == static_cast<std::size_t>(-1)) continue;
		auto& targetType = target.dataTypes[targetTypeIndex];
		if (targetType.name.empty()) targetType.name = sourceType.name;
		if (targetType.englishName.empty()) targetType.englishName = sourceType.englishName;
		targetType.state |= sourceType.state;
		if (targetType.elements.size() < sourceType.elements.size()) {
			targetType.elements = sourceType.elements;
			for (auto& element : targetType.elements) element.dataType = remapTypeCode(element.dataType);
		}
		const std::vector<std::size_t> originalTargetCommandIndexes = targetType.commandIndexes;
		targetType.commandIndexes.clear();
		for (const std::size_t sourceCommandIndex : sourceType.commandIndexes) {
			if (sourceCommandIndex >= source.commands.size()) continue;
			// Resolve a type's member command in its own context.  A global
			// greedy map is incorrect for repeated English names such as
			// GetType/GetDateTime; the visible member name and arity provide a
			// deterministic match and allow legitimate aliases to be reused.
			int bestScore = -1;
			std::size_t targetCommandIndex = static_cast<std::size_t>(-1);
			const SidecarRawCommand sourceCommand = [&]() {
				SidecarRawCommand result;
				result.metadata = source.commands[sourceCommandIndex];
				return result;
			}();
			const auto considerTargetCommand = [&](const std::size_t index) {
				SidecarRawCommand targetCommand;
				targetCommand.metadata = target.commands[index];
				const int score = SidecarCommandMatchScore(sourceCommand, targetCommand);
				if (score > bestScore) {
					bestScore = score;
					targetCommandIndex = index;
				}
			};
			if (!originalTargetCommandIndexes.empty()) {
				for (const std::size_t index : originalTargetCommandIndexes) {
					if (index < target.commands.size()) considerTargetCommand(index);
				}
			}
			if (bestScore < 0) {
				for (std::size_t index = 0; index < target.commands.size(); ++index) considerTargetCommand(index);
			}
			if (targetCommandIndex != static_cast<std::size_t>(-1)) targetType.commandIndexes.push_back(targetCommandIndex);
		}
	}

	std::unordered_map<std::string, std::size_t> targetConstantIndexes;
	for (std::size_t index = 0; index < target.constants.size(); ++index) {
		targetConstantIndexes.emplace(NormalizeMetadataMatchName(target.constants[index].name), index);
	}
	for (const auto& constant : source.constants) {
		const std::string key = NormalizeMetadataMatchName(constant.name);
		const auto found = targetConstantIndexes.find(key);
		if (found == targetConstantIndexes.end()) {
			targetConstantIndexes.emplace(key, target.constants.size());
			target.constants.push_back(constant);
		}
		else {
			// The neutral declaration carries the correct value/type when a
			// generated x64 FNE serialized a text constant as numeric zero.
			target.constants[found->second] = constant;
		}
	}
}

std::size_t SystemTypeSize(const std::uint32_t type, const TargetArchitecture architecture)
{
	switch (type) {
	case kTypeByte: return 1;
	case kTypeShort: return 2;
	case kTypeInt:
	case kTypeFloat:
	case kTypeBool: return 4;
	case kTypeText:
	case kTypeBinary:
	case kTypeSubroutine: return architecture == TargetArchitecture::X64 ? 8u : 4u;
	case kTypeInt64:
	case kTypeDouble:
	case kTypeDateTime: return 8;
	default: return 0;
	}
}

bool LoadLibraries(Program& program, std::string& error)
{
	const auto normalizeDependencyName = [](std::string value) {
		value = Trim(std::move(value));
		std::transform(value.begin(), value.end(), value.begin(), [](const unsigned char ch) {
			return static_cast<char>(std::tolower(ch));
		});
		return value;
	};
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
			dependency, program.inputRoot, program.supportLibrarySearchDirectories,
			program.targetArchitecture);
		Library library;
		library.ordinal = program.libraries.size();
		library.dependency = dependency;
		if (!path.empty()) {
			support_library_public_info::LibraryMetadata binaryMetadata;
			std::string binaryError;
			const bool binaryLoaded = support_library_public_info::LoadSupportLibraryMetadata(
				path, binaryMetadata, binaryError);
			if (!binaryLoaded || !IsMetadataCompatibleWithDependency(binaryMetadata, dependency)) {
				const std::filesystem::path sidecar = ResolveSupportLibrarySidecarPath(
					dependency, program.inputRoot, program.supportLibrarySearchDirectories);
				std::string sidecarError;
				if (sidecar.empty() || !LoadSupportLibrarySidecarMetadata(
						sidecar, dependency, library.metadata, sidecarError)) {
					error = "support_library_metadata_failed:" + PathToUtf8(path) + ":" +
					(binaryError.empty() ? std::string("library_identity_mismatch") : binaryError);
					return false;
				}
				library.implementationAvailable = false;
			}
			else {
				library.metadata = std::move(binaryMetadata);
				if (!library.metadata.name.empty() &&
					normalizeDependencyName(dependency.name) == normalizeDependencyName(dependency.fileName)) {
					library.dependency.name = library.metadata.name;
				}
				const std::filesystem::path sidecar = ResolveSupportLibrarySidecarPath(
					dependency, program.inputRoot, program.supportLibrarySearchDirectories);
				if (!sidecar.empty()) {
					support_library_public_info::LibraryMetadata neutralMetadata;
					std::string sidecarError;
					if (LoadSupportLibrarySidecarMetadata(
						sidecar, dependency, neutralMetadata, sidecarError)) {
		MergeArchitectureNeutralMetadata(library.metadata, neutralMetadata);
					}
				}
			}
		}
		else {
			const std::filesystem::path sidecar = ResolveSupportLibrarySidecarPath(
				dependency, program.inputRoot, program.supportLibrarySearchDirectories);
			std::string sidecarError;
			if (sidecar.empty() || !LoadSupportLibrarySidecarMetadata(
					sidecar, dependency, library.metadata, sidecarError)) {
				error = "support_library_not_found:" + dependency.fileName + ":" + dependency.name;
				return false;
			}
			library.implementationAvailable = false;
		}
		program.libraries.push_back(std::move(library));
	}
	if (program.libraries.empty()) {
		error = "support_library_dependency_missing";
		return false;
	}
	return true;
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
			// A recursive/opaque library object is represented by a pointer at
			// the language ABI boundary.  Keep it unresolved until a target
			// actually needs its inline layout.
			type.layoutComplete = false;
			type.size = sizeof(void*);
			return true;
		}
		const auto& source = program.libraries[type.libraryIndex].metadata.dataTypes[type.dataTypeIndex];
		std::size_t offset = 0;
		for (const auto& sourceElement : source.elements) {
			TypeElement element;
			element.name = sourceElement.name;
			element.type = { program.NormalizeLibraryType(type.libraryIndex, sourceElement.dataType), sourceElement.isArray, true };
			element.offset = offset;
			element.defaultValue = sourceElement.defaultValue;
			std::size_t elementSize = sourceElement.isArray
				? (program.targetArchitecture == TargetArchitecture::X64 ? 8u : 4u)
				: SystemTypeSize(element.type.code, program.targetArchitecture);
			if (elementSize == 0) {
				const auto nested = program.typeByCode.find(element.type.code);
				if (nested == program.typeByCode.end()) {
					type.layoutComplete = false;
					elementSize = sizeof(void*);
				}
				else {
					if (!calculate(nested->second, active)) {
						type.layoutComplete = false;
					}
					if (!program.types[nested->second].layoutComplete) type.layoutComplete = false;
					elementSize = sizeof(void*);
				}
				// 复合类型成员在易语言运行时中保存对象数据指针。
				elementSize = program.targetArchitecture == TargetArchitecture::X64 ? 8u : 4u;
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
	const auto sourceDeclaration = [&](const std::string& sourceFile, const std::size_t sourceLine) {
		for (const auto& source : program.bundle.sourceFiles) {
			const std::string path = source.relativePath.empty() ? source.logicalName : source.relativePath;
			if (path != sourceFile) continue;
			const std::vector<std::string> lines = SplitLines(source.content);
			if (sourceLine == 0 || sourceLine > lines.size()) break;
			return ":source=" + Trim(StripComment(lines[sourceLine - 1]));
		}
		return std::string();
	};
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
				error = method.sourceFile + ":" + std::to_string(variable.sourceLine) +
					": unknown_parameter_type:" + variable.typeName + ":method=" + method.name +
					sourceDeclaration(method.sourceFile, variable.sourceLine);
				return false;
			}
		}
		for (Variable& variable : method.locals) {
			variable.type = ResolveTypeName(program, variable.typeName, variable.type.isArray);
			if (!variable.type.valid) {
				error = method.sourceFile + ":" + std::to_string(variable.sourceLine) +
					": unknown_local_type:" + variable.typeName + ":method=" + method.name +
					sourceDeclaration(method.sourceFile, variable.sourceLine);
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
			const std::string methodKey = program.assemblies[assemblyIndex].isClass
				? program.assemblies[assemblyIndex].name + "." + method.name
				: method.name;
			if (!program.methodByName.emplace(methodKey, method.id).second) {
				error = "duplicate_method_name:" + methodKey;
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
	const TargetArchitecture targetArchitecture,
	const bool restrictSupportLibrarySearch,
	Program& outProgram,
	std::string& outError)
{
	outProgram = {};
	outProgram.bundle = std::move(bundle);
	outProgram.inputRoot = inputRoot;
	outProgram.supportLibrarySearchDirectories = supportLibrarySearchDirectories;
	outProgram.targetArchitecture = targetArchitecture;
	e2txt::ReadOptions moduleReadOptions;
	moduleReadOptions.supportLibrarySearchDirectories = supportLibrarySearchDirectories;
	moduleReadOptions.restrictSupportLibrarySearch = restrictSupportLibrarySearch;
	if (!ExpandEComDependencies(outProgram.bundle, inputRoot, moduleReadOptions, outError)) return false;
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
