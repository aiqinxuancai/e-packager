#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <utility>
#include <vector>

namespace e2txt {

// 支持库文件的定位信息；resolvedPath 非空时优先使用该路径。
struct FormControlSupportLibrary {
	std::string fileName;
	std::string resolvedPath;
};

// 窗口组件属性的公开定义。
struct FormControlPropertyDefinition {
	std::string name;
	std::string englishName;
	std::string xmlName;
	std::int16_t dataType = 0;
	std::uint16_t state = 0;
	std::size_t metadataIndex = 0;
	std::size_t callbackIndex = 0;
};

// 属性值的稳定表示，避免把支持库内部指针泄漏到 XML 编解码之外。
enum class FormControlPropertyValueKind {
	Integer,
	Double,
	Boolean,
	Text,
	Binary,
};

struct FormControlPropertyValue {
	FormControlPropertyDefinition definition;
	FormControlPropertyValueKind kind = FormControlPropertyValueKind::Binary;
	std::int32_t integerValue = 0;
	double doubleValue = 0;
	bool booleanValue = false;
	std::string textValue;
	std::vector<std::uint8_t> binaryValue;
};

// 窗口 XML 中用于描述定制属性的轻量节点，避免 codec 依赖 XML 解析器实现。
struct FormControlPropertyXmlNode {
	std::string name;
	std::vector<std::pair<std::string, std::string>> attributes;
	std::vector<FormControlPropertyXmlNode> children;
};

// 已知核心控件定制属性的语义值。
struct FormControlPropertySemanticData {
	bool hasListItems = false;
	std::vector<std::string> listItems;
	bool hasItemValues = false;
	std::vector<std::int32_t> itemValues;
	bool hasTabItems = false;
	std::vector<std::string> tabItems;
};

// 使用支持库公开的窗口单元接口读取和更新专属属性。
class FormControlPropertyCodec {
public:
	FormControlPropertyCodec(
		const std::string& sourcePath,
		const std::vector<FormControlSupportLibrary>& libraries,
		const std::vector<std::filesystem::path>& searchDirectories = {},
		bool restrictSearch = false);
	~FormControlPropertyCodec();

	FormControlPropertyCodec(const FormControlPropertyCodec&) = delete;
	FormControlPropertyCodec& operator=(const FormControlPropertyCodec&) = delete;

	// 读取一个窗口组件的全部可读专属属性。
	bool Decode(
		std::int32_t dataType,
		const std::vector<std::uint8_t>& propertyData,
		std::uint32_t formId,
		std::uint32_t unitId,
		std::vector<FormControlPropertyValue>& outValues,
		std::string* outError = nullptr,
		FormControlPropertySemanticData* outSemantic = nullptr);

	// 根据 XML 中出现的专属属性更新原始属性数据。未出现的字段保持原值。
	bool Apply(
		std::int32_t dataType,
		const std::vector<std::uint8_t>& originalData,
		std::uint32_t formId,
		std::uint32_t unitId,
		const std::vector<std::pair<std::string, std::string>>& xmlAttributes,
		std::vector<std::uint8_t>& outData,
		std::string* outError = nullptr,
		const std::vector<FormControlPropertyXmlNode>& xmlChildren = {});

	// 将读取到的属性值转换成 XML 属性文本。
	static std::string ValueToXmlText(const FormControlPropertyValue& value);

private:
	struct LibraryState;
	struct TypeContext;

	LibraryState* EnsureLibrary(std::uint16_t supportIndex);
	bool BuildTypeContext(
		std::int32_t dataType,
		TypeContext& out,
		std::string* outError);
	bool EnsureParentWindow();
	std::uint32_t CreateUnit(
		const TypeContext& context,
		const std::vector<std::uint8_t>& data,
		std::uint32_t formId,
		std::uint32_t unitId);

	const std::string m_sourcePath;
	const std::vector<FormControlSupportLibrary> m_libraries;
	const std::vector<std::filesystem::path> m_searchDirectories;
	const bool m_restrictSearch = false;
	std::vector<LibraryState> m_libraryStates;
	void* m_parentWindow = nullptr;
};

}  // namespace e2txt
