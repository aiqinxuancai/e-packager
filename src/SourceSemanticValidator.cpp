#include "SourceSemanticValidator.h"

#include "SourceExpressionParser.h"
#include "SourcePreflightValidator.h"
#include "PathHelper.h"
#include "SimpleXmlDocument.h"
#include "e2txt.h"

#include <Windows.h>

#include <algorithm>
#include <cctype>
#include <charconv>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <limits>
#include <optional>
#include <sstream>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace e2txt {
namespace {

enum class ResolveState {
	Valid,
	Invalid,
	Unknown,
};

enum class TypeCategory {
	Unknown,
	Numeric,
	Text,
	Logical,
	ByteSet,
	DateTime,
	Generic,
	Object,
	Void,
};

struct TypeInfo {
	std::string name;
	bool array = false;
	// 数组参数可能只标记“数组”而没有导出维数，因此维数可未知。
	std::optional<std::size_t> arrayRank;
};

struct Symbol {
	TypeInfo type;
	bool lvalue = true;
};

struct Parameter {
	std::string name;
	TypeInfo type;
	bool optional = false;
	bool byReference = false;
	bool variableOnly = false;
};

struct Callable {
	std::string name;
	std::string ownerType;
	TypeInfo returnType;
	std::vector<Parameter> parameters;
	bool variadic = false;
	bool metadataKnown = true;
};

struct TypeSymbol {
	TypeInfo type;
	bool enumeration = false;
	std::unordered_map<std::string, Symbol> members;
	std::unordered_map<std::string, std::vector<Callable>> methods;
};

struct MethodSymbol {
	Callable callable;
	std::unordered_map<std::string, Symbol> symbols;
	std::size_t headerLine = 0;
	std::size_t firstBodyLine = 0;
	std::size_t lastBodyLine = 0;
};

struct ProgramSource {
	std::string path;
	std::vector<std::string> lines;
	std::string assemblyName;
	bool hasFormBinding = false;
	bool formSymbolsComplete = false;
	std::string formBaseType;
	std::unordered_map<std::string, Symbol> classSymbols;
	std::vector<MethodSymbol> methods;
};

struct SemanticModel {
	std::unordered_map<std::string, Symbol> globals;
	std::unordered_map<std::string, TypeSymbol> types;
	std::unordered_map<std::string, std::vector<Callable>> functions;
	std::unordered_map<std::string, std::vector<Callable>> memberFunctions;
	std::unordered_map<std::string, TypeInfo> constants;
	std::unordered_set<std::string> resources;
	std::vector<ProgramSource> programs;
	bool externalMetadataComplete = true;
	bool anyExternalMetadata = false;
};

struct EvaluatedExpression {
	ResolveState state = ResolveState::Unknown;
	TypeInfo type;
	bool lvalue = false;
	std::string name;
	bool indexedResult = false;
};

struct FlowContext {
	enum class Kind {
	IfTrue,
	IfElse,
	While,
	DoWhile,
	Count,
	Variable,
	Switch,
	};
	Kind kind = Kind::IfTrue;
	TypeInfo switchType;
	bool conditionSwitch = false;
	std::size_t line = 0;
};

struct EvaluationContext {
	const SemanticModel& model;
	const ProgramSource& program;
	const MethodSymbol& method;
	std::vector<FlowContext>* flows = nullptr;
	const std::string& path;
	std::size_t line = 0;
};

struct ParsedDeclaration {
	std::vector<std::string> fields;
};

std::string Trim(std::string value)
{
	while (!value.empty() && std::isspace(static_cast<unsigned char>(value.front())) != 0) value.erase(value.begin());
	while (!value.empty() && std::isspace(static_cast<unsigned char>(value.back())) != 0) value.pop_back();
	return value;
}

bool StartsWith(const std::string& text, const std::string_view prefix)
{
	return text.size() >= prefix.size() && text.compare(0, prefix.size(), prefix) == 0;
}

bool MatchDirective(const std::string& line, const std::string_view name, std::string* outRest = nullptr)
{
	const std::string prefix = "." + std::string(name);
	if (!StartsWith(line, prefix) || (line.size() > prefix.size() && line[prefix.size()] != ' ' && line[prefix.size()] != '\t')) return false;
	if (outRest != nullptr) *outRest = Trim(line.substr(prefix.size()));
	return true;
}

std::vector<std::string> SplitLines(const std::string& text)
{
	std::vector<std::string> lines;
	std::size_t begin = 0;
	for (std::size_t i = 0; i < text.size(); ++i) {
		if (text[i] != '\r' && text[i] != '\n') continue;
		lines.push_back(text.substr(begin, i - begin));
		if (text[i] == '\r' && i + 1 < text.size() && text[i + 1] == '\n') ++i;
		begin = i + 1;
	}
	lines.push_back(text.substr(begin));
	return lines;
}

ParsedDeclaration SplitFields(const std::string& rest)
{
	ParsedDeclaration result;
	std::string field;
	bool quoted = false;
	for (std::size_t index = 0; index < rest.size();) {
		const char ch = rest[index];
		if (ch == '"') {
			if (quoted) quoted = false;
			else if (Trim(field).empty()) quoted = true;
		}
		if (ch == ',' && !quoted) {
			result.fields.push_back(Trim(field));
			field.clear();
			++index;
		}
		else {
			const char* begin = rest.c_str();
			const char* current = begin + index;
			const char* next = CharNextExA(CP_ACP, current, 0);
			const std::size_t nextIndex = next == nullptr || next <= current ? index + 1 : (std::min)(rest.size(), static_cast<std::size_t>(next - begin));
			field.append(rest, index, nextIndex - index);
			index = nextIndex;
		}
	}
	if (!rest.empty()) result.fields.push_back(Trim(field));
	return result;
}

std::string Field(const ParsedDeclaration& declaration, const std::size_t index)
{
	return index < declaration.fields.size() ? declaration.fields[index] : std::string();
}

bool Contains(const std::string& text, const std::string_view value)
{
	return text.find(value) != std::string::npos;
}

std::optional<std::size_t> ParseArrayRank(const ParsedDeclaration& declaration)
{
	std::string dimensions = Trim(Field(declaration, 3));
	if (dimensions.empty()) return std::nullopt;
	if (dimensions.size() < 2 || dimensions.front() != '"' || dimensions.back() != '"') return std::nullopt;
	dimensions = dimensions.substr(1, dimensions.size() - 2);
	if (dimensions.empty()) return std::nullopt;
	std::size_t rank = 1;
	std::size_t begin = 0;
	for (;;) {
		const std::size_t comma = dimensions.find(',', begin);
		const std::string part = Trim(dimensions.substr(begin, comma == std::string::npos ? std::string::npos : comma - begin));
		// A zero dimension denotes a dynamically resized array. Its rank is
		// intentionally unknown until a RedefineArray operation supplies it.
		if (part == "0") return std::nullopt;
		if (comma == std::string::npos) break;
		++rank;
		begin = comma + 1;
	}
	return rank;
}

TypeInfo ParseType(
	std::string typeName,
	const bool arrayHint = false,
	std::optional<std::size_t> arrayRank = std::nullopt)
{
	typeName = Trim(std::move(typeName));
	bool array = arrayHint;
	if (typeName.size() >= 2 && typeName.compare(typeName.size() - 2, 2, "[]") == 0) {
		typeName.erase(typeName.size() - 2);
		array = true;
		if (!arrayRank.has_value()) arrayRank = 1;
	}
	if (!array) arrayRank.reset();
	return TypeInfo { .name = Trim(std::move(typeName)), .array = array, .arrayRank = arrayRank };
}

// 易语言变量、参数和成员声明可以省略类型，此时默认为整数型。
// 返回值声明不经过此函数：空返回值仍表示无返回值。
TypeInfo ParseDeclaredType(
	std::string typeName,
	const bool arrayHint = false,
	std::optional<std::size_t> arrayRank = std::nullopt)
{
	if (Trim(typeName).empty()) typeName = "整数型";
	return ParseType(std::move(typeName), arrayHint, arrayRank);
}

TypeCategory Category(const TypeInfo& type)
{
	if (type.name.empty()) return TypeCategory::Unknown;
	if (type.array) return TypeCategory::Object;
	if (type.name == "通用型" || type.name == "变体型" || type.name == "子语句") return TypeCategory::Generic;
	if (type.name == "无返回值") return TypeCategory::Void;
	if (type.name == "文本型") return TypeCategory::Text;
	if (type.name == "逻辑型") return TypeCategory::Logical;
	if (type.name == "字节集") return TypeCategory::ByteSet;
	if (type.name == "日期时间型") return TypeCategory::DateTime;
	static const std::unordered_set<std::string> numeric = {
		"字节型", "短整数型", "整数型", "长整数型", "小数型", "双精度小数型",
	};
	if (numeric.contains(type.name)) return TypeCategory::Numeric;
	return TypeCategory::Object;
}

bool IsKnownBuiltinType(const std::string& name)
{
	static const std::unordered_set<std::string> names = {
		"字节型", "短整数型", "整数型", "长整数型", "小数型", "双精度小数型",
		"逻辑型", "文本型", "字节集", "日期时间型", "变体型", "变体类型", "通用型",
		"子程序指针", "条件语句型", "子语句", "无返回值",
	};
	return names.contains(name);
}

bool IsNumeric(const TypeInfo& type) { return Category(type) == TypeCategory::Numeric; }
bool IsLogical(const TypeInfo& type) { return Category(type) == TypeCategory::Logical; }
bool IsGeneric(const TypeInfo& type) { return type.name == "通用型" || type.name == "变体型" || type.name == "子语句"; }

bool Compatible(const TypeInfo& target, const TypeInfo& source, const SemanticModel* model = nullptr)
{
	if (target.name.empty() || source.name.empty()) return false;
	if (IsGeneric(target)) return true;
	if (IsGeneric(source)) return !source.array || target.array;
	if (target.array != source.array) return false;
	if (target.array && source.array && target.arrayRank.has_value() && source.arrayRank.has_value() &&
		*target.arrayRank != *source.arrayRank) return false;
	if (!target.array && model != nullptr) {
		const auto targetType = model->types.find(target.name);
		const auto sourceType = model->types.find(source.name);
		const bool targetEnum = targetType != model->types.end() && targetType->second.enumeration;
		const bool sourceEnum = sourceType != model->types.end() && sourceType->second.enumeration;
		if ((targetEnum && IsNumeric(source)) || (sourceEnum && IsNumeric(target))) return true;
	}
	if (IsNumeric(target) && IsNumeric(source)) return true;
	return target.name == source.name;
}

std::string LocalTextToUtf8(const std::string& text)
{
	if (text.empty()) return {};
	const int wideLength = MultiByteToWideChar(CP_ACP, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
	if (wideLength <= 0) return text;
	std::wstring wide(static_cast<std::size_t>(wideLength), L'\0');
	if (MultiByteToWideChar(CP_ACP, 0, text.data(), static_cast<int>(text.size()), wide.data(), wideLength) <= 0) return text;
	return WideToUtf8Text(wide);
}

void AddSemanticError(
	SourcePreflightReport& report,
	const std::string& path,
	const std::size_t line,
	const std::string& code,
	const std::string& message)
{
	const std::string utf8Path = LocalTextToUtf8(path);
	if (std::any_of(
			report.errors.begin(),
			report.errors.end(),
			[&](const SourcePreflightDiagnostic& diagnostic) {
				return diagnostic.filePath == utf8Path && diagnostic.line == line &&
					diagnostic.code == code && diagnostic.message == message;
			})) {
		return;
	}
	report.errors.push_back(SourcePreflightDiagnostic {
		.filePath = utf8Path,
		.line = line,
		.code = code,
		.message = message,
	});
}

std::string StripComment(const std::string& line)
{
	bool text = false;
	for (std::size_t i = 0; i < line.size();) {
		if (!text && line.compare(i, std::string("“").size(), "“") == 0) { text = true; i += std::string("“").size(); continue; }
		if (text && line.compare(i, std::string("”").size(), "”") == 0) { text = false; i += std::string("”").size(); continue; }
		if (!text && line[i] == '\'') return line.substr(0, i);
		const char* begin = line.c_str();
		const char* current = begin + i;
		const char* next = CharNextExA(CP_ACP, current, 0);
		i = next == nullptr || next <= current ? i + 1 : (std::min)(line.size(), static_cast<std::size_t>(next - begin));
	}
	return line;
}

bool ReadUtf8FileAsLocal(const std::filesystem::path& path, std::string& outText)
{
	std::ifstream input(path, std::ios::binary);
	if (!input) return false;
	std::string utf8((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
	if (utf8.size() >= 3 && static_cast<unsigned char>(utf8[0]) == 0xEF && static_cast<unsigned char>(utf8[1]) == 0xBB && static_cast<unsigned char>(utf8[2]) == 0xBF) utf8.erase(0, 3);
	if (utf8.empty()) { outText.clear(); return true; }
	const int wideLength = MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), nullptr, 0);
	if (wideLength <= 0) return false;
	std::wstring wide(static_cast<std::size_t>(wideLength), L'\0');
	if (MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), wide.data(), wideLength) <= 0) return false;
	const int localLength = WideCharToMultiByte(CP_ACP, 0, wide.data(), wideLength, nullptr, 0, "?", nullptr);
	if (localLength <= 0) return false;
	outText.resize(static_cast<std::size_t>(localLength));
	WideCharToMultiByte(CP_ACP, 0, wide.data(), wideLength, outText.data(), localLength, "?", nullptr);
	return true;
}

bool IsArrayField(const ParsedDeclaration& declaration)
{
	if (!Field(declaration, 3).empty()) return true;
	return Contains(Field(declaration, 2), "数组");
}

bool IsParameterArrayField(const ParsedDeclaration& declaration)
{
	return Contains(Field(declaration, 2), "数组");
}

bool IsOptionalField(const ParsedDeclaration& declaration)
{
	return Contains(Field(declaration, 2), "可空") || Contains(Field(declaration, 2), "默认空") || Contains(Field(declaration, 3), "可空");
}

bool IsByReferenceField(const ParsedDeclaration& declaration)
{
	return Contains(Field(declaration, 2), "只接收变量");
}

bool HasElibAttribute(const std::string& attributes, const std::string_view expected)
{
	std::size_t begin = 0;
	for (;;) {
		const std::size_t separator = attributes.find('|', begin);
		if (Trim(attributes.substr(begin, separator == std::string::npos ? std::string::npos : separator - begin)) == expected) {
			return true;
		}
		if (separator == std::string::npos) return false;
		begin = separator + 1;
	}
}

bool ElibParameterRequiresArray(const std::string& attributes)
{
	return HasElibAttribute(attributes, "数组") ||
		HasElibAttribute(attributes, "接收数组数据") ||
		HasElibAttribute(attributes, "只接收变量数组");
}

bool ElibParameterRequiresVariable(const std::string& attributes)
{
	return HasElibAttribute(attributes, "只接收变量") ||
		HasElibAttribute(attributes, "只接收变量数组") ||
		HasElibAttribute(attributes, "接收变量或数组");
}

std::string DirectiveToken(const std::string& line)
{
	if (line.empty() || line.front() != '.') return {};
	std::size_t end = 1;
	while (end < line.size() && line[end] != ' ' && line[end] != '\t' && line[end] != '(') ++end;
	return line.substr(0, end);
}

std::string CallNameFromNode(const SourceExpressionNode& node)
{
	if (node.kind == SourceExpressionKind::Name) return node.text;
	if (node.kind == SourceExpressionKind::Member) return node.text;
	return {};
}

void CollectGlobalPage(const std::string& text, SemanticModel& model, SourcePreflightReport& report)
{
	for (const std::string& original : SplitLines(text)) {
		const std::string line = Trim(StripComment(original));
		std::string rest;
		if (!MatchDirective(line, "全局变量", &rest)) continue;
		const ParsedDeclaration declaration = SplitFields(rest);
		const std::string name = Field(declaration, 0);
		if (name.empty()) continue;
		const TypeInfo type = ParseDeclaredType(Field(declaration, 1), IsArrayField(declaration), ParseArrayRank(declaration));
		model.globals.emplace(name, Symbol { .type = type });
	}
	(void)report;
}

void CollectStructPage(const std::string& text, SemanticModel& model)
{
	TypeSymbol* current = nullptr;
	for (const std::string& original : SplitLines(text)) {
		const std::string line = Trim(StripComment(original));
		std::string rest;
		if (MatchDirective(line, "数据类型", &rest)) {
			const ParsedDeclaration declaration = SplitFields(rest);
			const std::string name = Field(declaration, 0);
			if (name.empty()) { current = nullptr; continue; }
			TypeSymbol& type = model.types[name];
			type.type = TypeInfo { .name = name };
			current = &type;
			continue;
		}
		if (current != nullptr && MatchDirective(line, "成员", &rest)) {
			const ParsedDeclaration declaration = SplitFields(rest);
			const std::string name = Field(declaration, 0);
			if (name.empty()) continue;
			const TypeInfo type = ParseDeclaredType(Field(declaration, 1), IsArrayField(declaration), ParseArrayRank(declaration));
			current->members.emplace(name, Symbol { .type = type });
		}
	}
}

void CollectDllPage(const std::string& text, SemanticModel& model)
{
	Callable* current = nullptr;
	for (const std::string& original : SplitLines(text)) {
		const std::string line = Trim(StripComment(original));
		std::string rest;
		if (MatchDirective(line, "DLL命令", &rest)) {
			const ParsedDeclaration declaration = SplitFields(rest);
			const std::string name = Field(declaration, 0);
			if (name.empty()) { current = nullptr; continue; }
			Callable callable;
			callable.name = name;
			callable.returnType = ParseType(Field(declaration, 1));
			model.functions[name].push_back(std::move(callable));
			current = &model.functions[name].back();
			continue;
		}
		if (current != nullptr && MatchDirective(line, "参数", &rest)) {
			const ParsedDeclaration declaration = SplitFields(rest);
			Parameter parameter;
			parameter.name = Field(declaration, 0);
			parameter.type = ParseDeclaredType(Field(declaration, 1), IsParameterArrayField(declaration));
			// DLL 的“传址”描述 ABI 传参方式，易语言允许常量、空指针和
			// 可计算表达式作为实参，不能据此施加项目子程序式的左值限制。
			current->parameters.push_back(std::move(parameter));
		}
	}
}

TypeInfo InferConstantType(const std::string& value)
{
	std::string text = Trim(value);
	if (text.empty()) return {};
	if (text.size() >= 2 && text.front() == '"' && text.back() == '"') text = text.substr(1, text.size() - 2);
	if (StartsWith(text, "“") && text.size() >= std::string("“”").size() && text.compare(text.size() - std::string("”").size(), std::string("”").size(), "”") == 0) return TypeInfo { .name = "文本型" };
	if (text == "真" || text == "假") return TypeInfo { .name = "逻辑型" };
	if (text.front() == '{') return TypeInfo { .name = "字节集" };
	char* end = nullptr;
	(void)std::strtod(text.c_str(), &end);
	if (end != text.c_str() && end != nullptr && *end == '\0') return TypeInfo { .name = "整数型" };
	return {};
}

TypeInfo ParsePublishedConstantType(const std::string& publishedType)
{
	if (publishedType == "文本") return TypeInfo { .name = "文本型" };
	if (publishedType == "逻辑") return TypeInfo { .name = "逻辑型" };
	if (publishedType == "日期时间") return TypeInfo { .name = "日期时间型" };
	if (publishedType == "字节集" || publishedType == "图片" || publishedType == "声音") return TypeInfo { .name = "字节集" };
	if (publishedType == "数值") return TypeInfo { .name = "整数型" };
	return ParseType(publishedType);
}

void CollectConstantPage(const std::string& text, SemanticModel& model)
{
	for (const std::string& original : SplitLines(text)) {
		const std::string line = Trim(StripComment(original));
		std::string rest;
		if (!MatchDirective(line, "常量", &rest)) continue;
		const ParsedDeclaration declaration = SplitFields(rest);
		const std::string name = Field(declaration, 0);
		if (!name.empty()) model.constants["#" + name] = InferConstantType(Field(declaration, 1));
	}
}

void CollectProgramSource(const BundleSourceFile& file, SemanticModel& model)
{
	ProgramSource program;
	program.path = file.relativePath.empty() ? file.logicalName : file.relativePath;
	program.lines = SplitLines(file.content);
	MethodSymbol* current = nullptr;
	for (std::size_t index = 0; index < program.lines.size(); ++index) {
		const std::size_t lineNumber = index + 1;
		const std::string line = Trim(StripComment(program.lines[index]));
		std::string rest;
		if (MatchDirective(line, "程序集", &rest)) {
			const ParsedDeclaration declaration = SplitFields(rest);
			program.assemblyName = Field(declaration, 0);
			continue;
		}
		if (MatchDirective(line, "程序集变量", &rest)) {
			const ParsedDeclaration declaration = SplitFields(rest);
			const std::string name = Field(declaration, 0);
			if (!name.empty()) {
				program.classSymbols.emplace(name, Symbol { .type = ParseDeclaredType(Field(declaration, 1), IsArrayField(declaration), ParseArrayRank(declaration)) });
			}
			continue;
		}
		if (MatchDirective(line, "子程序", &rest)) {
			if (current != nullptr) current->lastBodyLine = index;
			program.methods.push_back(MethodSymbol {});
			current = &program.methods.back();
			const ParsedDeclaration declaration = SplitFields(rest);
			current->headerLine = lineNumber;
			current->firstBodyLine = lineNumber + 1;
			current->callable.name = Field(declaration, 0);
			current->callable.returnType = ParseType(Field(declaration, 1));
			continue;
		}
		if (current == nullptr) continue;
		if (MatchDirective(line, "参数", &rest)) {
			const ParsedDeclaration declaration = SplitFields(rest);
			Parameter parameter;
			parameter.name = Field(declaration, 0);
			parameter.type = ParseDeclaredType(Field(declaration, 1), IsParameterArrayField(declaration));
			parameter.optional = IsOptionalField(declaration);
			parameter.byReference = IsByReferenceField(declaration);
			current->callable.parameters.push_back(parameter);
			if (!parameter.name.empty()) current->symbols.emplace(parameter.name, Symbol { .type = parameter.type });
			continue;
		}
		if (MatchDirective(line, "局部变量", &rest)) {
			const ParsedDeclaration declaration = SplitFields(rest);
			const std::string name = Field(declaration, 0);
			if (!name.empty()) {
				const TypeInfo type = ParseDeclaredType(Field(declaration, 1), IsArrayField(declaration), ParseArrayRank(declaration));
				current->symbols.emplace(name, Symbol { .type = type });
			}
			continue;
		}
	}
	if (current != nullptr) current->lastBodyLine = program.lines.size();
	if (!program.assemblyName.empty()) {
		TypeSymbol& classType = model.types[program.assemblyName];
		classType.type = TypeInfo { .name = program.assemblyName };
		for (const auto& [name, symbol] : program.classSymbols) classType.members.emplace(name, symbol);
		for (const MethodSymbol& method : program.methods) {
			if (!method.callable.name.empty()) classType.methods[method.callable.name].push_back(method.callable);
		}
	}
	for (MethodSymbol& method : program.methods) {
		if (method.callable.name.empty()) continue;
		method.callable.metadataKnown = true;
		model.functions[method.callable.name].push_back(method.callable);
	}
	model.programs.push_back(std::move(program));
}

std::string XmlAttribute(const SimpleXmlNode& node, const std::string& name)
{
	if (const auto it = node.attributes.find(name); it != node.attributes.end()) return it->second;
	return {};
}

bool IsFormMetadataNode(const std::string& name)
{
	return name.find('.') != std::string::npos;
}

void CollectFormControlSymbols(
	const SimpleXmlNode& node,
	ProgramSource& program,
	const std::string& path,
	SourcePreflightReport& report)
{
	for (const SimpleXmlNode& child : node.children) {
		if (!IsFormMetadataNode(child.name)) {
			const std::string name = XmlAttribute(child, "名称");
			if (!name.empty()) {
				const auto [it, inserted] = program.classSymbols.emplace(name, Symbol { .type = TypeInfo { .name = child.name } });
				if (!inserted) {
					AddSemanticError(report, path, 1, "form_symbol_duplicate", "form controls and assembly variables must not use the same name");
				}
			}
		}
		CollectFormControlSymbols(child, program, path, report);
	}
}

void ValidateFormEventHandlers(
	const SimpleXmlNode& node,
	const ProgramSource& program,
	const std::string& path,
	SourcePreflightReport& report)
{
	for (const SimpleXmlNode& child : node.children) {
		if (child.name.size() >= std::string(".事件").size() &&
			child.name.compare(child.name.size() - std::string(".事件").size(), std::string(".事件").size(), ".事件") == 0) {
			const std::string handler = XmlAttribute(child, "处理器");
			if (!handler.empty()) {
				const std::size_t separator = handler.rfind("::");
				const std::string methodName = Trim(separator == std::string::npos ? handler : handler.substr(separator + 2));
				const bool found = std::any_of(
					program.methods.begin(),
					program.methods.end(),
					[&](const MethodSymbol& method) { return method.callable.name == methodName; });
				if (!found) AddSemanticError(report, path, 1, "form_event_handler_not_found", "form event handler is not declared in the bound window assembly");
			}
		}
		ValidateFormEventHandlers(child, program, path, report);
	}
}

void CollectFormSymbols(
	const ProjectBundle& bundle,
	SemanticModel& model,
	SourcePreflightReport& report)
{
	// 即使没有窗口程序集绑定，所有 XML 仍必须能被回包阶段解析。
	for (const BundleFormFile& form : bundle.formFiles) {
		SimpleXmlNode root;
		SimpleXmlParseError parseError;
		const std::string path = form.relativePath.empty() ? form.logicalName : form.relativePath;
		if (!ParseSimpleXmlDocument(form.xmlText, root, &parseError)) {
			AddSemanticError(report, path, parseError.lineIndex + 1, parseError.code, "form XML cannot be parsed");
		}
		else if (root.name != "窗口") {
			AddSemanticError(report, path, 1, "form_root_invalid", "form XML root element must be 窗口");
		}
	}
	for (ProgramSource& program : model.programs) {
		const auto bindingIt = std::find_if(
			bundle.windowBindings.begin(),
			bundle.windowBindings.end(),
			[&](const WindowBinding& binding) { return binding.className == program.assemblyName; });
		if (bindingIt == bundle.windowBindings.end()) continue;
		program.hasFormBinding = true;

		const auto formIt = std::find_if(
			bundle.formFiles.begin(),
			bundle.formFiles.end(),
			[&](const BundleFormFile& form) { return form.logicalName == bindingIt->formName; });
		if (formIt == bundle.formFiles.end()) {
			AddSemanticError(report, program.path, 1, "window_form_not_found", "window assembly binding refers to a form XML file that is not present");
			continue;
		}

		SimpleXmlNode root;
		SimpleXmlParseError parseError;
		const std::string path = formIt->relativePath.empty() ? formIt->logicalName : formIt->relativePath;
		if (!ParseSimpleXmlDocument(formIt->xmlText, root, &parseError)) {
			AddSemanticError(report, path, parseError.lineIndex + 1, parseError.code, "form XML cannot be parsed");
			continue;
		}
		if (root.name != "窗口") {
			AddSemanticError(report, path, 1, "form_root_invalid", "form XML root element must be 窗口");
			continue;
		}
		const std::string formName = XmlAttribute(root, "名称");
		if (!formName.empty() && formName != bindingIt->formName) {
			AddSemanticError(report, path, 1, "form_name_binding_mismatch", "form XML 名称 does not match its window binding");
			continue;
		}

		program.formBaseType = root.name;
		CollectFormControlSymbols(root, program, path, report);
		ValidateFormEventHandlers(root, program, path, report);
		program.formSymbolsComplete = true;
		TypeSymbol& classType = model.types[program.assemblyName];
		for (const auto& [name, symbol] : program.classSymbols) classType.members.emplace(name, symbol);
	}
}

std::string GetValueField(const ParsedDeclaration& declaration, const std::string_view key)
{
	for (const std::string& field : declaration.fields) {
		const std::string prefix = std::string(key) + "=";
		if (StartsWith(field, prefix)) return field.substr(prefix.size());
	}
	return {};
}

void ParseElibText(const std::string& text, SemanticModel& model)
{
	Callable* current = nullptr;
	std::string currentType;
	for (const std::string& original : SplitLines(text)) {
		const std::string line = Trim(StripComment(original));
		std::string rest;
		if (MatchDirective(line, "数据类型", &rest)) {
			const ParsedDeclaration declaration = SplitFields(rest);
			const std::string name = Field(declaration, 0);
			if (!name.empty()) {
				currentType = name;
				TypeSymbol& type = model.types[name];
				type.type = TypeInfo { .name = name };
				type.enumeration = GetValueField(declaration, "类型") == "枚举";
			}
			continue;
		}
		if (!currentType.empty() && MatchDirective(line, "成员", &rest)) {
			const ParsedDeclaration declaration = SplitFields(rest);
			const std::string name = Field(declaration, 0);
			if (!name.empty()) {
				model.types[currentType].members[name] = Symbol {
					.type = model.types[currentType].enumeration ? TypeInfo { .name = currentType } : ParseDeclaredType(Field(declaration, 1)),
					.lvalue = !Contains(GetValueField(declaration, "属性"), "只读"),
				};
			}
			continue;
		}
		if (MatchDirective(line, "常量", &rest)) {
			const ParsedDeclaration declaration = SplitFields(rest);
			const std::string name = Field(declaration, 0);
			if (!name.empty()) model.constants["#" + name] = ParsePublishedConstantType(Field(declaration, 1));
			continue;
		}
		if (MatchDirective(line, "命令", &rest) || MatchDirective(line, "成员命令", &rest)) {
			const ParsedDeclaration declaration = SplitFields(rest);
			const std::string name = Field(declaration, 0);
			const bool explicitMemberCommand = StartsWith(line, ".成员命令");
			const bool sharedMemberCommand = !explicitMemberCommand && Contains(GetValueField(declaration, "分类"), "成员命令");
			if (name == "<未命名>" || name.empty()) { current = nullptr; continue; }
			Callable callable;
			callable.name = name;
			callable.ownerType = explicitMemberCommand ? currentType : std::string();
			callable.returnType = ParseType(GetValueField(declaration, "返回值"));
			callable.variadic = Contains(GetValueField(declaration, "属性"), "允许追加参数");
			if (sharedMemberCommand) {
				model.memberFunctions[name].push_back(std::move(callable));
				current = &model.memberFunctions[name].back();
			}
			else if (callable.ownerType.empty()) {
				model.functions[name].push_back(std::move(callable));
				current = &model.functions[name].back();
			}
			else {
				model.types[callable.ownerType].methods[name].push_back(std::move(callable));
				current = &model.types[currentType].methods[name].back();
			}
			model.anyExternalMetadata = true;
			continue;
		}
		if (current != nullptr && MatchDirective(line, "参数", &rest)) {
			const ParsedDeclaration declaration = SplitFields(rest);
			const std::string attributes = GetValueField(declaration, "属性");
			Parameter parameter;
			parameter.name = Field(declaration, 0);
			parameter.type = ParseDeclaredType(Field(declaration, 1), ElibParameterRequiresArray(attributes));
			parameter.optional = Contains(attributes, "默认空") || Contains(attributes, "可空") || !GetValueField(declaration, "默认值").empty();
			parameter.byReference = ElibParameterRequiresVariable(attributes);
			parameter.variableOnly = ElibParameterRequiresVariable(attributes);
			current->parameters.push_back(std::move(parameter));
			continue;
		}
	}
}

void ParseEcomHeader(const std::string& text, SemanticModel& model)
{
	std::string currentType;
	Callable* current = nullptr;
	for (const std::string& original : SplitLines(text)) {
		const std::string line = Trim(StripComment(original));
		std::string rest;
		if (MatchDirective(line, "程序集", &rest)) {
			const ParsedDeclaration declaration = SplitFields(rest);
			currentType = Field(declaration, 0);
			if (!currentType.empty()) model.types[currentType].type = TypeInfo { .name = currentType };
			current = nullptr;
			continue;
		}
		if (MatchDirective(line, "常量", &rest)) {
			const ParsedDeclaration declaration = SplitFields(rest);
			const std::string name = Field(declaration, 0);
			if (!name.empty()) model.constants["#" + name] = InferConstantType(Field(declaration, 1));
			continue;
		}
		if (MatchDirective(line, "子程序", &rest)) {
			const ParsedDeclaration declaration = SplitFields(rest);
			const std::string name = Field(declaration, 0);
			if (name.empty()) { current = nullptr; continue; }
			Callable callable;
			callable.name = name;
			callable.ownerType = currentType;
			callable.returnType = ParseType(Field(declaration, 1));
			if (currentType.empty()) {
				model.functions[name].push_back(std::move(callable));
				current = &model.functions[name].back();
			}
			else {
				model.types[currentType].methods[name].push_back(std::move(callable));
				current = &model.types[currentType].methods[name].back();
			}
			model.anyExternalMetadata = true;
			continue;
		}
		if (!currentType.empty() && MatchDirective(line, "成员", &rest)) {
			const ParsedDeclaration declaration = SplitFields(rest);
			const std::string name = Field(declaration, 0);
			if (!name.empty()) {
				model.types[currentType].members[name] = Symbol {
					.type = ParseDeclaredType(Field(declaration, 1)),
					.lvalue = !Contains(GetValueField(declaration, "属性"), "只读"),
				};
			}
			continue;
		}
		if (current != nullptr && MatchDirective(line, "参数", &rest)) {
			const ParsedDeclaration declaration = SplitFields(rest);
			Parameter parameter;
			parameter.name = Field(declaration, 0);
			parameter.type = ParseDeclaredType(Field(declaration, 1), IsParameterArrayField(declaration));
			parameter.optional = IsOptionalField(declaration);
			parameter.byReference = IsByReferenceField(declaration);
			current->parameters.push_back(std::move(parameter));
		}
	}
}

void AddBuiltinCallables(SemanticModel& model)
{
	const auto add = [&](const char* name, const char* returnType, const char* parameterType, bool optional = false) {
		Callable callable;
		callable.name = name;
		callable.returnType = ParseType(returnType);
		if (parameterType != nullptr) callable.parameters.push_back(Parameter { .type = ParseType(parameterType), .optional = optional });
		model.functions[name].push_back(std::move(callable));
	};
	add("到文本", "文本型", "通用型");
	add("到整数", "整数型", "通用型");
	add("到数值", "小数型", "通用型");
	add("到小数", "小数型", "通用型");
	add("到字节", "字节型", "通用型");
	add("到字节集", "字节集", "通用型");
	add("到时间", "日期时间型", "文本型");
	add("取反", "逻辑型", "逻辑型");
}

void CollectDependencies(const ProjectBundle& bundle, SemanticModel& model)
{
	for (const Dependency& dependency : bundle.dependencies) {
		if (dependency.localWorkspace.empty()) {
			model.externalMetadataComplete = false;
			continue;
		}
		const std::filesystem::path workspace = Utf8PathToPath(dependency.localWorkspace);
		std::error_code ec;
		if (dependency.kind == DependencyKind::ELib) {
			std::string text;
			if (!std::filesystem::is_regular_file(workspace, ec) || !ReadUtf8FileAsLocal(workspace, text)) {
				model.externalMetadataComplete = false;
				continue;
			}
			ParseElibText(text, model);
		}
		else {
			std::filesystem::path header = workspace / "header" / "header.txt";
			std::string text;
			if (!std::filesystem::is_regular_file(header, ec) || !ReadUtf8FileAsLocal(header, text)) {
				model.externalMetadataComplete = false;
				continue;
			}
			ParseEcomHeader(text, model);
		}
	}
}

const Symbol* FindSymbol(const std::string& name, const EvaluationContext& context)
{
	if (const auto it = context.method.symbols.find(name); it != context.method.symbols.end()) return &it->second;
	if (const auto it = context.program.classSymbols.find(name); it != context.program.classSymbols.end()) return &it->second;
	if (const auto it = context.model.globals.find(name); it != context.model.globals.end()) return &it->second;
	return nullptr;
}

const MethodSymbol* FindCurrentMethod(const std::string& name, const SemanticModel& model, const ProgramSource& program)
{
	for (const MethodSymbol& method : program.methods) if (method.callable.name == name) return &method;
	(void)model;
	return nullptr;
}

TypeCategory EffectiveCategory(const TypeInfo& type) { return Category(type); }

EvaluatedExpression EvaluateExpression(
	const SourceExpressionNode& node,
	const EvaluationContext& context,
	SourcePreflightReport& report);

std::vector<const Callable*> FindCallableCandidates(
	const std::string& name,
	const EvaluationContext& context,
	const std::string& ownerType)
{
	std::vector<const Callable*> result;
	if (!ownerType.empty()) {
		if (const auto typeIt = context.model.types.find(ownerType); typeIt != context.model.types.end()) {
			if (const auto methodIt = typeIt->second.methods.find(name); methodIt != typeIt->second.methods.end()) {
				for (const Callable& callable : methodIt->second) result.push_back(&callable);
			}
		}
		if (const auto sharedIt = context.model.memberFunctions.find(name); sharedIt != context.model.memberFunctions.end()) {
			for (const Callable& callable : sharedIt->second) result.push_back(&callable);
		}
		return result;
	}
	if (const auto it = context.model.functions.find(name); it != context.model.functions.end()) {
		for (const Callable& callable : it->second) result.push_back(&callable);
	}
	return result;
}

bool IsMissing(const SourceExpressionNode& node) { return node.kind == SourceExpressionKind::Missing; }

bool IsLvalueNode(const SourceExpressionNode& node)
{
	return node.kind == SourceExpressionKind::Name || node.kind == SourceExpressionKind::Index || node.kind == SourceExpressionKind::Member;
}

std::optional<std::int64_t> EvaluateStaticInteger(const SourceExpressionNode& node)
{
	if (node.kind == SourceExpressionKind::Group && node.children.size() == 1) {
		return EvaluateStaticInteger(*node.children.front());
	}
	if (node.kind == SourceExpressionKind::Unary && node.children.size() == 1 &&
		(node.text == "+" || node.text == "＋" || node.text == "-" || node.text == "－")) {
		const auto value = EvaluateStaticInteger(*node.children.front());
		if (!value.has_value()) return std::nullopt;
		if (node.text == "+" || node.text == "＋") return value;
		if (*value == (std::numeric_limits<std::int64_t>::min)()) return std::nullopt;
		return -*value;
	}
	if (node.kind != SourceExpressionKind::NumberLiteral || node.text.empty() || node.text.find('.') != std::string::npos) {
		return std::nullopt;
	}
	std::int64_t value = 0;
	const char* begin = node.text.data();
	const char* end = begin + node.text.size();
	const auto result = std::from_chars(begin, end, value);
	if (result.ec != std::errc() || result.ptr != end) return std::nullopt;
	return value;
}

bool IsStaticDecimalLiteral(const SourceExpressionNode& node)
{
	if (node.kind == SourceExpressionKind::Group && node.children.size() == 1) {
		return IsStaticDecimalLiteral(*node.children.front());
	}
	if (node.kind == SourceExpressionKind::Unary && node.children.size() == 1 &&
		(node.text == "+" || node.text == "＋" || node.text == "-" || node.text == "－")) {
		return IsStaticDecimalLiteral(*node.children.front());
	}
	return node.kind == SourceExpressionKind::NumberLiteral && node.text.find('.') != std::string::npos;
}

bool MatchCallable(
	const Callable& callable,
	const SourceExpressionNode& call,
	const EvaluationContext& context,
	SourcePreflightReport& report,
	bool& outUnknown)
{
	outUnknown = false;
	const std::size_t actualCount = call.children.size() - 1;
	if (!callable.variadic && actualCount > callable.parameters.size()) return false;
	for (std::size_t index = 0; index < callable.parameters.size(); ++index) {
		const Parameter& parameter = callable.parameters[index];
		if (index >= actualCount) {
			if (!parameter.optional) return false;
			continue;
		}
		const SourceExpressionNode& argument = *call.children[index + 1];
		if (IsMissing(argument)) {
			if (!parameter.optional && !callable.variadic) return false;
			continue;
		}
		const EvaluatedExpression value = EvaluateExpression(argument, context, report);
		if (value.state == ResolveState::Invalid) return false;
		if (value.state == ResolveState::Unknown) outUnknown = true;
		if (value.state == ResolveState::Valid && !Compatible(parameter.type, value.type, &context.model)) {
			// 易语言命令参数允许把可转换的标量自动转为文本；严格类型错误仍由赋值和逻辑/数组参数检查拦截。
			const bool implicitTextConversion = Category(parameter.type) == TypeCategory::Text && !value.type.array && Category(value.type) != TypeCategory::Void;
			if (!implicitTextConversion) return false;
		}
		if (parameter.type.array && value.state == ResolveState::Valid && !value.type.array) return false;
		if ((parameter.byReference || parameter.variableOnly) && value.state == ResolveState::Valid && !value.lvalue) return false;
	}
	for (std::size_t index = callable.parameters.size(); index < actualCount; ++index) {
		const EvaluatedExpression value = EvaluateExpression(*call.children[index + 1], context, report);
		if (value.state == ResolveState::Invalid) return false;
		if (value.state == ResolveState::Unknown) outUnknown = true;
	}
	return true;
}

EvaluatedExpression EvaluateCall(
	const SourceExpressionNode& node,
	const EvaluationContext& context,
	SourcePreflightReport& report)
{
	if (node.children.empty()) return { ResolveState::Invalid, {}, false, {} };
	const SourceExpressionNode& callee = *node.children.front();
	std::string name;
	std::string ownerType;
	if (callee.kind == SourceExpressionKind::Name) {
		name = callee.text;
	}
	else if (callee.kind == SourceExpressionKind::Member && !callee.children.empty()) {
		name = callee.text;
		// 类名.静态方法() 的左侧是类型名而不是运行时对象。
		// 先按类型名查找，避免把类名当作未声明变量。
		if (callee.children.front()->kind == SourceExpressionKind::Name &&
			context.model.types.contains(callee.children.front()->text)) {
			ownerType = callee.children.front()->text;
		}
		else {
			const EvaluatedExpression receiver = EvaluateExpression(*callee.children.front(), context, report);
			if (receiver.state == ResolveState::Invalid) return receiver;
			if (receiver.state == ResolveState::Unknown) return { ResolveState::Unknown, {}, false, name };
			ownerType = receiver.type.name;
		}
	}
	else {
		return { ResolveState::Invalid, {}, false, {} };
	}
	const auto candidates = FindCallableCandidates(name, context, ownerType);
	if (candidates.empty()) {
		if (ownerType.empty() && FindSymbol(name, context) != nullptr) {
			AddSemanticError(report, context.path, context.line, "call_target_not_callable", "a variable cannot be called as a subprogram");
			return { ResolveState::Invalid, {}, false, name };
		}
		if (!ownerType.empty() && context.model.types.contains(ownerType)) {
			AddSemanticError(report, context.path, context.line, "member_not_found", "the object type has no member command with this name");
			return { ResolveState::Invalid, {}, false, name };
		}
		if (ownerType.empty() && context.model.externalMetadataComplete) {
			AddSemanticError(report, context.path, context.line, "call_not_found", "subprogram or command is not declared in the project or dependency metadata");
			return { ResolveState::Invalid, {}, false, name };
		}
		return { ResolveState::Unknown, {}, false, name };
	}
	bool anyUnknown = false;
	const Callable* matched = nullptr;
	std::size_t matchedDistance = (std::numeric_limits<std::size_t>::max)();
	for (const Callable* candidate : candidates) {
		bool unknown = false;
		if (MatchCallable(*candidate, node, context, report, unknown)) {
			const std::size_t actualCount = node.children.size() - 1;
			const std::size_t distance = candidate->variadic
				? 0
				: candidate->parameters.size() >= actualCount
				? candidate->parameters.size() - actualCount
				: actualCount - candidate->parameters.size();
			if (matched == nullptr || distance < matchedDistance) {
				matched = candidate;
				matchedDistance = distance;
				anyUnknown = unknown;
			}
		}
	}
	if (matched == nullptr) {
		// A module may intentionally omit support-library headers. In that case
		// a known local overload is not enough to prove that the call is invalid;
		// leave the result unknown until authoritative metadata is available.
		if (!context.model.externalMetadataComplete) {
			return { ResolveState::Unknown, {}, false, name };
		}
		std::ostringstream detail;
		detail << "subprogram call has " << (node.children.size() - 1)
			<< " argument(s), but none of " << candidates.size()
			<< " known signature(s) accepts their count, type, array shape, and reference attributes";
		AddSemanticError(report, context.path, context.line, "call_signature_mismatch", detail.str());
		return { ResolveState::Invalid, {}, false, name };
	}
	const bool unknownResult = matched->returnType.name.empty() || matched->returnType.name == "通用型" || matched->returnType.name == "子语句";
	return { anyUnknown || unknownResult ? ResolveState::Unknown : ResolveState::Valid, matched->returnType, false, name };
}

EvaluatedExpression EvaluateExpression(
	const SourceExpressionNode& node,
	const EvaluationContext& context,
	SourcePreflightReport& report)
{
	switch (node.kind) {
	case SourceExpressionKind::Missing:
		return { ResolveState::Valid, {}, false, {} };
	case SourceExpressionKind::NumberLiteral:
		return { ResolveState::Valid, TypeInfo { .name = "整数型" }, false, {} };
	case SourceExpressionKind::TextLiteral:
		return { ResolveState::Valid, TypeInfo { .name = "文本型" }, false, {} };
	case SourceExpressionKind::LogicalLiteral:
		return { ResolveState::Valid, TypeInfo { .name = "逻辑型" }, false, {} };
	case SourceExpressionKind::DateTimeLiteral:
		return { ResolveState::Valid, TypeInfo { .name = "日期时间型" }, false, {} };
	case SourceExpressionKind::ByteSetLiteral: {
		ResolveState state = ResolveState::Valid;
		TypeInfo elementType { .name = "通用型" };
		bool hasKnownElement = false;
		bool byteSetLiteral = true;
		for (const auto& child : node.children) {
			const EvaluatedExpression item = EvaluateExpression(*child, context, report);
			if (item.state == ResolveState::Invalid) return item;
			if (item.state == ResolveState::Unknown) {
				state = ResolveState::Unknown;
				continue;
			}
			if (item.type.array) {
				AddSemanticError(report, context.path, context.line, "array_literal_nested", "array literal items cannot themselves be arrays");
				return { ResolveState::Invalid, {}, false, {} };
			}
			if (!IsNumeric(item.type)) {
				byteSetLiteral = false;
			}
			if (!hasKnownElement) {
				elementType = item.type;
				hasKnownElement = true;
			}
			else if (!Compatible(elementType, item.type, &context.model) && !Compatible(item.type, elementType, &context.model)) {
				state = ResolveState::Unknown;
			}
		}
		if (byteSetLiteral) {
			return { state, TypeInfo { .name = "字节集" }, false, {} };
		}
		if (!hasKnownElement) {
			return { ResolveState::Unknown, TypeInfo { .name = "通用型", .array = true, .arrayRank = 1 }, false, {} };
		}
		elementType.array = true;
		elementType.arrayRank = 1;
		return { state, elementType, false, {} };
	}
	case SourceExpressionKind::Name: {
		if (node.text == "真" || node.text == "假") return { ResolveState::Valid, TypeInfo { .name = "逻辑型" }, false, node.text };
		if (!node.text.empty() && node.text.front() == '#') {
			if (const auto it = context.model.constants.find(node.text); it != context.model.constants.end() && !it->second.name.empty()) return { ResolveState::Valid, it->second, false, node.text };
			if (context.model.resources.contains(node.text.substr(1))) return { ResolveState::Valid, TypeInfo { .name = "字节集" }, false, node.text };
			if (context.model.externalMetadataComplete) {
				AddSemanticError(report, context.path, context.line, "constant_not_found", "constant or resource is not declared in the project or dependency metadata");
				return { ResolveState::Invalid, {}, false, node.text };
			}
			return { ResolveState::Unknown, {}, false, node.text };
		}
		if (const Symbol* symbol = FindSymbol(node.text, context)) return { ResolveState::Valid, symbol->type, symbol->lvalue, node.text };
		if (context.program.hasFormBinding) {
			if (!context.program.formBaseType.empty()) {
				if (const auto typeIt = context.model.types.find(context.program.formBaseType); typeIt != context.model.types.end()) {
					if (const auto memberIt = typeIt->second.members.find(node.text); memberIt != typeIt->second.members.end()) {
						return { ResolveState::Valid, memberIt->second.type, memberIt->second.lvalue, node.text };
					}
				}
			}
			// 窗口自身属性依赖窗口支持库。只有控件 XML 和全部依赖元数据都完整时，未知名称才可确定为错误。
			if (!context.program.formSymbolsComplete || !context.model.externalMetadataComplete) {
				return { ResolveState::Unknown, {}, true, node.text };
			}
		}
		AddSemanticError(report, context.path, context.line, "symbol_not_found", "variable or constant is not declared in the project scope");
		return { ResolveState::Invalid, {}, false, node.text };
	}
	case SourceExpressionKind::Group:
		return node.children.empty() ? EvaluatedExpression { ResolveState::Invalid, {}, false, {} } : EvaluateExpression(*node.children.front(), context, report);
	case SourceExpressionKind::Call:
		return EvaluateCall(node, context, report);
	case SourceExpressionKind::AddressOf: {
		if (node.children.size() != 1 || node.children.front()->kind != SourceExpressionKind::Name) {
			AddSemanticError(report, context.path, context.line, "address_of_target_invalid", "address-of requires a subprogram name");
			return { ResolveState::Invalid, {}, false, {} };
		}
		const std::string& name = node.children.front()->text;
		if (FindCallableCandidates(name, context, {}).empty()) {
			if (context.model.externalMetadataComplete) {
				AddSemanticError(report, context.path, context.line, "address_of_target_not_found", "address-of target subprogram is not declared");
				return { ResolveState::Invalid, {}, false, name };
			}
			return { ResolveState::Unknown, TypeInfo { .name = "子程序指针" }, false, name };
		}
		return { ResolveState::Valid, TypeInfo { .name = "子程序指针" }, false, name };
	}
	case SourceExpressionKind::Index: {
		if (node.children.empty()) return { ResolveState::Invalid, {}, false, {} };
		EvaluatedExpression base = EvaluateExpression(*node.children.front(), context, report);
		if (base.state == ResolveState::Invalid) return base;
		// E-language permits chained byte-set/array indexing in generated code.
		// Once one index has already been applied, the element type is not
		// reliable enough for a second index to be rejected as a scalar access.
		if (base.indexedResult && base.state == ResolveState::Valid) {
			for (std::size_t i = 1; i < node.children.size(); ++i) {
				const EvaluatedExpression index = EvaluateExpression(*node.children[i], context, report);
				if (index.state == ResolveState::Invalid) return index;
			}
			return { ResolveState::Unknown, {}, base.lvalue, base.name, true };
		}
		ResolveState state = base.state;
		const bool byteSetIndex = base.state == ResolveState::Valid && !base.type.array && Category(base.type) == TypeCategory::ByteSet;
		if (base.state == ResolveState::Valid && !base.type.array && !byteSetIndex) {
			if (context.model.types.contains(base.type.name)) {
				return { ResolveState::Unknown, {}, base.lvalue, base.name };
			}
			if (Category(base.type) == TypeCategory::Object || Category(base.type) == TypeCategory::Unknown) {
				return { ResolveState::Unknown, {}, base.lvalue, base.name };
			}
			AddSemanticError(report, context.path, context.line, "index_target_not_array", "only an array value can be indexed");
			return { ResolveState::Invalid, {}, false, {} };
		}
		const std::size_t suppliedRank = node.children.size() - 1;
		if (byteSetIndex && suppliedRank != 1) {
			AddSemanticError(report, context.path, context.line, "array_index_rank_mismatch", "byte-set access requires exactly one index");
			return { ResolveState::Invalid, {}, false, {} };
		}
		if (base.state == ResolveState::Valid && base.type.arrayRank.has_value() &&
			suppliedRank > *base.type.arrayRank) {
			AddSemanticError(report, context.path, context.line, "array_index_rank_mismatch", "array access provides more indexes than the declared array rank");
			return { ResolveState::Invalid, {}, false, {} };
		}
		for (std::size_t i = 1; i < node.children.size(); ++i) {
			const EvaluatedExpression index = EvaluateExpression(*node.children[i], context, report);
			if (index.state == ResolveState::Invalid) return index;
			if (index.state == ResolveState::Unknown) state = ResolveState::Unknown;
			if (index.state == ResolveState::Valid && IsGeneric(index.type)) state = ResolveState::Unknown;
			else if (index.state == ResolveState::Valid && !IsNumeric(index.type) && Category(index.type) != TypeCategory::ByteSet) {
				AddSemanticError(report, context.path, context.line, "array_index_type_mismatch", "array indexes must be numeric expressions");
				return { ResolveState::Invalid, {}, false, {} };
			}
		}
		TypeInfo type = base.type;
		if (byteSetIndex) {
			type = TypeInfo { .name = "字节型" };
			return { state, type, base.lvalue, base.name, true };
		}
		if (base.type.arrayRank.has_value() && suppliedRank < *base.type.arrayRank) {
			type.arrayRank = *base.type.arrayRank - suppliedRank;
		}
		else {
			type.array = false;
			type.arrayRank.reset();
			if (!base.type.arrayRank.has_value() && base.state == ResolveState::Valid) state = ResolveState::Unknown;
		}
		return { state, type, base.lvalue, base.name, true };
	}
	case SourceExpressionKind::Member: {
		if (node.children.empty()) return { ResolveState::Invalid, {}, false, {} };
		if (node.children.front()->kind == SourceExpressionKind::Name &&
			!node.children.front()->text.empty() && node.children.front()->text.front() == '#') {
			const std::string typeName = node.children.front()->text.substr(1);
			if (const auto typeIt = context.model.types.find(typeName); typeIt != context.model.types.end()) {
				if (const auto memberIt = typeIt->second.members.find(node.text); memberIt != typeIt->second.members.end()) return { ResolveState::Valid, memberIt->second.type, false, node.text };
				AddSemanticError(report, context.path, context.line, "member_not_found", "the published enum or data type has no member with this name");
				return { ResolveState::Invalid, {}, false, node.text };
			}
		}
		const EvaluatedExpression base = EvaluateExpression(*node.children.front(), context, report);
		if (base.state != ResolveState::Valid) return base;
		const auto typeIt = context.model.types.find(base.type.name);
		if (typeIt == context.model.types.end()) return { ResolveState::Unknown, {}, false, node.text };
		if (const auto memberIt = typeIt->second.members.find(node.text); memberIt != typeIt->second.members.end()) return { ResolveState::Valid, memberIt->second.type, memberIt->second.lvalue, node.text };
		if (typeIt->second.methods.contains(node.text)) return { ResolveState::Valid, TypeInfo {}, false, node.text };
		AddSemanticError(report, context.path, context.line, "member_not_found", "the object type has no member with this name");
		return { ResolveState::Invalid, {}, false, node.text };
	}
	case SourceExpressionKind::Unary: {
		if (node.children.empty()) return { ResolveState::Invalid, {}, false, {} };
		const EvaluatedExpression value = EvaluateExpression(*node.children.front(), context, report);
		if (value.state != ResolveState::Valid || IsGeneric(value.type)) return value.state == ResolveState::Valid ? EvaluatedExpression { ResolveState::Unknown, {}, value.lvalue, value.name } : value;
		if (node.text == "!" && !IsLogical(value.type)) {
			AddSemanticError(report, context.path, context.line, "unary_logical_type_mismatch", "logical negation requires a logical expression");
			return { ResolveState::Invalid, {}, false, {} };
		}
		if (node.text != "!" && !IsNumeric(value.type)) {
			AddSemanticError(report, context.path, context.line, "unary_numeric_type_mismatch", "unary plus or minus requires a numeric expression");
			return { ResolveState::Invalid, {}, false, {} };
		}
		return value;
	}
	case SourceExpressionKind::Binary: {
		if (node.children.size() != 2) return { ResolveState::Invalid, {}, false, {} };
		const EvaluatedExpression left = EvaluateExpression(*node.children[0], context, report);
		const EvaluatedExpression right = EvaluateExpression(*node.children[1], context, report);
		if (left.state == ResolveState::Invalid || right.state == ResolveState::Invalid) return { ResolveState::Invalid, {}, false, {} };
		if (left.state == ResolveState::Unknown || right.state == ResolveState::Unknown) return { ResolveState::Unknown, {}, false, {} };
		if (IsGeneric(left.type) || IsGeneric(right.type)) return { ResolveState::Unknown, {}, false, {} };
		const std::string& op = node.text;
		if (op == "且" || op == "或" || op == "&" || op == "|") {
			if (!IsLogical(left.type) || !IsLogical(right.type)) {
				AddSemanticError(report, context.path, context.line, "logical_operator_type_mismatch", "且/或 operands must be logical values");
				return { ResolveState::Invalid, {}, false, {} };
			}
			return { ResolveState::Valid, TypeInfo { .name = "逻辑型" }, false, {} };
		}
		if (op == "＝" || op == "=" || op == "==" || op == "≠" || op == "!=" || op == "<>" || op == "?=" ||
			op == "＜" || op == "<" || op == "＞" || op == ">" || op == "≤" || op == "<=" || op == "≥" || op == ">=") {
			if (!Compatible(left.type, right.type, &context.model)) {
				AddSemanticError(report, context.path, context.line, "comparison_type_mismatch", "comparison operands have definitely incompatible types");
				return { ResolveState::Invalid, {}, false, {} };
			}
			return { ResolveState::Valid, TypeInfo { .name = "逻辑型" }, false, {} };
		}
		if (op == "＋" || op == "+") {
			if ((IsNumeric(left.type) && IsNumeric(right.type)) ||
				(Category(left.type) == TypeCategory::Text && Category(right.type) == TypeCategory::Text) ||
				(Category(left.type) == TypeCategory::ByteSet && Category(right.type) == TypeCategory::ByteSet)) {
				return { ResolveState::Valid, IsNumeric(left.type) ? TypeInfo { .name = "小数型" } : left.type, false, {} };
			}
			AddSemanticError(report, context.path, context.line, "addition_type_mismatch", "addition operands must both be numeric, text, or byte-set values");
			return { ResolveState::Invalid, {}, false, {} };
		}
		if (!IsNumeric(left.type) || !IsNumeric(right.type)) {
			AddSemanticError(report, context.path, context.line, "arithmetic_type_mismatch", "arithmetic operands must be numeric values");
			return { ResolveState::Invalid, {}, false, {} };
		}
		return { ResolveState::Valid, TypeInfo { .name = "小数型" }, false, {} };
	}
	}
	return { ResolveState::Unknown, {}, false, {} };
}

const MethodSymbol* FindMethodForLine(const ProgramSource& program, const std::size_t line)
{
	for (const MethodSymbol& method : program.methods) {
		if (line >= method.headerLine && line <= method.lastBodyLine) return &method;
	}
	return nullptr;
}

bool ParseDirectiveArguments(const std::string& rest, std::vector<std::string>& arguments, bool& hasParentheses)
{
	hasParentheses = false;
	const std::size_t open = rest.find('(');
	if (open == std::string::npos) {
		return Trim(rest).empty();
	}
	if (rest.empty() || rest.back() != ')') return false;
	hasParentheses = true;
	return SplitSourceCallArguments(rest.substr(open + 1, rest.size() - open - 2), arguments);
}

void ValidateCallStatement(
	const std::string& code,
	const EvaluationContext& context,
	SourcePreflightReport& report)
{
	const SourceExpressionParseResult parsed = ParseSourceExpression(code);
	if (!parsed.IsValid()) {
		AddSemanticError(report, context.path, context.line, "expression_syntax_invalid", "expression cannot be parsed; " + parsed.error);
		return;
	}
	if (parsed.root->kind != SourceExpressionKind::Call) {
		AddSemanticError(report, context.path, context.line, "statement_not_callable", "an executable statement must be a subprogram or command call");
		return;
	}
	const std::string callName = parsed.root->children.empty() ? std::string() : CallNameFromNode(*parsed.root->children.front());
	if (callName == "返回") {
		const std::size_t argumentCount = parsed.root->children.size() - 1;
		if (argumentCount > 1) {
			AddSemanticError(report, context.path, context.line, "return_argument_count_invalid", "return accepts at most one value");
			return;
		}
		if (argumentCount == 0 || IsMissing(*parsed.root->children[1])) return;
		const EvaluatedExpression value = EvaluateExpression(*parsed.root->children[1], context, report);
		const TypeInfo& returnType = context.method.callable.returnType;
		if (returnType.name.empty()) {
			if (context.model.externalMetadataComplete) {
				AddSemanticError(report, context.path, context.line, "return_value_unexpected", "a subprogram without a return type cannot return a value");
			}
			return;
		}
		if (Category(returnType) == TypeCategory::Void) {
			AddSemanticError(report, context.path, context.line, "return_value_unexpected", "a subprogram without a return type cannot return a value");
		}
		else if (value.state == ResolveState::Valid && !Compatible(returnType, value.type, &context.model)) {
			AddSemanticError(report, context.path, context.line, "return_type_mismatch", "returned expression is definitely incompatible with the subprogram return type");
		}
		return;
	}
	if (callName == "跳出循环" || callName == "到循环尾") {
		if (parsed.root->children.size() != 1) AddSemanticError(report, context.path, context.line, "loop_transfer_argument_invalid", "loop transfer command does not accept arguments");
		const bool insideLoop = context.flows != nullptr && std::any_of(
			context.flows->begin(),
			context.flows->end(),
			[](const FlowContext& flow) {
				return flow.kind == FlowContext::Kind::While || flow.kind == FlowContext::Kind::DoWhile ||
					flow.kind == FlowContext::Kind::Count || flow.kind == FlowContext::Kind::Variable;
			});
		if (!insideLoop) AddSemanticError(report, context.path, context.line, "loop_transfer_outside_loop", "loop transfer command is only valid inside a loop");
		return;
	}
	(void)EvaluateExpression(*parsed.root, context, report);
}

bool ValidateByteSetAssignmentLiteral(
	const SourceExpressionNode& literal,
	const EvaluationContext& context,
	SourcePreflightReport& report)
{
	for (const auto& child : literal.children) {
		const EvaluatedExpression item = EvaluateExpression(*child, context, report);
		if (item.state == ResolveState::Invalid) return false;
		if (item.state == ResolveState::Valid && !IsNumeric(item.type)) {
			AddSemanticError(report, context.path, context.line, "byte_set_item_type_mismatch", "a byte-set assignment literal can contain only numeric values");
			return false;
		}
		if (IsStaticDecimalLiteral(*child)) {
			AddSemanticError(report, context.path, context.line, "byte_set_item_integer_required", "byte-set literal numeric constants must be integers");
			return false;
		}
		if (const auto staticValue = EvaluateStaticInteger(*child); staticValue.has_value() &&
			(*staticValue < 0 || *staticValue > 255)) {
			AddSemanticError(report, context.path, context.line, "byte_set_item_range_invalid", "byte-set literal integer items must be in the range 0..255");
			return false;
		}
	}
	return true;
}

void ValidateAssignmentStatement(
	const std::string& code,
	const EvaluationContext& context,
	SourcePreflightReport& report)
{
	std::size_t assignmentPosition = 0;
	std::size_t assignmentLength = 0;
	if (!FindSourceTopLevelAssignment(code, assignmentPosition, assignmentLength)) {
		ValidateCallStatement(code, context, report);
		return;
	}
	const std::string leftText = Trim(code.substr(0, assignmentPosition));
	const std::string rightText = Trim(code.substr(assignmentPosition + assignmentLength));
	if (leftText.empty() || rightText.empty()) {
		AddSemanticError(report, context.path, context.line, "assignment_expression_missing", "assignment requires a non-empty target and value");
		return;
	}
	const SourceExpressionParseResult left = ParseSourceExpression(leftText);
	const SourceExpressionParseResult right = ParseSourceExpression(rightText);
	if (!left.IsValid() || !right.IsValid()) {
		AddSemanticError(report, context.path, context.line, "expression_syntax_invalid", "assignment target or value cannot be parsed");
		return;
	}
	if (!IsLvalueNode(*left.root)) {
		AddSemanticError(report, context.path, context.line, "assignment_target_invalid", "assignment target must be a variable, array element, or object member");
		return;
	}
	const EvaluatedExpression target = EvaluateExpression(*left.root, context, report);
	const EvaluatedExpression value = EvaluateExpression(*right.root, context, report);
	if (target.state == ResolveState::Invalid || value.state == ResolveState::Invalid) return;
	if (target.state == ResolveState::Valid && !target.lvalue) {
		AddSemanticError(report, context.path, context.line, "assignment_target_read_only", "assignment target is a constant, enum value, method, or other read-only expression");
		return;
	}
	if (target.state == ResolveState::Valid && !target.type.array &&
		Category(target.type) == TypeCategory::ByteSet &&
		right.root->kind == SourceExpressionKind::ByteSetLiteral) {
		(void)ValidateByteSetAssignmentLiteral(*right.root, context, report);
		return;
	}
	if (target.state == ResolveState::Valid && value.state == ResolveState::Valid && !Compatible(target.type, value.type, &context.model) &&
		!(target.type.array && right.root->kind == SourceExpressionKind::ByteSetLiteral)) {
		AddSemanticError(report, context.path, context.line, "assignment_type_mismatch", "assignment source and target types are definitely incompatible");
	}
}

void ValidateFlowStatement(
	const std::string& code,
	const EvaluationContext& context,
	SourcePreflightReport& report)
{
	const std::string token = DirectiveToken(code);
	std::string rest = Trim(code.substr(token.size()));
	std::vector<std::string> arguments;
	bool hasParentheses = false;
	if (!ParseDirectiveArguments(rest, arguments, hasParentheses)) {
		AddSemanticError(report, context.path, context.line, "flow_expression_syntax_invalid", "flow directive arguments are malformed");
		return;
	}
	static const std::unordered_set<std::string> noArgumentDirectives = {
		".否则", ".如果真结束", ".如果结束", ".判断循环尾", ".循环判断首",
		".计次循环尾", ".变量循环尾", ".默认", ".判断结束",
	};
	if (noArgumentDirectives.contains(token) && !arguments.empty()) {
		AddSemanticError(report, context.path, context.line, "flow_argument_count_invalid", "this flow directive does not accept arguments");
	}
	const auto evaluateArgument = [&](const std::size_t index) -> EvaluatedExpression {
		if (index >= arguments.size() || arguments[index].empty()) return { ResolveState::Valid, {}, false, {} };
		const SourceExpressionParseResult parsed = ParseSourceExpression(arguments[index]);
		if (!parsed.IsValid()) {
			AddSemanticError(report, context.path, context.line, "expression_syntax_invalid", "flow condition or selector cannot be parsed");
			return { ResolveState::Invalid, {}, false, {} };
		}
		return EvaluateExpression(*parsed.root, context, report);
	};
	const auto requireLogical = [&](const std::size_t index) {
		const EvaluatedExpression value = evaluateArgument(index);
		if (value.state == ResolveState::Valid && !IsLogical(value.type)) AddSemanticError(report, context.path, context.line, "flow_condition_type_mismatch", "flow condition must be a logical expression");
	};
	const auto requireNumeric = [&](const std::size_t index) {
		const EvaluatedExpression value = evaluateArgument(index);
		if (value.state == ResolveState::Valid && !IsNumeric(value.type)) AddSemanticError(report, context.path, context.line, "flow_numeric_type_mismatch", "loop count and bounds must be numeric expressions");
	};
	if (token == ".如果真" || token == ".如果" || token == ".判断循环首") {
		if (arguments.size() != 1 || arguments[0].empty()) AddSemanticError(report, context.path, context.line, "flow_argument_count_invalid", "this flow directive requires one condition argument");
		else requireLogical(0);
	}
	else if (token == ".循环判断尾") {
		if (arguments.size() != 1 || arguments[0].empty()) AddSemanticError(report, context.path, context.line, "flow_argument_count_invalid", "loop condition requires one argument");
		else requireLogical(0);
	}
	else if (token == ".计次循环首") {
		if ((arguments.size() != 1 && arguments.size() != 2) || arguments[0].empty()) AddSemanticError(report, context.path, context.line, "flow_argument_count_invalid", "count loop requires a count and optionally a receiving variable");
		else if (arguments.size() == 1 || arguments[1].empty()) requireNumeric(0);
		else {
			requireNumeric(0);
			const SourceExpressionParseResult parsed = ParseSourceExpression(arguments[1]);
			if (parsed.IsValid()) {
				const EvaluatedExpression value = EvaluateExpression(*parsed.root, context, report);
				if (value.state == ResolveState::Valid && (!value.lvalue || !IsNumeric(value.type) || value.type.array)) AddSemanticError(report, context.path, context.line, "flow_counter_variable_invalid", "count loop receiver must be a scalar numeric variable");
			}
		}
	}
	else if (token == ".变量循环首") {
		if ((arguments.size() != 3 && arguments.size() != 4) || arguments[0].empty() || arguments[1].empty() || arguments[2].empty()) AddSemanticError(report, context.path, context.line, "flow_argument_count_invalid", "variable loop requires start, end, step, and optionally a receiving variable");
		else if (arguments.size() == 3 || arguments[3].empty()) {
			requireNumeric(0); requireNumeric(1); requireNumeric(2);
		}
		else {
			requireNumeric(0); requireNumeric(1); requireNumeric(2);
			const SourceExpressionParseResult parsed = ParseSourceExpression(arguments[3]);
			if (parsed.IsValid()) {
				const EvaluatedExpression value = EvaluateExpression(*parsed.root, context, report);
				if (value.state == ResolveState::Valid && (!value.lvalue || !IsNumeric(value.type) || value.type.array)) AddSemanticError(report, context.path, context.line, "flow_counter_variable_invalid", "variable loop receiver must be a scalar numeric variable");
			}
		}
	}
	else if (token == ".判断开始") {
		if (arguments.size() != 1 || arguments[0].empty()) {
			AddSemanticError(report, context.path, context.line, "flow_argument_count_invalid", "switch start requires one logical condition");
		}
		else {
			requireLogical(0);
		}
		if (context.flows != nullptr) context.flows->push_back(FlowContext { .kind = FlowContext::Kind::Switch, .conditionSwitch = true, .line = context.line });
		return;
	}
	else if (token == ".判断") {
		if (arguments.size() != 1 || arguments[0].empty()) {
			AddSemanticError(report, context.path, context.line, "flow_argument_count_invalid", "switch case requires one expression");
			return;
		}
		if (context.flows != nullptr && !context.flows->empty() && context.flows->back().kind == FlowContext::Kind::Switch) {
			const EvaluatedExpression value = evaluateArgument(0);
			if (!context.flows->back().conditionSwitch && value.state == ResolveState::Valid && !Compatible(context.flows->back().switchType, value.type, &context.model)) AddSemanticError(report, context.path, context.line, "switch_case_type_mismatch", "switch case expression is incompatible with the selector type");
			if (context.flows->back().conditionSwitch && value.state == ResolveState::Valid && !IsLogical(value.type)) AddSemanticError(report, context.path, context.line, "switch_condition_type_mismatch", "condition-style switch cases must be logical expressions");
		}
	}
	(void)hasParentheses;
}

void ValidateMethodBody(
	const ProgramSource& program,
	const MethodSymbol& method,
	SemanticModel& model,
	SourcePreflightReport& report)
{
	std::vector<FlowContext> flows;
	for (std::size_t index = method.firstBodyLine; index <= method.lastBodyLine && index <= program.lines.size(); ++index) {
		const std::string code = Trim(StripComment(program.lines[index - 1]));
		if (code.empty() || StartsWith(code, "'") || code == ".版本 2") continue;
		EvaluationContext context { .model = model, .program = program, .method = method, .flows = &flows, .path = program.path, .line = index };
		if (code.front() == '.') {
			const std::string token = DirectiveToken(code);
			if (token == ".如果真" || token == ".如果") {
				ValidateFlowStatement(code, context, report);
				flows.push_back(FlowContext { .kind = token == ".如果真" ? FlowContext::Kind::IfTrue : FlowContext::Kind::IfElse, .line = index });
			}
			else if (token == ".判断循环首") { ValidateFlowStatement(code, context, report); flows.push_back({ .kind = FlowContext::Kind::While, .line = index }); }
			else if (token == ".循环判断首") { ValidateFlowStatement(code, context, report); flows.push_back({ .kind = FlowContext::Kind::DoWhile, .line = index }); }
			else if (token == ".计次循环首") { ValidateFlowStatement(code, context, report); flows.push_back({ .kind = FlowContext::Kind::Count, .line = index }); }
			else if (token == ".变量循环首") { ValidateFlowStatement(code, context, report); flows.push_back({ .kind = FlowContext::Kind::Variable, .line = index }); }
			else if (token == ".判断开始") { ValidateFlowStatement(code, context, report); }
			else if (token == ".如果真结束") { if (flows.empty() || flows.back().kind != FlowContext::Kind::IfTrue) AddSemanticError(report, program.path, index, "flow_end_unexpected", "if-true end does not match the open flow block"); else flows.pop_back(); }
			else if (token == ".如果结束") { if (flows.empty() || flows.back().kind != FlowContext::Kind::IfElse) AddSemanticError(report, program.path, index, "flow_end_unexpected", "if end does not match the open flow block"); else flows.pop_back(); }
			else if (token == ".否则") { if (flows.empty() || flows.back().kind != FlowContext::Kind::IfElse) AddSemanticError(report, program.path, index, "else_unexpected", "else is outside an if block"); }
			else if (token == ".判断循环尾" || token == ".循环判断尾" || token == ".计次循环尾" || token == ".变量循环尾") {
				const FlowContext::Kind expected = token == ".判断循环尾" ? FlowContext::Kind::While : token == ".循环判断尾" ? FlowContext::Kind::DoWhile : token == ".计次循环尾" ? FlowContext::Kind::Count : FlowContext::Kind::Variable;
				if (token == ".循环判断尾") ValidateFlowStatement(code, context, report);
				if (flows.empty() || flows.back().kind != expected) AddSemanticError(report, program.path, index, "flow_end_unexpected", "loop end does not match the open flow block"); else flows.pop_back();
			}
			else if (token == ".判断") { ValidateFlowStatement(code, context, report); }
			else if (token == ".默认" || token == ".判断结束") { if (token == ".判断结束") { if (flows.empty() || flows.back().kind != FlowContext::Kind::Switch) AddSemanticError(report, program.path, index, "switch_end_unexpected", "switch end does not match the open switch"); else flows.pop_back(); } }
			else if (token == ".支持库") { /* page-level declaration already handled */ }
			else { /* declaration directives are handled by the structural preflight */ }
			continue;
		}
		ValidateAssignmentStatement(code, context, report);
	}
	for (const FlowContext& flow : flows) AddSemanticError(report, program.path, flow.line, "flow_end_missing", "flow block is not closed before the subprogram ends");
}

void ValidateDeclaredTypes(const SemanticModel& model, SourcePreflightReport& report)
{
	for (const auto& [name, symbol] : model.globals) {
		if (!symbol.type.name.empty() && !IsKnownBuiltinType(symbol.type.name) && !model.types.contains(symbol.type.name) && model.externalMetadataComplete) AddSemanticError(report, "src/.全局变量.txt", 1, "type_not_found", "global variable type is not declared: " + LocalTextToUtf8(symbol.type.name));
	}
	for (const ProgramSource& program : model.programs) {
		for (const auto& [name, symbol] : program.classSymbols) {
			if (!symbol.type.name.empty() && !IsKnownBuiltinType(symbol.type.name) && !model.types.contains(symbol.type.name) && model.externalMetadataComplete) AddSemanticError(report, program.path, 1, "type_not_found", "assembly variable type is not declared: " + LocalTextToUtf8(symbol.type.name));
		}
		for (const MethodSymbol& method : program.methods) {
			if (!method.callable.returnType.name.empty() && !IsKnownBuiltinType(method.callable.returnType.name) && !model.types.contains(method.callable.returnType.name) && model.externalMetadataComplete) AddSemanticError(report, program.path, method.headerLine, "type_not_found", "subprogram return type is not declared: " + LocalTextToUtf8(method.callable.returnType.name));
			for (const auto& [name, symbol] : method.symbols) if (!symbol.type.name.empty() && !IsKnownBuiltinType(symbol.type.name) && !model.types.contains(symbol.type.name) && model.externalMetadataComplete) AddSemanticError(report, program.path, method.headerLine, "type_not_found", "local or parameter type is not declared: " + LocalTextToUtf8(symbol.type.name));
		}
	}
}

void ValidateCrossPageNames(const ProjectBundle& bundle, SourcePreflightReport& report)
{
	struct Origin { std::string path; std::size_t line = 0; };
	std::unordered_map<std::string, Origin> localTypes;
	const std::vector<std::string> dataTypeLines = SplitLines(bundle.dataTypeText);
	for (std::size_t index = 0; index < dataTypeLines.size(); ++index) {
		const std::string line = Trim(StripComment(dataTypeLines[index]));
		std::string rest;
		if (!MatchDirective(line, "数据类型", &rest)) continue;
		const std::string name = Field(SplitFields(rest), 0);
		if (!name.empty()) localTypes.emplace(name, Origin { "src/.数据类型.txt", index + 1 });
	}
	for (const BundleSourceFile& file : bundle.sourceFiles) {
		const std::string path = file.relativePath.empty() ? file.logicalName : file.relativePath;
		const std::vector<std::string> lines = SplitLines(file.content);
		for (std::size_t index = 0; index < lines.size(); ++index) {
			const std::string line = Trim(StripComment(lines[index]));
			std::string rest;
			if (!MatchDirective(line, "程序集", &rest)) continue;
			const std::string name = Field(SplitFields(rest), 0);
			if (name.empty()) continue;
			const auto [it, inserted] = localTypes.emplace(name, Origin { path, index + 1 });
			if (!inserted) AddSemanticError(report, path, index + 1, "type_name_conflict", "assembly and data-type names must be unique across source pages");
		}
	}

	std::unordered_map<std::string, Origin> constantNames;
	const std::vector<std::string> constantLines = SplitLines(bundle.constantText);
	for (std::size_t index = 0; index < constantLines.size(); ++index) {
		const std::string line = Trim(StripComment(constantLines[index]));
		std::string rest;
		if (!MatchDirective(line, "常量", &rest)) continue;
		const std::string name = Field(SplitFields(rest), 0);
		if (!name.empty()) constantNames.emplace(name, Origin { "src/.常量.txt", index + 1 });
	}
	for (const BundleBinaryResource& resource : bundle.resources) {
		if (resource.logicalName.empty()) continue;
		if (constantNames.contains(resource.logicalName)) AddSemanticError(report, resource.relativePath, 1, "constant_resource_name_conflict", "constant and binary resource names share the same #name namespace");
		else constantNames.emplace(resource.logicalName, Origin { resource.relativePath, 1 });
	}
}

}  // namespace

void ValidateProjectBundleSemantics(const ProjectBundle& bundle, SourcePreflightReport& report)
{
	SemanticModel model;
	// `.ec` 拆包通过内部 `.e` 桥接暴露源码。桥接源码中的匿名原生类、
	// 支持库命令和窗体成员不一定能从公开头文件还原，但未修改源码仍可
	// 依靠原生快照完整回封；这些符号只能判定为未知，不能当成确定错误。
	if (bundle.sourceFileKind == SourceFileKind::EC) {
		model.externalMetadataComplete = false;
	}
	for (const BundleBinaryResource& resource : bundle.resources) model.resources.insert(resource.logicalName);
	CollectGlobalPage(bundle.globalText, model, report);
	CollectStructPage(bundle.dataTypeText, model);
	CollectDllPage(bundle.dllDeclareText, model);
	CollectConstantPage(bundle.constantText, model);
	for (const BundleSourceFile& file : bundle.sourceFiles) CollectProgramSource(file, model);
	CollectFormSymbols(bundle, model, report);
	AddBuiltinCallables(model);
	CollectDependencies(bundle, model);
	ValidateCrossPageNames(bundle, report);
	ValidateDeclaredTypes(model, report);
	for (const ProgramSource& program : model.programs) {
		for (const MethodSymbol& method : program.methods) ValidateMethodBody(program, method, model, report);
	}
	if (!model.externalMetadataComplete) {
		// `.ec` 公开源码不包含完整的支持库/匿名类型元数据。保留语法、
		// 结构和名称冲突错误，但丢弃依赖这些元数据才能确定的类型诊断。
		static const std::unordered_set<std::string> uncertainCodes = {
			"address_of_target_not_found", "addition_type_mismatch", "arithmetic_type_mismatch",
			"array_index_rank_mismatch", "array_index_type_mismatch", "assignment_type_mismatch",
			"call_not_found", "call_signature_mismatch", "comparison_type_mismatch",
			"constant_not_found", "flow_condition_type_mismatch", "flow_counter_variable_invalid",
			"flow_numeric_type_mismatch", "form_event_handler_not_found", "index_target_not_array",
			"logical_operator_type_mismatch", "member_not_found", "return_type_mismatch",
			"symbol_not_found", "switch_case_type_mismatch",
			"switch_condition_type_mismatch", "type_not_found", "unary_logical_type_mismatch",
			"unary_numeric_type_mismatch",
		};
		report.errors.erase(
			std::remove_if(
				report.errors.begin(),
				report.errors.end(),
				[&](const SourcePreflightDiagnostic& diagnostic) { return uncertainCodes.contains(diagnostic.code); }),
			report.errors.end());
	}
}

}  // namespace e2txt
