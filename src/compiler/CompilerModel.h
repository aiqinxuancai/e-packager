#pragma once

#include "../SourceExpressionParser.h"
#include "../SupportLibraryPublicInfo.h"
#include "../e2txt.h"
#include "CompilerTarget.h"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <unordered_set>
#include <string>
#include <unordered_map>
#include <vector>

namespace ecompiler {

// 独立编译器使用的系统数据类型编码。
inline constexpr std::uint32_t kTypeNull = 0;
inline constexpr std::uint32_t kTypeAll = 0x80000000u;
inline constexpr std::uint32_t kTypeByte = 0x80000101u;
inline constexpr std::uint32_t kTypeShort = 0x80000201u;
inline constexpr std::uint32_t kTypeInt = 0x80000301u;
inline constexpr std::uint32_t kTypeInt64 = 0x80000401u;
inline constexpr std::uint32_t kTypeFloat = 0x80000501u;
inline constexpr std::uint32_t kTypeDouble = 0x80000601u;
inline constexpr std::uint32_t kTypeBool = 0x80000002u;
inline constexpr std::uint32_t kTypeDateTime = 0x80000003u;
inline constexpr std::uint32_t kTypeText = 0x80000004u;
inline constexpr std::uint32_t kTypeBinary = 0x80000005u;
inline constexpr std::uint32_t kTypeSubroutine = 0x80000006u;
inline constexpr std::uint32_t kTypeArrayFlag = 0x20000000u;
// 窗口组件是运行时句柄，不把它伪装成支持库复合对象布局。
inline constexpr std::uint32_t kTypeWindowUnit = 0x70000001u;

struct TypeRef {
	std::uint32_t code = kTypeNull;
	bool isArray = false;
	bool valid = false;
};

struct Variable {
	std::string name;
	std::string typeName;
	TypeRef type;
	bool byReference = false;
	bool nullable = false;
	std::vector<int> arrayDimensions;
	std::size_t sourceLine = 0;
};

enum class StatementKind {
	Expression,
	Assignment,
	Return,
	IfTrue,
	IfElse,
	Switch,
	While,
	DoWhile,
	CountLoop,
	ForLoop,
	Break,
	Continue,
	MachineCode,
};

struct StatementBranch {
	std::unique_ptr<e2txt::SourceExpressionNode> condition;
	std::vector<struct Statement> body;
};

struct Statement {
	StatementKind kind = StatementKind::Expression;
	std::size_t sourceLine = 0;
	std::unique_ptr<e2txt::SourceExpressionNode> expression;
	std::unique_ptr<e2txt::SourceExpressionNode> target;
	std::vector<std::unique_ptr<e2txt::SourceExpressionNode>> arguments;
	std::vector<Statement> body;
	std::vector<Statement> elseBody;
	std::vector<StatementBranch> branches;
	std::vector<std::uint8_t> machineCode;
};

struct Method {
	std::size_t id = 0;
	std::size_t assemblyIndex = 0;
	std::string name;
	std::string sourceFile;
	std::size_t sourceLine = 0;
	std::string returnTypeName;
	TypeRef returnType;
	TypeRef ownerType;
	std::string exportName;
	bool isPublic = false;
	bool usesCdecl = false;
	std::vector<Variable> parameters;
	std::vector<Variable> locals;
	std::vector<Statement> body;
};

struct Assembly {
	std::string name;
	std::string sourceFile;
	bool isClass = false;
	std::vector<Variable> variables;
	std::vector<std::size_t> methodIds;
};

struct Library {
	std::size_t ordinal = 0;
	e2txt::Dependency dependency;
	support_library_public_info::LibraryMetadata metadata;
	// 二进制 FNE 可加载时为 true；仅从 elib/*.txt 恢复接口时为 false。
	// 后者允许未触达的兼容声明参与类型检查，但不能产生可执行调用。
	bool implementationAvailable = true;
};

// 项目 DLL 声明页中的外部命令。
struct DllCommand {
	std::string name;
	std::string fileName;
	std::string entryName;
	TypeRef returnType;
	bool usesCdecl = false;
	std::vector<Variable> parameters;
	std::size_t sourceLine = 0;
};

struct Constant {
	std::string name;
	std::uint32_t type = kTypeNull;
	double numberValue = 0;
	std::string textValue;
};

struct TypeElement {
	std::string name;
	TypeRef type;
	std::size_t offset = 0;
	std::int32_t defaultValue = 0;
};

struct TypeInfo {
	TypeRef type;
	std::string name;
	std::size_t libraryIndex = static_cast<std::size_t>(-1);
	std::size_t dataTypeIndex = static_cast<std::size_t>(-1);
	std::size_t size = 0;
	bool isEnum = false;
	// FNE metadata may describe optional/window types whose dependent type is
	// absent in a target-architecture build.  Keep the declaration available
	// for name resolution, but do not emit a layout until it is complete.
	bool layoutComplete = true;
	std::vector<TypeElement> elements;
	std::vector<std::size_t> memberCommandIndexes;
	std::vector<std::size_t> memberMethodIds;
};

// 窗口事件对应的宿主消息类别。Unknown 保留原始事件索引回退路径。
enum class WindowEventTrigger {
	Unknown,
	Created,
	Closing,
	Destroyed,
	SizeChanged,
	Moved,
	Activated,
	Deactivated,
	FocusGained,
	FocusLost,
	Clicked,
	DoubleClicked,
	DropDown,
	SelectionChanged,
	PositionChanged,
	Changed,
	KeyDown,
	KeyUp,
	MouseDown,
	MouseUp,
	MouseMove,
	Paint,
	Timer,
	ListClosed,
	SelectionChanging,
	CharInput,
	RightMouseDown,
	RightMouseUp,
	FirstActivated,
	Shown,
	Hidden,
	Idle,
	Tray,
	MouseEnter,
	MouseLeave,
};

// 窗口 XML 中的事件绑定及其运行时触发类别。
struct WindowEventBinding {
	std::int32_t index = -1;
	std::size_t methodId = static_cast<std::size_t>(-1);
	std::string name;
	WindowEventTrigger trigger = WindowEventTrigger::Unknown;
};

// 独立编译器使用的窗口设计期模型。窗口组件的原始扩展数据同时保留，
// 已能从支持库公开接口解码的属性放入 properties，供编译器和诊断使用。
struct WindowProperty {
	std::string name;
	std::string englishName;
	std::string xmlName;
	std::int16_t dataType = 0;
	std::uint16_t state = 0;
	std::size_t metadataIndex = 0;
	std::size_t callbackIndex = 0;
	std::int32_t integerValue = 0;
	double doubleValue = 0;
	bool booleanValue = false;
	std::string textValue;
	std::vector<std::uint8_t> binaryValue;
};

struct WindowControl {
	std::uint32_t id = 0;
	std::int32_t parentId = 0;
	std::string typeName;
	std::string name;
	std::int32_t left = 0;
	std::int32_t top = 0;
	std::int32_t width = 0;
	std::int32_t height = 0;
	std::string text;
	bool visible = true;
	bool disabled = false;
	bool tabStop = true;
	std::int32_t tabIndex = 0;
	bool tabPageBreak = false;
	std::int32_t tabOwner = 0;
	std::int32_t tabPage = -1;
	std::vector<std::string> tabPageTitles;
	std::vector<std::pair<std::string, std::string>> attributes;
	std::vector<std::string> listItems;
	std::vector<std::int32_t> itemValues;
	bool listItemsDefined = false;
	bool itemValuesDefined = false;
	std::vector<std::uint8_t> extensionData;
	std::vector<WindowProperty> properties;
	std::vector<std::int32_t> children;
	std::vector<WindowEventBinding> events;
};

struct WindowForm {
	std::string name;
	std::string className;
	std::size_t assemblyIndex = static_cast<std::size_t>(-1);
	std::uint32_t id = 0;
	std::int32_t left = 0;
	std::int32_t top = 0;
	std::int32_t width = 640;
	std::int32_t height = 480;
	std::string title;
	std::int32_t border = 2;
	std::int32_t position = 0;
	bool visible = true;
	bool disabled = false;
	bool controlButtons = true;
	bool maximizeButton = true;
	bool minimizeButton = true;
	bool canMove = true;
	bool topmost = false;
	bool showInTaskbar = true;
	bool escapeCloses = false;
	std::vector<std::pair<std::string, std::string>> attributes;
	std::vector<std::uint8_t> extensionData;
	std::vector<WindowProperty> properties;
	std::vector<WindowControl> controls;
	std::vector<WindowEventBinding> events;
};

struct Program {
	bool buildDll = false;
	bool windowsGui = false;
	// 仅传统 x86 静态库需要 VC6/MFC 运行时桥接；现代 adapter 不应引入它。
	bool useLegacyX86RuntimeBridge = false;
	TargetArchitecture targetArchitecture = TargetArchitecture::X86;
	// 当前编译配置启用的条件宏。宏名称按易语言规则不区分大小写。
	std::unordered_set<std::string> conditionMacros;
	e2txt::ProjectBundle bundle;
	std::filesystem::path inputRoot;
	std::vector<std::filesystem::path> supportLibrarySearchDirectories;
	std::vector<Library> libraries;
	std::vector<DllCommand> dllCommands;
	std::vector<Assembly> assemblies;
	// 项目级全局变量页（与程序集变量使用同一类型模型）。
	std::vector<Variable> globals;
	std::vector<Method> methods;
	std::vector<TypeInfo> types;
	std::unordered_map<std::string, TypeRef> typeByName;
	std::unordered_map<std::uint32_t, std::size_t> typeByCode;
	std::unordered_map<std::string, std::size_t> methodByName;
	std::unordered_map<std::string, std::vector<std::pair<std::size_t, std::size_t>>> globalCommands;
	std::unordered_map<std::string, std::size_t> dllCommandByName;
	std::unordered_map<std::string, Constant> constants;
	std::vector<WindowForm> windows;

	const TypeInfo* FindType(std::uint32_t code) const;
	std::uint32_t NormalizeLibraryType(std::size_t libraryIndex, std::uint32_t code) const;
};

// 从目录包或原生工程生成语义模型，并直接从每个目标架构 FNE 读取绑定信息。
bool BuildCompilerModel(
	e2txt::ProjectBundle bundle,
	const std::filesystem::path& inputRoot,
	const std::vector<std::filesystem::path>& supportLibrarySearchDirectories,
	const std::vector<std::string>& conditionMacros,
	TargetArchitecture targetArchitecture,
	bool restrictSupportLibrarySearch,
	Program& outProgram,
	std::string& outError);

}  // namespace ecompiler
