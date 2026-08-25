#include "SourcePreflightValidator.h"

#include <Windows.h>

#include <algorithm>
#include <array>
#include <charconv>
#include <cerrno>
#include <cstdlib>
#include <cstdint>
#include <limits>
#include <optional>
#include <sstream>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>

#include "PathHelper.h"
#include "SourceExpressionParser.h"
#include "SourceSemanticValidator.h"
#include "e2txt.h"

namespace e2txt {
namespace {

constexpr size_t kMaximumFormattedDiagnostics = 40;

enum class ValueKind {
	Unknown,
	Numeric,
	Text,
	Logical,
	ByteSet,
	DateTime,
	Variant,
};

struct VariableSymbol {
	std::string typeName;
	bool isArray = false;
	bool arrayRankKnown = false;
};

struct ParsedDeclaration {
	std::vector<std::string> fields;
	bool quotesBalanced = true;
};

enum class FlowKind {
	IfTrue,
	IfElse,
	WhileLoop,
	DoWhileLoop,
	CountLoop,
	VariableLoop,
	Switch,
};

struct FlowFrame {
	FlowKind kind = FlowKind::IfTrue;
	size_t line = 0;
	bool sawElse = false;
	bool sawDefault = false;
};

struct MethodState {
	bool active = false;
	bool sawLocal = false;
	bool bodyStarted = false;
	std::string name;
	std::string returnType;
	std::unordered_map<std::string, VariableSymbol> symbols;
	std::vector<FlowFrame> flows;
	size_t commentedCountLoopStarts = 0;
};

std::string TrimAsciiCopy(std::string value)
{
	auto isSpace = [](const unsigned char ch) {
		return ch == ' ' || ch == '\t' || ch == '\r' || ch == '\n' || ch == '\f' || ch == '\v';
	};
	while (!value.empty() && isSpace(static_cast<unsigned char>(value.front()))) {
		value.erase(value.begin());
	}
	while (!value.empty() && isSpace(static_cast<unsigned char>(value.back()))) {
		value.pop_back();
	}
	return value;
}

bool StartsWith(const std::string_view text, const std::string_view prefix)
{
	return text.size() >= prefix.size() && text.substr(0, prefix.size()) == prefix;
}

bool IsEscapedBodyLine(const std::string& line)
{
	const std::string trimmed = TrimAsciiCopy(line);
	return StartsWith(trimmed, "#e2txt_body_line#") ||
		StartsWith(trimmed, "' #e2txt_body_line#");
}

bool EndsWith(const std::string_view text, const std::string_view suffix)
{
	return text.size() >= suffix.size() && text.substr(text.size() - suffix.size()) == suffix;
}

size_t NextLocalCharacterIndex(const std::string& text, const size_t index)
{
	if (index >= text.size()) {
		return text.size();
	}
	const char* const begin = text.c_str();
	const char* const current = begin + index;
	const char* const next = CharNextExA(CP_ACP, current, 0);
	if (next == nullptr || next <= current) {
		return index + 1;
	}
	return (std::min)(text.size(), static_cast<size_t>(next - begin));
}

std::vector<std::string> SplitLines(std::string text)
{
	if (text.size() >= 3 &&
		static_cast<unsigned char>(text[0]) == 0xEF &&
		static_cast<unsigned char>(text[1]) == 0xBB &&
		static_cast<unsigned char>(text[2]) == 0xBF) {
		text.erase(0, 3);
	}

	std::vector<std::string> lines;
	size_t start = 0;
	for (size_t index = 0; index < text.size(); ++index) {
		if (text[index] != '\r' && text[index] != '\n') {
			continue;
		}
		lines.push_back(text.substr(start, index - start));
		if (text[index] == '\r' && index + 1 < text.size() && text[index + 1] == '\n') {
			++index;
		}
		start = index + 1;
	}
	lines.push_back(text.substr(start));
	return lines;
}

std::string LocalTextToUtf8(const std::string& text)
{
	if (text.empty()) {
		return std::string();
	}
	const int wideLength = MultiByteToWideChar(
		CP_ACP,
		0,
		text.data(),
		static_cast<int>(text.size()),
		nullptr,
		0);
	if (wideLength <= 0) {
		return text;
	}
	std::wstring wide(static_cast<size_t>(wideLength), L'\0');
	if (MultiByteToWideChar(
			CP_ACP,
			0,
			text.data(),
			static_cast<int>(text.size()),
			wide.data(),
			wideLength) <= 0) {
		return text;
	}
	return WideToUtf8Text(wide);
}

std::string DiagnosticPathToUtf8(const std::string& path)
{
	return LocalTextToUtf8(path);
}

void AddError(
	SourcePreflightReport& report,
	const std::string& path,
	const size_t line,
	const std::string& code,
	const std::string& message)
{
	report.errors.push_back(SourcePreflightDiagnostic {
		.filePath = DiagnosticPathToUtf8(path),
		.line = line,
		.code = code,
		.message = message,
	});
}

bool HasAsciiWhitespace(const std::string& text)
{
	return std::any_of(text.begin(), text.end(), [](const unsigned char ch) {
		return ch == ' ' || ch == '\t' || ch == '\r' || ch == '\n' || ch == '\f' || ch == '\v';
	});
}

bool MatchDirective(
	const std::string& line,
	const std::string_view keyword,
	std::string* outRest = nullptr)
{
	const std::string prefix = "." + std::string(keyword);
	if (!StartsWith(line, prefix)) {
		return false;
	}
	if (line.size() > prefix.size() && line[prefix.size()] != ' ' && line[prefix.size()] != '\t') {
		return false;
	}
	if (outRest != nullptr) {
		*outRest = TrimAsciiCopy(line.substr(prefix.size()));
	}
	return true;
}

std::string DirectiveToken(const std::string& line)
{
	if (line.empty() || line.front() != '.') {
		return std::string();
	}
	size_t end = 1;
	while (end < line.size() && line[end] != ' ' && line[end] != '\t' && line[end] != '(') {
		++end;
	}
	return line.substr(0, end);
}

ParsedDeclaration SplitDeclarationFields(const std::string& rest)
{
	ParsedDeclaration result;
	if (rest.empty()) {
		return result;
	}

	std::string field;
	bool inQuote = false;
	for (const char ch : rest) {
		if (ch == '"') {
			if (inQuote) {
				inQuote = false;
			}
			else if (TrimAsciiCopy(field).empty()) {
				inQuote = true;
			}
			field.push_back(ch);
			continue;
		}
		if (ch == ',' && !inQuote) {
			result.fields.push_back(TrimAsciiCopy(field));
			field.clear();
			continue;
		}
		field.push_back(ch);
	}
	result.fields.push_back(TrimAsciiCopy(field));
	result.quotesBalanced = !inQuote;
	return result;
}

std::string FieldOrEmpty(const ParsedDeclaration& declaration, const size_t index)
{
	return index < declaration.fields.size() ? declaration.fields[index] : std::string();
}

bool IsEmptyDeclaration(const ParsedDeclaration& declaration, const size_t structuralFieldCount)
{
	for (size_t index = 0; index < structuralFieldCount; ++index) {
		if (!TrimAsciiCopy(FieldOrEmpty(declaration, index)).empty()) {
			return false;
		}
	}
	return true;
}

ParsedDeclaration ParseDeclaration(
	const std::string& line,
	const std::string_view keyword,
	const std::string& path,
	const size_t lineNumber,
	SourcePreflightReport& report)
{
	std::string rest;
	if (!MatchDirective(line, keyword, &rest)) {
		AddError(report, path, lineNumber, "declaration_keyword_boundary_invalid", "declaration keyword must be followed by whitespace");
		return {};
	}
	ParsedDeclaration declaration = SplitDeclarationFields(rest);
	// Free-form description fields may contain ordinary ASCII quotes. Structural
	// fields validate their own quoting below (array dimensions and DLL names),
	// so a quote anywhere in the complete declaration is not by itself an error.
	return declaration;
}

void ValidateNameField(
	const ParsedDeclaration& declaration,
	const std::string& path,
	const size_t lineNumber,
	SourcePreflightReport& report)
{
	const std::string name = FieldOrEmpty(declaration, 0);
	if (name.empty()) {
		AddError(report, path, lineNumber, "declaration_name_missing", "declaration name is required");
		return;
	}
	if (HasAsciiWhitespace(name) || name.find(',') != std::string::npos) {
		AddError(report, path, lineNumber, "declaration_name_invalid", "declaration name cannot contain whitespace or a structural comma");
	}
}

void ValidateTypeField(
	const ParsedDeclaration& declaration,
	const size_t fieldIndex,
	const bool required,
	const std::string& path,
	const size_t lineNumber,
	SourcePreflightReport& report)
{
	const std::string typeName = FieldOrEmpty(declaration, fieldIndex);
	if (typeName.empty()) {
		if (required) {
			AddError(report, path, lineNumber, "declaration_type_missing", "declaration type is required; a missing comma may have merged name and type");
		}
		return;
	}
	if (HasAsciiWhitespace(typeName) || typeName.find(',') != std::string::npos) {
		AddError(report, path, lineNumber, "declaration_type_invalid", "type name cannot contain whitespace or a structural comma; check declaration slots");
	}
}

void ValidateExactField(
	const ParsedDeclaration& declaration,
	const size_t fieldIndex,
	const std::unordered_set<std::string>& allowed,
	const std::string& path,
	const size_t lineNumber,
	const std::string& code,
	SourcePreflightReport& report)
{
	const std::string value = FieldOrEmpty(declaration, fieldIndex);
	if (!allowed.contains(value)) {
		AddError(report, path, lineNumber, code, "declaration attribute is in the wrong slot or has an unsupported value");
	}
}

bool TryParseNonNegativeInteger(const std::string& rawText)
{
	const std::string text = TrimAsciiCopy(rawText);
	if (text.empty()) {
		return false;
	}
	std::uint64_t value = 0;
	const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
	return error == std::errc() && end == text.data() + text.size() &&
		value <= static_cast<std::uint64_t>((std::numeric_limits<std::int32_t>::max)());
}

void ValidateArrayField(
	const ParsedDeclaration& declaration,
	const size_t fieldIndex,
	const std::string& path,
	const size_t lineNumber,
	SourcePreflightReport& report)
{
	const std::string field = FieldOrEmpty(declaration, fieldIndex);
	if (field.empty()) {
		return;
	}
	if (field.size() < 2 || field.front() != '"' || field.back() != '"') {
		AddError(report, path, lineNumber, "array_dimension_quote_invalid", "array dimensions must be one quoted declaration field, for example \"0\" or \"2,3\"");
		return;
	}
	const std::string dimensions = field.substr(1, field.size() - 2);
	if (dimensions.empty()) {
		AddError(report, path, lineNumber, "array_dimension_empty", "quoted array dimensions cannot be empty");
		return;
	}
	size_t begin = 0;
	while (begin <= dimensions.size()) {
		const size_t comma = dimensions.find(',', begin);
		const std::string part = dimensions.substr(
			begin,
			comma == std::string::npos ? std::string::npos : comma - begin);
		if (!TryParseNonNegativeInteger(part)) {
			AddError(report, path, lineNumber, "array_dimension_invalid", "every array dimension must be a non-negative integer");
			return;
		}
		if (comma == std::string::npos) {
			break;
		}
		begin = comma + 1;
	}
}

bool HasZeroArrayDimension(const ParsedDeclaration& declaration, const size_t fieldIndex)
{
	const std::string field = FieldOrEmpty(declaration, fieldIndex);
	if (field.size() < 2 || field.front() != '"' || field.back() != '"') {
		return false;
	}
	const std::string dimensions = field.substr(1, field.size() - 2);
	size_t begin = 0;
	while (begin <= dimensions.size()) {
		const size_t comma = dimensions.find(',', begin);
		const std::string part = dimensions.substr(
			begin,
			comma == std::string::npos ? std::string::npos : comma - begin);
		std::uint64_t value = 0;
		const auto [end, error] = std::from_chars(part.data(), part.data() + part.size(), value);
		if (error == std::errc() && end == part.data() + part.size() && value == 0) {
			return true;
		}
		if (comma == std::string::npos) {
			break;
		}
		begin = comma + 1;
	}
	return false;
}

void ValidateQuotedDllField(
	const ParsedDeclaration& declaration,
	const size_t fieldIndex,
	const std::string& path,
	const size_t lineNumber,
	SourcePreflightReport& report)
{
	const std::string field = FieldOrEmpty(declaration, fieldIndex);
	if (field.empty()) {
		return;
	}
	if (field.size() < 2 || field.front() != '"' || field.back() != '"') {
		AddError(report, path, lineNumber, "dll_text_field_quote_invalid", "non-empty DLL file and entry fields must use ASCII declaration quotes");
	}
}

bool ValidateFlagWords(
	const std::string& flags,
	const std::unordered_set<std::string>& allowed)
{
	if (flags.empty()) {
		return true;
	}
	std::istringstream stream(flags);
	std::unordered_set<std::string> seen;
	std::string word;
	while (stream >> word) {
		if (!allowed.contains(word) || !seen.insert(word).second) {
			return false;
		}
	}
	return true;
}

void ValidateParameterFlags(
	const ParsedDeclaration& declaration,
	const bool dllParameter,
	const std::string& path,
	const size_t lineNumber,
	SourcePreflightReport& report)
{
	static const std::unordered_set<std::string> kMethodFlags = { "", "参考", "可空", "数组" };
	static const std::unordered_set<std::string> kDllFlags = { "", "传址", "数组" };
	const std::string flags = FieldOrEmpty(declaration, 2);
	if (!ValidateFlagWords(flags, dllParameter ? kDllFlags : kMethodFlags)) {
		AddError(report, path, lineNumber, "parameter_attribute_invalid", "parameter attributes are unsupported or in the wrong slot");
	}
}

void ValidateVariableDeclaration(
	const ParsedDeclaration& declaration,
	const std::string_view kind,
	const std::string& path,
	const size_t lineNumber,
	SourcePreflightReport& report)
{
	ValidateNameField(declaration, path, lineNumber, report);
	// 易语言变量/参数声明省略类型时默认为整数型；
	// 数据类型成员仍由其所在页的结构规则单独约束。
	ValidateTypeField(declaration, 1, false, path, lineNumber, report);
	if (kind == "程序集变量") {
		ValidateExactField(declaration, 2, { "" }, path, lineNumber, "class_variable_reserved_slot_not_empty", report);
	}
	else if (kind == "局部变量") {
		ValidateExactField(declaration, 2, { "", "静态" }, path, lineNumber, "local_variable_attribute_invalid", report);
	}
	else if (kind == "全局变量") {
		ValidateExactField(declaration, 2, { "", "公开" }, path, lineNumber, "global_variable_attribute_invalid", report);
	}
	else if (kind == "成员") {
		ValidateExactField(declaration, 2, { "", "传址" }, path, lineNumber, "struct_member_attribute_invalid", report);
	}
	ValidateArrayField(declaration, 3, path, lineNumber, report);
	if (kind == "成员" && HasZeroArrayDimension(declaration, 3)) {
		AddError(report, path, lineNumber, "struct_member_array_dimension_zero", "a data-type member cannot have a zero array dimension");
	}
}

bool IsQuotedArrayField(const ParsedDeclaration& declaration, const size_t index)
{
	const std::string field = FieldOrEmpty(declaration, index);
	return field.size() >= 2 && field.front() == '"' && field.back() == '"';
}

std::string StripInlineComment(const std::string& line)
{
	constexpr std::string_view kLeftQuote = "“";
	constexpr std::string_view kRightQuote = "”";
	bool inText = false;
	for (size_t index = 0; index < line.size();) {
		if (!inText && line.compare(index, kLeftQuote.size(), kLeftQuote) == 0) {
			inText = true;
			index += kLeftQuote.size();
			continue;
		}
		if (inText && line.compare(index, kRightQuote.size(), kRightQuote) == 0) {
			inText = false;
			index += kRightQuote.size();
			continue;
		}
		if (!inText && line[index] == '\'') {
			return line.substr(0, index);
		}
		index = NextLocalCharacterIndex(line, index);
	}
	return line;
}

bool ValidateBalancedBodySyntax(
	const std::string& rawCode,
	const std::string& path,
	const size_t lineNumber,
	SourcePreflightReport& report)
{
	constexpr std::string_view kLeftQuote = "“";
	constexpr std::string_view kRightQuote = "”";
	const std::string code = StripInlineComment(rawCode);
	bool inText = false;
	std::vector<char> delimiters;
	for (size_t index = 0; index < code.size();) {
		if (!inText && code.compare(index, kLeftQuote.size(), kLeftQuote) == 0) {
			inText = true;
			index += kLeftQuote.size();
			continue;
		}
		if (inText && code.compare(index, kRightQuote.size(), kRightQuote) == 0) {
			inText = false;
			index += kRightQuote.size();
			continue;
		}
		if (!inText && code.compare(index, kRightQuote.size(), kRightQuote) == 0) {
			AddError(report, path, lineNumber, "text_quote_unexpected_end", "text literal has a closing quote without a matching opening quote");
			return false;
		}
		if (inText) {
			index = NextLocalCharacterIndex(code, index);
			continue;
		}

		const char ch = code[index];
		if (ch == '(' || ch == '[' || ch == '{') {
			delimiters.push_back(ch);
		}
		else if (ch == ')' || ch == ']' || ch == '}') {
			const char expected = ch == ')' ? '(' : (ch == ']' ? '[' : '{');
			if (delimiters.empty() || delimiters.back() != expected) {
				AddError(report, path, lineNumber, "delimiter_unexpected_end", "closing delimiter does not match the current expression");
				return false;
			}
			delimiters.pop_back();
		}
		index = NextLocalCharacterIndex(code, index);
	}
	if (inText) {
		AddError(report, path, lineNumber, "text_quote_unclosed", "text literal quote is not closed on this statement");
		return false;
	}
	if (!delimiters.empty()) {
		AddError(report, path, lineNumber, "delimiter_unclosed", "expression delimiter is not closed on this statement");
		return false;
	}
	return true;
}

std::optional<size_t> FindTopLevelOperator(
	const std::string& code,
	const std::string_view target)
{
	constexpr std::string_view kLeftQuote = "“";
	constexpr std::string_view kRightQuote = "”";
	bool inText = false;
	int parentheses = 0;
	int brackets = 0;
	int braces = 0;
	for (size_t index = 0; index < code.size();) {
		if (!inText && code.compare(index, kLeftQuote.size(), kLeftQuote) == 0) {
			inText = true;
			index += kLeftQuote.size();
			continue;
		}
		if (inText && code.compare(index, kRightQuote.size(), kRightQuote) == 0) {
			inText = false;
			index += kRightQuote.size();
			continue;
		}
		if (inText) {
			index = NextLocalCharacterIndex(code, index);
			continue;
		}
		if (parentheses == 0 && brackets == 0 && braces == 0 &&
			code.compare(index, target.size(), target) == 0) {
			return index;
		}
		switch (code[index]) {
		case '(': ++parentheses; break;
		case ')': --parentheses; break;
		case '[': ++brackets; break;
		case ']': --brackets; break;
		case '{': ++braces; break;
		case '}': --braces; break;
		default: break;
		}
		index = NextLocalCharacterIndex(code, index);
	}
	return std::nullopt;
}

std::string NormalizeSimpleVariableName(std::string expression, bool& outIndexed)
{
	expression = TrimAsciiCopy(std::move(expression));
	outIndexed = false;
	const size_t bracket = expression.find('[');
	if (bracket != std::string::npos) {
		if (!EndsWith(expression, "]")) {
			return std::string();
		}
		outIndexed = true;
		expression = TrimAsciiCopy(expression.substr(0, bracket));
	}
	if (expression.empty() || HasAsciiWhitespace(expression) ||
		expression.find_first_of(".(){}") != std::string::npos) {
		return std::string();
	}
	return expression;
}

ValueKind ClassifyTypeName(const std::string& rawTypeName)
{
	const std::string typeName = TrimAsciiCopy(rawTypeName);
	static const std::unordered_set<std::string> kNumericTypes = {
		"字节型", "短整数型", "整数型", "长整数型", "小数型", "双精度小数型",
	};
	if (kNumericTypes.contains(typeName)) {
		return ValueKind::Numeric;
	}
	if (typeName == "文本型") {
		return ValueKind::Text;
	}
	if (typeName == "逻辑型") {
		return ValueKind::Logical;
	}
	if (typeName == "字节集") {
		return ValueKind::ByteSet;
	}
	if (typeName == "日期时间型") {
		return ValueKind::DateTime;
	}
	if (typeName == "变体型") {
		return ValueKind::Variant;
	}
	return ValueKind::Unknown;
}

bool TryParseNumberLiteral(std::string text)
{
	text = TrimAsciiCopy(std::move(text));
	const std::string fullMinus = "－";
	if (StartsWith(text, fullMinus)) {
		text.replace(0, fullMinus.size(), "-");
	}
	if (text.empty()) {
		return false;
	}
	char* end = nullptr;
	errno = 0;
	(void)std::strtod(text.c_str(), &end);
	return errno != ERANGE && end != text.c_str() && end != nullptr && *end == '\0';
}

ValueKind ClassifyExpression(
	const std::string& rawExpression,
	const std::unordered_map<std::string, VariableSymbol>& methodSymbols,
	const std::unordered_map<std::string, VariableSymbol>& classSymbols,
	const std::unordered_map<std::string, VariableSymbol>& globalSymbols)
{
	const std::string expression = TrimAsciiCopy(rawExpression);
	if (expression.empty()) {
		return ValueKind::Unknown;
	}
	if (StartsWith(expression, "“") && EndsWith(expression, "”")) {
		return ValueKind::Text;
	}
	if (expression == "真" || expression == "假") {
		return ValueKind::Logical;
	}
	if (StartsWith(expression, "{") && EndsWith(expression, "}")) {
		return ValueKind::ByteSet;
	}
	if (TryParseNumberLiteral(expression)) {
		return ValueKind::Numeric;
	}

	bool indexed = false;
	const std::string variableName = NormalizeSimpleVariableName(expression, indexed);
	auto findSymbolKind = [&](const auto& symbols) -> std::optional<ValueKind> {
		const auto it = symbols.find(variableName);
		if (it == symbols.end()) {
			return std::nullopt;
		}
		if (it->second.isArray && !indexed) {
			return ValueKind::Unknown;
		}
		if (indexed && it->second.isArray && !it->second.arrayRankKnown) {
			return ValueKind::Unknown;
		}
		const ValueKind kind = ClassifyTypeName(it->second.typeName);
		// A scalar byte-set is indexed to a byte; an array whose element type is
		// byte-set remains a byte-set after one array index.
		return indexed && !it->second.isArray && kind == ValueKind::ByteSet ? ValueKind::Numeric : kind;
	};
	if (!variableName.empty()) {
		if (const auto value = findSymbolKind(methodSymbols); value.has_value()) {
			return *value;
		}
		if (const auto value = findSymbolKind(classSymbols); value.has_value()) {
			return *value;
		}
		if (const auto value = findSymbolKind(globalSymbols); value.has_value()) {
			return *value;
		}
	}

	const std::array<std::pair<std::string_view, ValueKind>, 8> knownConversions = {
		std::pair<std::string_view, ValueKind>{ "到文本 (", ValueKind::Text },
		{ "到整数 (", ValueKind::Numeric },
		{ "到数值 (", ValueKind::Numeric },
		{ "到小数 (", ValueKind::Numeric },
		{ "到字节 (", ValueKind::Numeric },
		{ "到字节集 (", ValueKind::ByteSet },
		{ "到时间 (", ValueKind::DateTime },
		{ "取反 (", ValueKind::Logical },
	};
	for (const auto& [prefix, kind] : knownConversions) {
		if (StartsWith(expression, prefix) && EndsWith(expression, ")")) {
			return kind;
		}
	}
	return ValueKind::Unknown;
}

bool AreDefinitelyIncompatible(const ValueKind target, const ValueKind source)
{
	if (target == ValueKind::Unknown || source == ValueKind::Unknown ||
		target == ValueKind::Variant || source == ValueKind::Variant) {
		return false;
	}
	return target != source;
}

const VariableSymbol* FindVariable(
	const std::string& name,
	const MethodState& method,
	const std::unordered_map<std::string, VariableSymbol>& classSymbols,
	const std::unordered_map<std::string, VariableSymbol>& globalSymbols)
{
	if (const auto it = method.symbols.find(name); it != method.symbols.end()) {
		return &it->second;
	}
	if (const auto it = classSymbols.find(name); it != classSymbols.end()) {
		return &it->second;
	}
	if (const auto it = globalSymbols.find(name); it != globalSymbols.end()) {
		return &it->second;
	}
	return nullptr;
}

void ValidateAssignment(
	const std::string& rawCode,
	const std::string& path,
	const size_t lineNumber,
	const MethodState& method,
	const std::unordered_map<std::string, VariableSymbol>& classSymbols,
	const std::unordered_map<std::string, VariableSymbol>& globalSymbols,
	SourcePreflightReport& report)
{
	const std::string code = TrimAsciiCopy(StripInlineComment(rawCode));
	if (code.empty() || code.front() == '.') {
		return;
	}
	size_t assignment = 0;
	size_t assignmentLength = 0;
	if (!FindSourceTopLevelAssignment(code, assignment, assignmentLength)) {
		return;
	}

	const std::string left = TrimAsciiCopy(code.substr(0, assignment));
	const std::string right = TrimAsciiCopy(code.substr(assignment + assignmentLength));
	if (left.empty() || right.empty()) {
		AddError(report, path, lineNumber, "assignment_empty_side", "assignment requires both a target and a value");
		return;
	}

	bool targetIndexed = false;
	const std::string targetName = NormalizeSimpleVariableName(left, targetIndexed);
	if (targetName.empty()) {
		return;
	}
	const VariableSymbol* target = FindVariable(targetName, method, classSymbols, globalSymbols);
	if (target == nullptr || (target->isArray && !targetIndexed)) {
		return;
	}
	const ValueKind declaredTargetKind = ClassifyTypeName(target->typeName);
	const ValueKind targetKind = targetIndexed && !target->isArray && declaredTargetKind == ValueKind::ByteSet
		? ValueKind::Numeric
		: declaredTargetKind;
	const ValueKind sourceKind = ClassifyExpression(right, method.symbols, classSymbols, globalSymbols);
	if (AreDefinitelyIncompatible(targetKind, sourceKind)) {
		AddError(report, path, lineNumber, "assignment_type_mismatch", "assignment source and target types are definitely incompatible");
	}
}

void CloseMethodFlows(
	MethodState& method,
	const std::string& path,
	SourcePreflightReport& report)
{
	for (const FlowFrame& frame : method.flows) {
		AddError(report, path, frame.line, "flow_end_missing", "flow-control block is not closed before the subprogram ends");
	}
	method.flows.clear();
}

bool FlowKindMatches(const FlowKind actual, const FlowKind expected)
{
	return actual == expected;
}

void PopExpectedFlow(
	MethodState& method,
	const FlowKind expected,
	const std::string& path,
	const size_t lineNumber,
	const std::string& code,
	SourcePreflightReport& report)
{
	if (method.flows.empty() || !FlowKindMatches(method.flows.back().kind, expected)) {
		AddError(report, path, lineNumber, code, "flow-control end marker does not match the current open block");
		return;
	}
	method.flows.pop_back();
}

void ValidateFlowDirective(
	const std::string& code,
	const std::string& path,
	const size_t lineNumber,
	MethodState& method,
	SourcePreflightReport& report)
{
	const std::string token = DirectiveToken(code);
	if (token == ".如果真") {
		method.flows.push_back(FlowFrame { .kind = FlowKind::IfTrue, .line = lineNumber });
	}
	else if (token == ".如果") {
		method.flows.push_back(FlowFrame { .kind = FlowKind::IfElse, .line = lineNumber });
	}
	else if (token == ".如果真结束") {
		PopExpectedFlow(method, FlowKind::IfTrue, path, lineNumber, "if_true_end_unexpected", report);
	}
	else if (token == ".否则") {
		if (method.flows.empty() || method.flows.back().kind != FlowKind::IfElse || method.flows.back().sawElse) {
			AddError(report, path, lineNumber, "else_unexpected", "else marker is only valid once inside an open if/else block");
		}
		else {
			method.flows.back().sawElse = true;
		}
	}
	else if (token == ".如果结束") {
		if (!method.flows.empty() && method.flows.back().kind == FlowKind::IfElse && !method.flows.back().sawElse) {
			AddError(report, path, lineNumber, "else_missing", "if/else flow requires an else marker before its end marker");
		}
		PopExpectedFlow(method, FlowKind::IfElse, path, lineNumber, "if_end_unexpected", report);
	}
	else if (token == ".判断循环首") {
		method.flows.push_back(FlowFrame { .kind = FlowKind::WhileLoop, .line = lineNumber });
	}
	else if (token == ".判断循环尾") {
		PopExpectedFlow(method, FlowKind::WhileLoop, path, lineNumber, "while_end_unexpected", report);
	}
	else if (token == ".循环判断首") {
		method.flows.push_back(FlowFrame { .kind = FlowKind::DoWhileLoop, .line = lineNumber });
	}
	else if (token == ".循环判断尾") {
		PopExpectedFlow(method, FlowKind::DoWhileLoop, path, lineNumber, "do_while_end_unexpected", report);
	}
	else if (token == ".计次循环首") {
		method.flows.push_back(FlowFrame { .kind = FlowKind::CountLoop, .line = lineNumber });
	}
	else if (token == ".计次循环尾") {
		PopExpectedFlow(method, FlowKind::CountLoop, path, lineNumber, "count_loop_end_unexpected", report);
	}
	else if (token == ".变量循环首") {
		method.flows.push_back(FlowFrame { .kind = FlowKind::VariableLoop, .line = lineNumber });
	}
	else if (token == ".变量循环尾") {
		PopExpectedFlow(method, FlowKind::VariableLoop, path, lineNumber, "variable_loop_end_unexpected", report);
	}
	else if (token == ".判断开始") {
		method.flows.push_back(FlowFrame { .kind = FlowKind::Switch, .line = lineNumber });
	}
	else if (token == ".判断") {
		if (method.flows.empty() || method.flows.back().kind != FlowKind::Switch || method.flows.back().sawDefault) {
			AddError(report, path, lineNumber, "switch_case_unexpected", "switch case is outside a switch or follows the default branch");
		}
	}
	else if (token == ".默认") {
		if (method.flows.empty() || method.flows.back().kind != FlowKind::Switch || method.flows.back().sawDefault) {
			AddError(report, path, lineNumber, "switch_default_unexpected", "default marker is only valid once inside a switch");
		}
		else {
			method.flows.back().sawDefault = true;
		}
	}
	else if (token == ".判断结束") {
		PopExpectedFlow(method, FlowKind::Switch, path, lineNumber, "switch_end_unexpected", report);
	}
	else {
		AddError(report, path, lineNumber, "unknown_body_directive", "unknown dot directive would otherwise be packed as raw code");
	}
}

void InsertVariableSymbol(
	std::unordered_map<std::string, VariableSymbol>& symbols,
	const ParsedDeclaration& declaration,
	const std::string& path,
	const size_t lineNumber,
	const std::string& duplicateCode,
	SourcePreflightReport& report)
{
	const std::string name = FieldOrEmpty(declaration, 0);
	if (name.empty()) {
		return;
	}
	std::string typeName = FieldOrEmpty(declaration, 1);
	if (TrimAsciiCopy(typeName).empty()) {
		typeName = "整数型";
	}
	const auto [it, inserted] = symbols.emplace(
		name,
		VariableSymbol {
			.typeName = std::move(typeName),
			.isArray = IsQuotedArrayField(declaration, 3) ||
				FieldOrEmpty(declaration, 2).find("数组") != std::string::npos,
			.arrayRankKnown = !HasZeroArrayDimension(declaration, 3),
		});
	(void)it;
	if (!inserted) {
		AddError(report, path, lineNumber, duplicateCode, "duplicate declaration name in the same scope");
	}
}

void ValidateVersionLine(
	const std::vector<std::string>& lines,
	const std::string& path,
	const bool allowEmptyFile,
	SourcePreflightReport& report)
{
	for (size_t index = 0; index < lines.size(); ++index) {
		const std::string line = TrimAsciiCopy(lines[index]);
		if (line.empty() || StartsWith(line, "'")) {
			continue;
		}
		if (line != ".版本 2") {
			AddError(report, path, index + 1, "version_header_missing", "the first non-comment line must be exactly .version 2 in E-language syntax");
		}
		return;
	}
	if (!allowEmptyFile) {
		AddError(report, path, 1, "version_header_missing", "source page is empty and has no version header");
	}
}

void ValidateGlobalPage(
	const std::string& text,
	const std::string& path,
	std::unordered_map<std::string, VariableSymbol>& globalSymbols,
	SourcePreflightReport& report)
{
	const std::vector<std::string> lines = SplitLines(text);
	++report.checkedFiles;
	report.checkedLines += lines.size();
	ValidateVersionLine(lines, path, true, report);
	for (size_t index = 0; index < lines.size(); ++index) {
		const std::string line = TrimAsciiCopy(lines[index]);
		if (line.empty() || StartsWith(line, "'") || line == ".版本 2") {
			continue;
		}
		if (!MatchDirective(line, "全局变量")) {
			AddError(report, path, index + 1, "fixed_page_directive_invalid", "global-variable page may only contain global-variable declarations");
			continue;
		}
		const ParsedDeclaration declaration = ParseDeclaration(line, "全局变量", path, index + 1, report);
		// 旧工程的原生变量表可能保留一个完全空白的末尾网格行。
		if (IsEmptyDeclaration(declaration, 4)) {
			continue;
		}
		ValidateVariableDeclaration(declaration, "全局变量", path, index + 1, report);
		InsertVariableSymbol(globalSymbols, declaration, path, index + 1, "global_variable_duplicate", report);
	}
}

void ValidateStructPage(
	const std::string& text,
	const std::string& path,
	SourcePreflightReport& report)
{
	const std::vector<std::string> lines = SplitLines(text);
	++report.checkedFiles;
	report.checkedLines += lines.size();
	ValidateVersionLine(lines, path, true, report);
	std::unordered_set<std::string> structNames;
	bool hasCurrentStruct = false;
	for (size_t index = 0; index < lines.size(); ++index) {
		const std::string line = TrimAsciiCopy(lines[index]);
		if (line.empty() || StartsWith(line, "'") || line == ".版本 2") {
			continue;
		}
		if (MatchDirective(line, "数据类型")) {
			const ParsedDeclaration declaration = ParseDeclaration(line, "数据类型", path, index + 1, report);
			if (IsEmptyDeclaration(declaration, 2)) {
				hasCurrentStruct = false;
				continue;
			}
			ValidateNameField(declaration, path, index + 1, report);
			ValidateExactField(declaration, 1, { "", "公开" }, path, index + 1, "struct_attribute_invalid", report);
			const std::string name = FieldOrEmpty(declaration, 0);
			if (!name.empty() && !structNames.insert(name).second) {
				AddError(report, path, index + 1, "struct_duplicate", "duplicate data type name");
			}
			hasCurrentStruct = true;
			continue;
		}
		if (MatchDirective(line, "成员")) {
			const ParsedDeclaration declaration = ParseDeclaration(line, "成员", path, index + 1, report);
			// 原生数据类型表允许用空白成员行或仅带说明的成员行作分隔。
			if (IsEmptyDeclaration(declaration, 4)) {
				continue;
			}
			ValidateVariableDeclaration(declaration, "成员", path, index + 1, report);
			if (!hasCurrentStruct) {
				AddError(report, path, index + 1, "struct_member_without_owner", "member declaration must follow a data type declaration");
			}
			continue;
		}
		if (hasCurrentStruct && line.front() != '.') {
			// 声明说明可包含原生换行，拆包后后续行保持为说明正文。
			continue;
		}
		AddError(report, path, index + 1, "fixed_page_directive_invalid", "data-type page may only contain data-type and member declarations");
	}
}

void ValidateDllPage(
	const std::string& text,
	const std::string& path,
	SourcePreflightReport& report)
{
	const std::vector<std::string> lines = SplitLines(text);
	++report.checkedFiles;
	report.checkedLines += lines.size();
	ValidateVersionLine(lines, path, true, report);
	std::unordered_set<std::string> dllNames;
	bool hasCurrentDll = false;
	for (size_t index = 0; index < lines.size(); ++index) {
		const std::string line = TrimAsciiCopy(lines[index]);
		if (line.empty() || StartsWith(line, "'") || line == ".版本 2") {
			continue;
		}
		if (MatchDirective(line, "DLL命令")) {
			const ParsedDeclaration declaration = ParseDeclaration(line, "DLL命令", path, index + 1, report);
			if (IsEmptyDeclaration(declaration, 5)) {
				hasCurrentDll = false;
				continue;
			}
			ValidateNameField(declaration, path, index + 1, report);
			ValidateTypeField(declaration, 1, false, path, index + 1, report);
			ValidateQuotedDllField(declaration, 2, path, index + 1, report);
			ValidateQuotedDllField(declaration, 3, path, index + 1, report);
			ValidateExactField(declaration, 4, { "", "公开" }, path, index + 1, "dll_public_attribute_invalid", report);
			const std::string name = FieldOrEmpty(declaration, 0);
			if (!name.empty() && !dllNames.insert(name).second) {
				AddError(report, path, index + 1, "dll_command_duplicate", "duplicate DLL command name");
			}
			hasCurrentDll = true;
			continue;
		}
		if (MatchDirective(line, "参数")) {
			const ParsedDeclaration declaration = ParseDeclaration(line, "参数", path, index + 1, report);
			if (IsEmptyDeclaration(declaration, 3)) {
				continue;
			}
			ValidateNameField(declaration, path, index + 1, report);
			ValidateTypeField(declaration, 1, false, path, index + 1, report);
			ValidateParameterFlags(declaration, true, path, index + 1, report);
			if (!hasCurrentDll) {
				AddError(report, path, index + 1, "dll_parameter_without_owner", "DLL parameter must follow a DLL command declaration");
			}
			continue;
		}
		if (hasCurrentDll && line.front() != '.') {
			// DLL 命令及参数的说明字段可以包含原生换行。
			continue;
		}
		AddError(report, path, index + 1, "fixed_page_directive_invalid", "DLL page may only contain DLL-command and parameter declarations");
	}
}

void ValidateConstantPage(
	const std::string& text,
	const std::string& path,
	SourcePreflightReport& report)
{
	const std::vector<std::string> lines = SplitLines(text);
	++report.checkedFiles;
	report.checkedLines += lines.size();
	ValidateVersionLine(lines, path, true, report);
	std::unordered_set<std::string> names;
	for (size_t index = 0; index < lines.size(); ++index) {
		const std::string line = TrimAsciiCopy(lines[index]);
		if (line.empty() || StartsWith(line, "'") || line == ".版本 2") {
			continue;
		}
		if (!MatchDirective(line, "常量")) {
			AddError(report, path, index + 1, "fixed_page_directive_invalid", "constant page may only contain constant declarations");
			continue;
		}
		const ParsedDeclaration declaration = ParseDeclaration(line, "常量", path, index + 1, report);
		// 旧工程常量表会把空白分隔行导出成 `.常量 , ""`。
		if (FieldOrEmpty(declaration, 0).empty() &&
			FieldOrEmpty(declaration, 1) == "\"\"") {
			continue;
		}
		// 原生常量表允许保留没有公开名称的内部常量槽位。
		if (!FieldOrEmpty(declaration, 0).empty()) {
			ValidateNameField(declaration, path, index + 1, report);
		}
		if (FieldOrEmpty(declaration, 1).empty()) {
			AddError(report, path, index + 1, "constant_value_missing", "constant value is required");
		}
		ValidateExactField(declaration, 2, { "", "公开" }, path, index + 1, "constant_public_attribute_invalid", report);
		const std::string name = FieldOrEmpty(declaration, 0);
		if (!name.empty() && !names.insert(name).second) {
			AddError(report, path, index + 1, "constant_duplicate", "duplicate constant name");
		}
	}
}

void ValidateProgramPage(
	const BundleSourceFile& file,
	const std::unordered_map<std::string, VariableSymbol>& globalSymbols,
	const bool ecBridge,
	SourcePreflightReport& report)
{
	const std::string path = file.relativePath.empty() ? file.logicalName : file.relativePath;
	const std::vector<std::string> lines = SplitLines(file.content);
	++report.checkedFiles;
	report.checkedLines += lines.size();
	ValidateVersionLine(lines, path, false, report);

	bool sawAssembly = false;
	bool sawMethod = false;
	std::unordered_map<std::string, VariableSymbol> classSymbols;
	std::unordered_set<std::string> methodNames;
	MethodState method;
	for (size_t index = 0; index < lines.size(); ++index) {
		const size_t lineNumber = index + 1;
		std::string line = TrimAsciiCopy(lines[index]);
		if (line.empty()) {
			continue;
		}
		if (StartsWith(line, "'")) {
			if (ecBridge && StartsWith(line, "' .计次循环首")) {
				++method.commentedCountLoopStarts;
			}
			continue;
		}
		if (ecBridge && IsEscapedBodyLine(line)) {
			continue;
		}
		if (line == ".版本 2") {
			if (index != 0 && sawAssembly) {
				AddError(report, path, lineNumber, "version_header_misplaced", "version header is only valid at the start of a source page");
			}
			continue;
		}
		if (MatchDirective(line, "支持库")) {
			if (sawAssembly) {
				AddError(report, path, lineNumber, "support_library_misplaced", "support-library declaration must appear before the assembly declaration");
			}
			continue;
		}
		if (MatchDirective(line, "程序集")) {
			if (sawAssembly) {
				AddError(report, path, lineNumber, "assembly_duplicate", "source page must contain exactly one assembly declaration");
			}
			if (method.active) {
				CloseMethodFlows(method, path, report);
			}
			const ParsedDeclaration declaration = ParseDeclaration(line, "程序集", path, lineNumber, report);
			ValidateNameField(declaration, path, lineNumber, report);
			ValidateTypeField(declaration, 1, false, path, lineNumber, report);
			ValidateExactField(declaration, 2, { "", "公开" }, path, lineNumber, "assembly_public_attribute_invalid", report);
			sawAssembly = true;
			continue;
		}

		if (!sawAssembly) {
			AddError(report, path, lineNumber, "assembly_header_missing", "declarations and code cannot appear before the assembly header");
			continue;
		}
		if (MatchDirective(line, "程序集变量")) {
			const ParsedDeclaration declaration = ParseDeclaration(line, "程序集变量", path, lineNumber, report);
			if (IsEmptyDeclaration(declaration, 4)) {
				continue;
			}
			ValidateVariableDeclaration(declaration, "程序集变量", path, lineNumber, report);
			if (sawMethod) {
				AddError(report, path, lineNumber, "class_variable_misplaced", "assembly variables must appear before the first subprogram");
			}
			InsertVariableSymbol(classSymbols, declaration, path, lineNumber, "class_variable_duplicate", report);
			continue;
		}
		if (MatchDirective(line, "子程序")) {
			if (method.active) {
				CloseMethodFlows(method, path, report);
			}
			method = {};
			method.active = true;
			const ParsedDeclaration declaration = ParseDeclaration(line, "子程序", path, lineNumber, report);
			ValidateNameField(declaration, path, lineNumber, report);
			ValidateTypeField(declaration, 1, false, path, lineNumber, report);
			ValidateExactField(declaration, 2, { "", "公开" }, path, lineNumber, "subprogram_public_attribute_invalid", report);
			method.name = FieldOrEmpty(declaration, 0);
			method.returnType = FieldOrEmpty(declaration, 1);
			if (!method.name.empty() && !methodNames.insert(method.name).second) {
				AddError(report, path, lineNumber, "subprogram_duplicate", "duplicate subprogram name in the same source page");
			}
			sawMethod = true;
			continue;
		}
		if (MatchDirective(line, "参数")) {
			const ParsedDeclaration declaration = ParseDeclaration(line, "参数", path, lineNumber, report);
			if (IsEmptyDeclaration(declaration, 3)) {
				continue;
			}
			ValidateNameField(declaration, path, lineNumber, report);
			ValidateTypeField(declaration, 1, false, path, lineNumber, report);
			ValidateParameterFlags(declaration, false, path, lineNumber, report);
			if (!method.active || method.bodyStarted || method.sawLocal) {
				AddError(report, path, lineNumber, "parameter_misplaced", "parameters must directly follow a subprogram and precede local variables");
			}
			InsertVariableSymbol(method.symbols, declaration, path, lineNumber, "method_variable_duplicate", report);
			continue;
		}
		if (MatchDirective(line, "局部变量")) {
			const ParsedDeclaration declaration = ParseDeclaration(line, "局部变量", path, lineNumber, report);
			if (IsEmptyDeclaration(declaration, 4)) {
				continue;
			}
			ValidateVariableDeclaration(declaration, "局部变量", path, lineNumber, report);
			if (!method.active || method.bodyStarted) {
				AddError(report, path, lineNumber, "local_variable_misplaced", "local variables must appear before the first subprogram statement");
			}
			method.sawLocal = true;
			InsertVariableSymbol(method.symbols, declaration, path, lineNumber, "method_variable_duplicate", report);
			continue;
		}

		if (!method.active) {
			AddError(report, path, lineNumber, "code_outside_subprogram", "executable code must belong to a subprogram");
			continue;
		}
		method.bodyStarted = true;
		ValidateBalancedBodySyntax(line, path, lineNumber, report);
		if (!line.empty() && line.front() == '.') {
			if (ecBridge && DirectiveToken(line) == ".计次循环尾" &&
				(method.flows.empty() || method.flows.back().kind != FlowKind::CountLoop) &&
				method.commentedCountLoopStarts != 0) {
				--method.commentedCountLoopStarts;
				continue;
			}
			ValidateFlowDirective(line, path, lineNumber, method, report);
		}
		else {
			ValidateAssignment(line, path, lineNumber, method, classSymbols, globalSymbols, report);
		}
	}
	if (!sawAssembly) {
		AddError(report, path, 1, "assembly_header_missing", "source page has no assembly declaration");
	}
	if (method.active) {
		CloseMethodFlows(method, path, report);
	}
}

void AppendFormattedDiagnostic(
	std::ostringstream& stream,
	const char* severity,
	const SourcePreflightDiagnostic& diagnostic)
{
	stream << "source_preflight_" << severity
		<< ": file=" << diagnostic.filePath
		<< ", line=" << diagnostic.line
		<< ", code=" << diagnostic.code;
	if (!diagnostic.message.empty()) {
		stream << ", detail=" << diagnostic.message;
	}
	stream << '\n';
}

}  // namespace

bool SourcePreflightReport::IsValid() const
{
	return errors.empty();
}

SourcePreflightReport ValidateProjectBundleSource(const ProjectBundle& bundle)
{
	SourcePreflightReport report;
	std::unordered_map<std::string, VariableSymbol> globalSymbols;
	ValidateGlobalPage(bundle.globalText, "src/.全局变量.txt", globalSymbols, report);
	ValidateStructPage(bundle.dataTypeText, "src/.数据类型.txt", report);
	ValidateDllPage(bundle.dllDeclareText, "src/.DLL声明.txt", report);
	ValidateConstantPage(bundle.constantText, "src/.常量.txt", report);
	for (const BundleSourceFile& file : bundle.sourceFiles) {
		ValidateProgramPage(file, globalSymbols, bundle.sourceFileKind == SourceFileKind::EC, report);
	}
	ValidateProjectBundleSemantics(bundle, report);
	if (bundle.sourceFileKind == SourceFileKind::EC) {
		report.errors.erase(
			std::remove_if(
				report.errors.begin(),
				report.errors.end(),
				[](const SourcePreflightDiagnostic& diagnostic) {
					return diagnostic.code == "struct_member_duplicate";
				}),
			report.errors.end());
	}
	return report;
}

std::string FormatSourcePreflightReport(const SourcePreflightReport& report)
{
	std::ostringstream stream;
	stream << "files=" << report.checkedFiles
		<< ", lines=" << report.checkedLines
		<< ", errors=" << report.errors.size()
		<< ", warnings=" << report.warnings.size();
	if (report.errors.empty() && report.warnings.empty()) {
		return stream.str();
	}
	stream << '\n';

	size_t formatted = 0;
	for (const SourcePreflightDiagnostic& diagnostic : report.errors) {
		if (formatted >= kMaximumFormattedDiagnostics) {
			break;
		}
		AppendFormattedDiagnostic(stream, "error", diagnostic);
		++formatted;
	}
	for (const SourcePreflightDiagnostic& diagnostic : report.warnings) {
		if (formatted >= kMaximumFormattedDiagnostics) {
			break;
		}
		AppendFormattedDiagnostic(stream, "warning", diagnostic);
		++formatted;
	}
	const size_t total = report.errors.size() + report.warnings.size();
	if (formatted < total) {
		stream << "source_preflight_diagnostics_omitted: count=" << (total - formatted) << '\n';
	}
	std::string result = stream.str();
	while (!result.empty() && (result.back() == '\r' || result.back() == '\n')) {
		result.pop_back();
	}
	return result;
}

}  // namespace e2txt
