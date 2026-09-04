#include "WindowModel.h"

#include "../FormControlPropertyCodec.h"
#include "../PathHelper.h"
#include "../SimpleXmlDocument.h"

#include <algorithm>
#include <charconv>
#include <cctype>
#include <cstring>
#include <iostream>
#include <string_view>

namespace ecompiler {
namespace {

std::string Attribute(const e2txt::SimpleXmlNode& node, const char* name)
{
	const auto it = node.attributes.find(name);
	return it == node.attributes.end() ? std::string() : it->second;
}

std::vector<std::pair<std::string, std::string>> CopyAttributes(const e2txt::SimpleXmlNode& node)
{
	std::vector<std::pair<std::string, std::string>> result;
	result.reserve(node.attributes.size());
	for (const auto& [name, value] : node.attributes) result.emplace_back(name, value);
	std::sort(result.begin(), result.end(), [](const auto& left, const auto& right) {
		return left.first < right.first;
	});
	return result;
}

std::int32_t IntAttribute(const e2txt::SimpleXmlNode& node, const char* name, const std::int32_t fallback)
{
	const std::string value = Attribute(node, name);
	if (value.empty()) return fallback;
	std::int32_t result = 0;
	const auto parsed = std::from_chars(value.data(), value.data() + value.size(), result);
	return parsed.ec == std::errc() && parsed.ptr == value.data() + value.size() ? result : fallback;
}

bool ContainsAsciiInsensitive(const std::string& value, const std::string_view needle)
{
	if (needle.empty()) return true;
	for (std::size_t start = 0; start + needle.size() <= value.size(); ++start) {
		bool matched = true;
		for (std::size_t index = 0; index < needle.size(); ++index) {
			const unsigned char left = static_cast<unsigned char>(value[start + index]);
			const unsigned char right = static_cast<unsigned char>(needle[index]);
			if (std::tolower(left) != std::tolower(right)) {
				matched = false;
				break;
			}
		}
		if (matched) return true;
	}
	return false;
}

bool NameContains(const std::string& value, const std::string_view text)
{
	return value.find(text) != std::string::npos;
}

bool IsCheckCollection(const e2txt::FormControlPropertyCollection& collection)
{
	return NameContains(collection.definition.name, "选中") ||
		NameContains(collection.definition.name, "选择状态") ||
		ContainsAsciiInsensitive(collection.definition.englishName, "check") ||
		ContainsAsciiInsensitive(collection.definition.englishName, "checked");
}

bool IsEnabledCollection(const e2txt::FormControlPropertyCollection& collection)
{
	return NameContains(collection.definition.name, "允许") ||
		NameContains(collection.definition.name, "允许状态") ||
		ContainsAsciiInsensitive(collection.definition.englishName, "enable") ||
		ContainsAsciiInsensitive(collection.definition.englishName, "enabled");
}

bool IsItemValueCollection(const e2txt::FormControlPropertyCollection& collection)
{
	return NameContains(collection.definition.name, "项目数值") ||
		ContainsAsciiInsensitive(collection.definition.englishName, "itemdata") ||
		ContainsAsciiInsensitive(collection.definition.englishName, "itemvalue");
}

bool IsTabTitleCollection(const e2txt::FormControlPropertyCollection& collection)
{
	return NameContains(collection.definition.name, "子夹") ||
		ContainsAsciiInsensitive(collection.definition.englishName, "tab") ||
		ContainsAsciiInsensitive(collection.definition.englishName, "page");
}

bool CollectionBooleanValue(
	const e2txt::FormControlPropertyCollection& collection,
	const std::size_t index,
	const bool fallback)
{
	if (collection.kind == e2txt::FormControlPropertyCollectionKind::Integer) {
		return index < collection.integerValues.size() && collection.integerValues[index] != 0;
	}
	if (index >= collection.textValues.size()) return fallback;
	const std::string& value = collection.textValues[index];
	return value == "真" || value == "1" || ContainsAsciiInsensitive(value, "true")
		? true
		: (value == "假" || value == "0" || ContainsAsciiInsensitive(value, "false") ? false : fallback);
}

bool BoolAttribute(const e2txt::SimpleXmlNode& node, const char* name, const bool fallback)
{
	const std::string value = Attribute(node, name);
	if (value == "真" || value == "1" || value == "true") return true;
	if (value == "假" || value == "0" || value == "false") return false;
	return fallback;
}

std::string EventName(const e2txt::SimpleXmlNode& node)
{
	return Attribute(node, "名称");
}

WindowEventTrigger ClassifyEvent(
	const std::string& name,
	const std::int32_t index,
	const std::string& nodeName)
{
	const std::size_t separator = nodeName.find('.');
	const std::string typeName = separator == std::string::npos
		? nodeName : nodeName.substr(0, separator);
	const bool isWindow = typeName == "窗口";
	if (name == "创建完毕") return WindowEventTrigger::Created;
	// Only the form's close-query events use "关闭".  A combo/list close
	// notification is a separate event and must be classified below.
	if (isWindow && name.find("关闭") != std::string::npos) return WindowEventTrigger::Closing;
	if (name.find("销毁") != std::string::npos) return WindowEventTrigger::Destroyed;
	if (name.find("尺寸") != std::string::npos || name.find("大小") != std::string::npos) return WindowEventTrigger::SizeChanged;
	if (name == "位置被改变") return isWindow ? WindowEventTrigger::Moved : WindowEventTrigger::PositionChanged;
	if (name == "被激活") return WindowEventTrigger::Activated;
	if (name == "被取消激活") return WindowEventTrigger::Deactivated;
	if (name == "获得焦点") return WindowEventTrigger::FocusGained;
	if (name == "失去焦点") return WindowEventTrigger::FocusLost;
	if (name == "被单击") return WindowEventTrigger::Clicked;
	if (name == "列表项被选择" || name == "子夹被改变") return WindowEventTrigger::SelectionChanged;
	if (name == "选中状态被改变") return WindowEventTrigger::CheckChanged;
	if (name == "被双击") return WindowEventTrigger::DoubleClicked;
	if (name == "将弹出列表") return WindowEventTrigger::DropDown;
	if (name == "列表被关闭") return WindowEventTrigger::ListClosed;
	if (name == "将改变子夹") return WindowEventTrigger::SelectionChanging;
	if (name == "字符输入") return WindowEventTrigger::CharInput;
	if (name == "滚动条位置被改变" || name == "调节钮被按下") return WindowEventTrigger::PositionChanged;
	if (name == "首次激活") return WindowEventTrigger::FirstActivated;
	if (name == "被显示") return WindowEventTrigger::Shown;
	if (name == "被隐藏") return WindowEventTrigger::Hidden;
	if (name == "空闲") return WindowEventTrigger::Idle;
	if (name == "托盘事件") return WindowEventTrigger::Tray;
	if (name == "鼠标进入") return WindowEventTrigger::MouseEnter;
	if (name == "鼠标离开") return WindowEventTrigger::MouseLeave;
	if (name == "反馈事件") return WindowEventTrigger::Feedback;
	if (name == "现行选中项被改变" || name == "选择被改变") return WindowEventTrigger::SelectionChanged;
	if (name == "选择日期被改变" || name == "编辑内容被改变") return WindowEventTrigger::Changed;
	if (name == "双击选择") return WindowEventTrigger::DoubleClicked;
	if (name.find("位置被改变") != std::string::npos) return isWindow ? WindowEventTrigger::Moved : WindowEventTrigger::PositionChanged;
	if (name.find("改变") != std::string::npos || name == "内容被改变") return WindowEventTrigger::Changed;
	if ((name.find("按键") != std::string::npos || name.find("某键") != std::string::npos) && name.find("按下") != std::string::npos) return WindowEventTrigger::KeyDown;
	if ((name.find("按键") != std::string::npos || name.find("某键") != std::string::npos) && name.find("放开") != std::string::npos) return WindowEventTrigger::KeyUp;
	if (name.find("鼠标") != std::string::npos && name.find("双击") != std::string::npos) return WindowEventTrigger::DoubleClicked;
	if (name.find("鼠标右键") != std::string::npos && name.find("按下") != std::string::npos) return WindowEventTrigger::RightMouseDown;
	if (name.find("鼠标右键") != std::string::npos && name.find("放开") != std::string::npos) return WindowEventTrigger::RightMouseUp;
	if (name.find("鼠标") != std::string::npos && name.find("移动") != std::string::npos) return WindowEventTrigger::MouseMove;
	if (name.find("鼠标") != std::string::npos && name.find("按下") != std::string::npos) return WindowEventTrigger::MouseDown;
	if (name.find("鼠标") != std::string::npos && name.find("放开") != std::string::npos) return WindowEventTrigger::MouseUp;
	if (name == "重画" || name == "需要重画" || name == "绘画") return WindowEventTrigger::Paint;
	if (name == "周期事件") return WindowEventTrigger::Timer;
	// Older core controls expose internal event names in XML.  Their stable
	// event index is still part of the public window type contract.
	if (isWindow) {
		if (index == 0) return WindowEventTrigger::Created;
		// The classic core's opaque EVENT_INFO names are stable by index:
		// 1/12 are the two close-query events, 2 is destruction, 3 is move,
		// 4 is resize, and 5/6 are activation transitions.
		if (index == 1 || index == 12) return WindowEventTrigger::Closing;
		if (index == 2) return WindowEventTrigger::Destroyed;
		if (index == 3) return WindowEventTrigger::Moved;
		if (index == 4) return WindowEventTrigger::SizeChanged;
		if (index == 5) return WindowEventTrigger::Activated;
		if (index == 6) return WindowEventTrigger::Deactivated;
	}
	if (typeName == "选择夹") {
		if (index == 0) return WindowEventTrigger::Clicked;
		if (index == 1) return WindowEventTrigger::SelectionChanging;
		if (index == 2) return WindowEventTrigger::SelectionChanged;
	}
	if ((typeName == "列表框" || typeName == "选择列表框") && index == 0) return WindowEventTrigger::SelectionChanged;
	if (typeName == "列表框" && index == 1) return WindowEventTrigger::DoubleClicked;
	// 选择列表框 exposes a separate check-state event between selection
	// change and double-click.  The Win32 listbox reports both through the
	// selection notification, so use the same trigger and let the dispatcher
	// invoke the bound handler consistently.
	if (typeName == "选择列表框" && index == 1) return WindowEventTrigger::CheckChanged;
	if (typeName == "选择列表框" && index == 2) return WindowEventTrigger::DoubleClicked;
	if (typeName == "组合框") {
		if (index == 0) return WindowEventTrigger::SelectionChanged;
		if (index == 1) return WindowEventTrigger::Changed;
		if (index == 2) return WindowEventTrigger::DropDown;
		if (index == 3) return WindowEventTrigger::ListClosed;
		if (index == 4) return WindowEventTrigger::DoubleClicked;
	}
	if ((typeName == "日期框" || typeName == "月历") && index == 0) return WindowEventTrigger::Changed;
	if ((typeName == "滑块条" || typeName == "横向滚动条" || typeName == "纵向滚动条") && index == 0) return WindowEventTrigger::PositionChanged;
	if (typeName == "调节器" && index == 0) return WindowEventTrigger::PositionChanged;
	if (index == -3) return WindowEventTrigger::DoubleClicked;
	if (typeName == "组合框" && index == 2) return WindowEventTrigger::DropDown;
	return WindowEventTrigger::Unknown;
}

void ReadStructuredChildren(
	const e2txt::SimpleXmlNode& node,
	WindowControl& control)
{
	// Explicit collection nodes are decoded by FormControlPropertyCodec from
	// the public UD_CUSTOMIZE metadata.  Keep this compatibility fallback for
	// old bundles whose sidecar is unavailable, but do not key it to a control
	// type or to a fixed support-library property name.
	for (const auto& child : node.children) {
		if (child.name.find('.') == std::string::npos) continue;
		// These nodes are structural, rather than generic collection properties.
		// In particular, a tab page contains ordinary controls whose 标题
		// attributes must never be interpreted as tab header text.
		if (child.name == node.name + ".事件" ||
			child.name == node.name + ".子夹管理" ||
			child.name == node.name + ".子夹") continue;
		bool textCollection = false;
		bool integerCollection = false;
		bool checkedCollection = false;
		bool enabledCollection = false;
		for (const auto& item : child.children) {
			if (item.attributes.contains("数值")) integerCollection = true;
			if (item.attributes.contains("选中") || item.attributes.contains("选择状态")) checkedCollection = true;
			if (item.attributes.contains("允许") || item.attributes.contains("允许状态")) enabledCollection = true;
			if (item.attributes.contains("文本") || item.attributes.contains("标题") ||
				item.attributes.contains("值")) {
				textCollection = true;
			}
		}
		if (checkedCollection && !textCollection && !integerCollection) {
			control.itemCheckedDefined = true;
			for (const auto& item : child.children) {
				if (item.attributes.contains("选中")) control.itemChecked.push_back(BoolAttribute(item, "选中", false));
				else control.itemChecked.push_back(BoolAttribute(item, "选择状态", false));
			}
			continue;
		}
		if (enabledCollection && !textCollection && !integerCollection) {
			control.itemEnabledDefined = true;
			for (const auto& item : child.children) {
				if (item.attributes.contains("允许")) control.itemEnabled.push_back(BoolAttribute(item, "允许", true));
				else control.itemEnabled.push_back(BoolAttribute(item, "允许状态", true));
			}
			continue;
		}
		// A list item's text, value, and check state may be serialized in the
		// same node.  Decode each field independently so one field cannot make
		// the whole collection disappear.
		if (textCollection) {
			if (!control.listItemsDefined) {
				control.listItemsDefined = true;
				for (const auto& item : child.children) {
					if (item.attributes.contains("文本")) control.listItems.push_back(Attribute(item, "文本"));
					else if (item.attributes.contains("值")) control.listItems.push_back(Attribute(item, "值"));
					else if (item.attributes.contains("标题")) control.listItems.push_back(Attribute(item, "标题"));
				}
			}
			if (control.typeName == "选择列表框") {
				if (checkedCollection && !control.itemCheckedDefined) {
					control.itemCheckedDefined = true;
					for (const auto& item : child.children) control.itemChecked.push_back(
						item.attributes.contains("选中") ? BoolAttribute(item, "选中", false) : BoolAttribute(item, "选择状态", false));
				}
				if (enabledCollection && !control.itemEnabledDefined) {
					control.itemEnabledDefined = true;
					for (const auto& item : child.children) control.itemEnabled.push_back(
						item.attributes.contains("允许") ? BoolAttribute(item, "允许", true) : BoolAttribute(item, "允许状态", true));
				}
			}
			if (integerCollection && !control.itemValuesDefined) {
				control.itemValuesDefined = true;
				for (const auto& item : child.children) {
					if (item.attributes.contains("数值")) control.itemValues.push_back(IntAttribute(item, "数值", 0));
				}
			}
			continue;
		}
		if (integerCollection) {
			// The names are the only reliable discriminator for state arrays in
			// old XML bundles where the custom collection type is not published.
			if (checkedCollection && control.typeName == "选择列表框" && !control.itemCheckedDefined) {
				control.itemCheckedDefined = true;
				for (const auto& item : child.children) control.itemChecked.push_back(IntAttribute(item, "数值", 0) != 0);
			}
			else if (enabledCollection && control.typeName == "选择列表框" && !control.itemEnabledDefined) {
				control.itemEnabledDefined = true;
				for (const auto& item : child.children) control.itemEnabled.push_back(IntAttribute(item, "数值", 1) != 0);
			}
			else if (!control.itemValuesDefined) {
				control.itemValuesDefined = true;
				for (const auto& item : child.children) {
					if (item.attributes.contains("数值")) control.itemValues.push_back(IntAttribute(item, "数值", 0));
				}
			}
		}
	}
}

int Base64Value(const unsigned char value)
{
	if (value >= 'A' && value <= 'Z') return value - 'A';
	if (value >= 'a' && value <= 'z') return value - 'a' + 26;
	if (value >= '0' && value <= '9') return value - '0' + 52;
	if (value == '+') return 62;
	if (value == '/') return 63;
	return -1;
}

std::vector<std::uint8_t> DecodeBase64(const std::string& text)
{
	std::vector<std::uint8_t> result;
	if (text.empty() || text.size() % 4 != 0) return result;
	for (std::size_t index = 0; index < text.size(); index += 4) {
		const int a = Base64Value(static_cast<unsigned char>(text[index]));
		const int b = Base64Value(static_cast<unsigned char>(text[index + 1]));
		const unsigned char cChar = static_cast<unsigned char>(text[index + 2]);
		const unsigned char dChar = static_cast<unsigned char>(text[index + 3]);
		const int c = cChar == '=' ? 0 : Base64Value(cChar);
		const int d = dChar == '=' ? 0 : Base64Value(dChar);
		if (a < 0 || b < 0 || c < 0 || d < 0) return {};
		const std::uint32_t value = (static_cast<std::uint32_t>(a) << 18) |
			(static_cast<std::uint32_t>(b) << 12) | (static_cast<std::uint32_t>(c) << 6) |
			static_cast<std::uint32_t>(d);
		result.push_back(static_cast<std::uint8_t>(value >> 16));
		if (cChar != '=') result.push_back(static_cast<std::uint8_t>(value >> 8));
		if (dChar != '=') result.push_back(static_cast<std::uint8_t>(value));
	}
	return result;
}

const TypeInfo* FindWindowType(const Program& program, const std::string& name)
{
	const auto it = program.typeByName.find(name);
	return it == program.typeByName.end() ? nullptr : program.FindType(it->second.code);
}

void CopyDecodedProperty(const e2txt::FormControlPropertyValue& source, WindowProperty& target)
{
	target.name = source.definition.name;
	target.englishName = source.definition.englishName;
	target.xmlName = source.definition.xmlName;
	target.dataType = source.definition.dataType;
	target.state = source.definition.state;
	target.metadataIndex = source.definition.metadataIndex;
	target.callbackIndex = source.definition.callbackIndex;
	target.integerValue = source.integerValue;
	target.doubleValue = source.doubleValue;
	target.booleanValue = source.booleanValue;
	target.textValue = source.textValue;
	target.binaryValue = source.binaryValue;
}

void ReadEvents(
	const e2txt::SimpleXmlNode& node,
	const std::string& nodeName,
	const Program& program,
	const std::string& ownerName,
	std::vector<WindowEventBinding>& output)
{
	for (const auto& child : node.children) {
		if (child.name == nodeName) {
			const std::int32_t index = IntAttribute(child, "索引", -1);
			const std::string eventDisplayName = EventName(child);
			const std::string handler = Attribute(child, "处理器");
			const std::size_t separator = handler.rfind("::");
			const std::string methodName = separator == std::string::npos ? handler : handler.substr(separator + 2);
			const auto exact = std::find_if(program.methods.begin(), program.methods.end(), [&](const Method& method) {
				return method.assemblyIndex < program.assemblies.size() &&
					program.assemblies[method.assemblyIndex].name == ownerName && method.name == methodName;
			});
			// 部分旧工程的 XML 处理器带有规范化前的程序集名，方法名本身仍是可靠的。
			const auto fallback = exact != program.methods.end()
				? exact
				: std::find_if(program.methods.begin(), program.methods.end(), [&](const Method& method) {
					return method.name == methodName;
				});
			if (index != -1 && fallback != program.methods.end()) {
				output.push_back(WindowEventBinding {
					index, fallback->id, eventDisplayName,
					ClassifyEvent(eventDisplayName, index, nodeName) });
			}
		}
	}
}

bool ReadPropertyData(
	const Program& program,
	const std::int32_t dataType,
	const std::vector<std::uint8_t>& extensionData,
	const std::uint32_t formId,
	const std::uint32_t unitId,
	std::vector<WindowProperty>& output,
	std::string* outError,
	e2txt::FormControlPropertySemanticData* outSemantic)
{
	std::vector<e2txt::FormControlSupportLibrary> libraries;
	for (const Library& library : program.libraries) {
		// Decode with the same target-architecture FNE that supplied the
		// metadata and will be linked into the generated program.  A project
		// dependency may retain an old installed path (often x86) in
		// resolvedPath; loading that file here can make an otherwise valid
		// public property probe fail before it reaches the component ABI.
		const std::string resolvedPath = library.dependency.resolvedPath;
		libraries.push_back({ library.dependency.fileName, resolvedPath });
	}
	e2txt::FormControlPropertyCodec codec(
		program.bundle.sourcePath,
		libraries,
		program.supportLibrarySearchDirectories,
		program.targetArchitecture == TargetArchitecture::X64);
	std::vector<e2txt::FormControlPropertyValue> values;
	if (!codec.Decode(dataType, extensionData, formId, unitId, values, outError, outSemantic)) return false;
	for (const auto& value : values) {
		WindowProperty property;
		CopyDecodedProperty(value, property);
		output.push_back(std::move(property));
	}
	return true;
}

std::uint32_t ResolveTypeCode(const Program& program, const std::string& typeName)
{
	if (const auto* type = FindWindowType(program, typeName)) return type->type.code;
	return 0;
}

bool ReadControl(
	const e2txt::SimpleXmlNode& node,
	const Program& program,
	const std::int32_t parentId,
	const std::uint32_t formId,
	const std::string& ownerName,
	std::uint32_t& nextId,
	WindowForm& form,
	std::int32_t& outId,
	const std::int32_t tabOwner,
	const std::int32_t tabPage)
{
	if (node.name.find('.') != std::string::npos) return true;
	if (!HasNativeWin32Class(node.name)) {
		outId = 0;
		return true;
	}
	WindowControl control;
	control.id = nextId++;
	control.parentId = parentId;
	control.typeName = node.name;
	control.name = Attribute(node, "名称");
	control.attributes = CopyAttributes(node);
	control.left = IntAttribute(node, "左边", 0);
	control.top = IntAttribute(node, "顶边", 0);
	control.width = IntAttribute(node, "宽度", 0);
	control.height = IntAttribute(node, "高度", 0);
	control.text = Attribute(node, "标题");
	if (control.text.empty()) control.text = Attribute(node, "内容");
	control.visible = BoolAttribute(node, "可视", true);
	control.disabled = BoolAttribute(node, "禁止", false);
	control.tabStop = BoolAttribute(node, "可停留焦点", true);
	control.tabIndex = IntAttribute(node, "停留顺序", 0);
	control.tabOwner = tabOwner;
	control.tabPage = tabPage;
	if (control.typeName == "选择夹") {
		control.tabCurrentPage = IntAttribute(node, "现行子夹", 0);
	}
	control.extensionData = DecodeBase64(Attribute(node, "扩展属性数据"));
	ReadStructuredChildren(node, control);
	e2txt::FormControlPropertySemanticData semantic;
	const std::uint32_t typeCode = ResolveTypeCode(program, control.typeName);
	if (typeCode != 0) {
		std::string propertyError;
		const bool propertyDecoded = ReadPropertyData(program, static_cast<std::int32_t>(typeCode), control.extensionData,
			formId, control.id, control.properties, &propertyError, &semantic);
		if (!propertyDecoded) {
			// Keep diagnostics ASCII-safe: the CLI may be running under CP936,
			// while control names are UTF-8 and would otherwise appear garbled.
			std::cerr << "window property decode failed: data_type=" << typeCode
				<< " " << propertyError << "\n";
		}
	}
	for (const auto& collection : semantic.collections) {
		const bool checkCollection = IsCheckCollection(collection);
		const bool enabledCollection = IsEnabledCollection(collection);
		if (control.typeName == "选择列表框" && checkCollection) {
			if (!control.itemCheckedDefined) {
				const std::size_t count = collection.kind == e2txt::FormControlPropertyCollectionKind::Integer
					? collection.integerValues.size() : collection.textValues.size();
				control.itemChecked.resize(count, false);
				for (std::size_t index = 0; index < count; ++index)
					control.itemChecked[index] = CollectionBooleanValue(collection, index, false);
				control.itemCheckedDefined = true;
			}
			continue;
		}
		if (control.typeName == "选择列表框" && enabledCollection) {
			if (!control.itemEnabledDefined) {
				const std::size_t count = collection.kind == e2txt::FormControlPropertyCollectionKind::Integer
					? collection.integerValues.size() : collection.textValues.size();
				control.itemEnabled.resize(count, true);
				for (std::size_t index = 0; index < count; ++index)
					control.itemEnabled[index] = CollectionBooleanValue(collection, index, true);
				control.itemEnabledDefined = true;
			}
			continue;
		}
		if (control.typeName == "选择夹" && collection.kind == e2txt::FormControlPropertyCollectionKind::Text &&
			(collection.textValues.empty() || IsTabTitleCollection(collection))) {
			if (control.tabPageTitles.empty()) control.tabPageTitles = collection.textValues;
			continue;
		}
		if ((control.typeName == "列表框" || control.typeName == "选择列表框" || control.typeName == "组合框") &&
			collection.kind == e2txt::FormControlPropertyCollectionKind::Integer &&
			(IsItemValueCollection(collection) || !control.itemValuesDefined)) {
			if (!control.itemValuesDefined) {
				control.itemValues = collection.integerValues;
				control.itemValuesDefined = true;
			}
			continue;
		}
		if ((control.typeName == "列表框" || control.typeName == "选择列表框" || control.typeName == "组合框") &&
			collection.kind == e2txt::FormControlPropertyCollectionKind::Text && !control.listItemsDefined) {
			control.listItems = collection.textValues;
			control.listItemsDefined = true;
		}
	}
	ReadEvents(node, control.typeName + ".事件", program, ownerName, control.events);
	const std::int32_t currentId = static_cast<std::int32_t>(control.id);
	std::vector<const e2txt::SimpleXmlNode*> tabPages;
	const std::string tabPageNodeName = control.typeName + ".子夹";
	const std::string tabManagerNodeName = control.typeName + ".子夹管理";
	std::size_t tabHeaderCount = 0;
	std::size_t explicitPageCount = 0;
	for (const auto& child : node.children) {
		if (child.name == tabPageNodeName) {
			tabPages.push_back(&child);
			const std::int32_t index = IntAttribute(child, "索引", -1);
			if (index >= 0) explicitPageCount = (std::max)(explicitPageCount, static_cast<std::size_t>(index) + 1);
		}
		if (child.name == tabManagerNodeName) {
			for (const auto& header : child.children) {
				if (header.name != "子夹") continue;
				++tabHeaderCount;
				const std::int32_t index = IntAttribute(header, "索引", -1);
				if (index >= 0) explicitPageCount = (std::max)(explicitPageCount, static_cast<std::size_t>(index) + 1);
			}
		}
	}
	if (control.typeName == "选择夹") {
		// All page-bearing representations share one zero-based index space.  An
		// explicit sparse index (for example page 3 with no page 2 node) must
		// still create the intervening empty page so later pages keep their names
		// and controls when the tab is switched back and forth.
		const std::size_t pageCount = (std::max)(explicitPageCount,
			(std::max)(tabPages.size(), (std::max)(tabHeaderCount, control.tabPageTitles.size())));
		if (pageCount > 0) {
			control.tabPageBreak = !tabPages.empty();
			control.tabPageTitles.resize(pageCount);
		}
		else {
			control.tabPageTitles.clear();
		}
	}
	if (!tabPages.empty()) {
		control.tabPageBreak = true;
		// Page groups are authoritative for child ownership; header metadata can
		// contain additional empty pages, which must still remain addressable.
	}
	for (const auto& child : node.children) {
		if (child.name.find('.') != std::string::npos) {
			if (child.name == tabManagerNodeName) {
				std::vector<std::string> titles = control.tabPageTitles;
				std::vector<bool> assigned(titles.size(), false);
				std::size_t fallbackIndex = 0;
				for (const auto& tabHeader : child.children) {
					if (tabHeader.name != "子夹" || titles.empty()) continue;
					std::int32_t index = IntAttribute(tabHeader, "索引", -1);
					if (index < 0 || static_cast<std::size_t>(index) >= titles.size() || assigned[static_cast<std::size_t>(index)]) {
						while (fallbackIndex < assigned.size() && assigned[fallbackIndex]) ++fallbackIndex;
						if (fallbackIndex >= assigned.size()) continue;
						index = static_cast<std::int32_t>(fallbackIndex);
					}
					const auto slot = static_cast<std::size_t>(index);
					const std::string title = Attribute(tabHeader, "标题");
					if (!title.empty() || titles[slot].empty()) titles[slot] = title;
					assigned[slot] = true;
					if (slot == fallbackIndex) ++fallbackIndex;
				}
				control.tabPageTitles = std::move(titles);
				continue;
			}
			continue;
		}
		std::int32_t childId = 0;
		if (!ReadControl(child, program, currentId, formId, ownerName, nextId, form, childId, tabOwner, tabPage)) return false;
		if (childId != 0) control.children.push_back(childId);
	}
	if (control.typeName == "选择夹") {
		// Read page controls in their own groups after ordinary child nodes so
		// each control receives the correct tabOwner/tabPage metadata.
		const std::size_t pageCount = control.tabPageTitles.size();
		std::vector<bool> usedPage(pageCount, false);
		std::size_t fallbackPage = 0;
		for (const auto* pageNode : tabPages) {
			std::int32_t pageIndex = IntAttribute(*pageNode, "索引", -1);
			if (pageIndex < 0 || static_cast<std::size_t>(pageIndex) >= pageCount || usedPage[static_cast<std::size_t>(pageIndex)]) {
				while (fallbackPage < usedPage.size() && usedPage[fallbackPage]) ++fallbackPage;
				if (fallbackPage >= usedPage.size()) continue;
				pageIndex = static_cast<std::int32_t>(fallbackPage);
			}
			const auto page = static_cast<std::size_t>(pageIndex);
			usedPage[page] = true;
			if (page == fallbackPage) ++fallbackPage;
			for (const auto& pageChild : pageNode->children) {
				std::int32_t childId = 0;
				if (!ReadControl(pageChild, program, currentId, formId, ownerName, nextId, form, childId, currentId, pageIndex)) return false;
				if (childId != 0) control.children.push_back(childId);
			}
		}
	}
	form.controls.push_back(std::move(control));
	outId = currentId;
	return true;
}

}  // namespace

bool BuildWindowModel(Program& program, std::string& outError)
{
	program.windows.clear();
	std::uint32_t nextId = 1;
	std::vector<e2txt::WindowBinding> bindings = program.bundle.windowBindings;
	for (const auto& formFile : program.bundle.formFiles) {
		const bool alreadyBound = std::any_of(bindings.begin(), bindings.end(),
			[&](const e2txt::WindowBinding& binding) { return binding.formName == formFile.logicalName; });
		if (alreadyBound) continue;
		const std::string generatedClassName = !formFile.logicalName.empty() && formFile.logicalName.front() == '_'
			? "窗口程序集" + formFile.logicalName
			: "窗口程序集_" + formFile.logicalName;
		if (std::any_of(program.assemblies.begin(), program.assemblies.end(),
			[&](const Assembly& assembly) { return assembly.name == generatedClassName; })) {
			bindings.push_back({ formFile.logicalName, generatedClassName });
		}
		else if (program.assemblies.empty()) {
			bindings.push_back({ formFile.logicalName, generatedClassName });
		}
	}
	for (const e2txt::WindowBinding& binding : bindings) {
		const auto formIt = std::find_if(program.bundle.formFiles.begin(), program.bundle.formFiles.end(),
			[&](const e2txt::BundleFormFile& form) { return form.logicalName == binding.formName; });
		if (formIt == program.bundle.formFiles.end()) continue;
		if (program.assemblies.empty()) {
			Assembly assembly;
			assembly.name = binding.className;
			assembly.sourceFile = "<window-generated>";
			program.assemblies.push_back(std::move(assembly));
		}
		const auto assemblyIt = std::find_if(program.assemblies.begin(), program.assemblies.end(),
			[&](const Assembly& assembly) { return assembly.name == binding.className; });
		if (assemblyIt == program.assemblies.end()) continue;
		e2txt::SimpleXmlNode root;
		e2txt::SimpleXmlParseError parseError;
		if (!e2txt::ParseSimpleXmlDocument(formIt->xmlText, root, &parseError)) {
			outError = formIt->relativePath + ":" + parseError.code;
			return false;
		}
		if (root.name != "窗口") {
			outError = formIt->relativePath + ":form_root_invalid";
			return false;
		}
		WindowForm form;
		form.name = binding.formName;
		form.className = binding.className;
		form.assemblyIndex = static_cast<std::size_t>(assemblyIt - program.assemblies.begin());
		form.id = nextId++;
		form.left = IntAttribute(root, "左边", 50);
		form.top = IntAttribute(root, "顶边", 50);
		form.width = IntAttribute(root, "宽度", 640);
		form.height = IntAttribute(root, "高度", 480);
		form.title = Attribute(root, "标题");
		form.border = IntAttribute(root, "边框", 2);
		form.position = IntAttribute(root, "位置", 0);
		form.visible = BoolAttribute(root, "可视", true);
		form.disabled = BoolAttribute(root, "禁止", false);
		form.controlButtons = BoolAttribute(root, "控制按钮", true);
		form.maximizeButton = BoolAttribute(root, "最大化按钮", true);
		form.minimizeButton = BoolAttribute(root, "最小化按钮", true);
		form.canMove = BoolAttribute(root, "可否移动", true);
		form.enterToNext = BoolAttribute(root, "回车下移焦点", false);
		form.f1OpenHelp = BoolAttribute(root, "F1键打开帮助", false);
		form.helpFileName = Attribute(root, "帮助文件名");
		form.helpContext = IntAttribute(root, "帮助标志值", 0);
	form.hitMove = BoolAttribute(root, "随意移动", false);
	form.topmost = BoolAttribute(root, "总在最前", false);
	form.keepTitleBarActive = BoolAttribute(root, "保持标题条激活", false);
	form.shape = IntAttribute(root, "外形", 0);
	const std::string formBackColor = Attribute(root, "底色");
	if (!formBackColor.empty()) {
		std::int32_t parsedValue = 0;
		const auto parsed = std::from_chars(formBackColor.data(), formBackColor.data() + formBackColor.size(), parsedValue);
		if (parsed.ec == std::errc() && parsed.ptr == formBackColor.data() + formBackColor.size()) {
			form.backColor = parsedValue;
			form.hasBackColor = true;
		}
	}
	form.backPicMode = IntAttribute(root, "底图方式", 0);
		form.playCount = IntAttribute(root, "播放次数", 2);
		form.showInTaskbar = BoolAttribute(root, "在任务条中显示", true);
		form.escapeCloses = BoolAttribute(root, "Esc键关闭", false);
		form.attributes = CopyAttributes(root);
		form.extensionData = DecodeBase64(Attribute(root, "扩展属性数据"));
		const std::uint32_t formTypeCode = ResolveTypeCode(program, "窗口");
		if (formTypeCode != 0) {
			(void)ReadPropertyData(program, static_cast<std::int32_t>(formTypeCode), form.extensionData,
				form.id, form.id, form.properties, nullptr, nullptr);
		}
		ReadEvents(root, "窗口.事件", program, binding.className, form.events);
		for (const auto& child : root.children) {
			if (child.name.find('.') != std::string::npos) continue;
			std::int32_t id = 0;
			if (!ReadControl(child, program, 0, form.id, binding.className, nextId, form, id, 0, -1)) {
				outError = formIt->relativePath + ":control_tree_invalid";
				return false;
			}
		}
		program.windows.push_back(std::move(form));
	}
	return true;
}

}  // namespace ecompiler
