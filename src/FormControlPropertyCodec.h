#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace e2txt {

// 支持库文件的定位信息；resolvedPath 非空时优先使用该路径。
struct FormControlSupportLibrary {
	std::string fileName;
	std::string resolvedPath;
};

// 公开事件签名；类型编号已转换成工程支持库类型编号。
struct FormControlEventParameter {
    std::string name;
    std::int32_t type = 0;
    bool byReference = false;
};
struct FormControlEventDefinition {
    std::int32_t index = 0;
    std::string name;
    std::int32_t returnType = 0;
    std::vector<FormControlEventParameter> parameters;
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
	Unknown,
	Integer,
	Double,
	Boolean,
	Text,
	Binary,
};

struct FormControlPropertyValue {
	FormControlPropertyDefinition definition;
	FormControlPropertyValueKind kind = FormControlPropertyValueKind::Unknown;
	std::int32_t integerValue = 0;
	double doubleValue = 0;
	bool booleanValue = false;
	std::string textValue;
	std::vector<std::uint8_t> binaryValue;
};

// 从自定义属性中识别出的通用集合。原始二进制属性始终另外保留，
// 集合只是便于窗口 XML 读写列表、页签等公开的重复数据。
enum class FormControlPropertyCollectionKind {
	Text,
	Integer,
};

struct FormControlPropertyCollection {
	FormControlPropertyDefinition definition;
	FormControlPropertyCollectionKind kind = FormControlPropertyCollectionKind::Text;
	std::vector<std::string> textValues;
	std::vector<std::int32_t> integerValues;
};

// 窗口 XML 中用于描述定制属性的轻量节点，避免 codec 依赖 XML 解析器实现。
struct FormControlPropertyXmlNode {
	std::string name;
	std::vector<std::pair<std::string, std::string>> attributes;
	std::vector<FormControlPropertyXmlNode> children;
};

// 可从公开属性数据中无损识别的重复值集合。
struct FormControlPropertySemanticData {
	std::vector<FormControlPropertyCollection> collections;
	std::vector<FormControlPropertyXmlNode> structured;
};

// 仅工作进程和独立 ABI 测试使用 InProcess；主程序隔离第三方窗口回调。
enum class FormControlExecutionMode { Isolated, InProcess };

// 使用支持库公开的窗口单元接口读取和更新专属属性。
class FormControlPropertyCodec {
public:
	FormControlPropertyCodec(
		const std::string& sourcePath,
		const std::vector<FormControlSupportLibrary>& libraries,
		const std::vector<std::filesystem::path>& searchDirectories = {},
		bool restrictSearch = false,
		FormControlExecutionMode executionMode = FormControlExecutionMode::Isolated);
	~FormControlPropertyCodec();
	// 工作进程每条请求结束后销毁临时窗口，保留支持库和 OLE 初始化供后续请求复用。
	void ReleaseProbeWindow();

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

	// 根据 XML 中出现的专属属性更新原始属性数据。未出现的字段保持原值；
	// 原始数据为空时以支持库默认实例为基底，由 XML 属性完整重建。
	bool Apply(
		std::int32_t dataType,
		const std::vector<std::uint8_t>& originalData,
		std::uint32_t formId,
		std::uint32_t unitId,
		const std::vector<std::pair<std::string, std::string>>& xmlAttributes,
		std::vector<std::uint8_t>& outData,
		std::string* outError = nullptr,
		const std::vector<FormControlPropertyXmlNode>& xmlChildren = {});

	// 判断不带原始数据、仅凭 XML 属性重建的结果是否与原始属性数据等价，
	// 等价时窗口 XML 可省略“扩展属性数据”。outReason 返回不等价的原因。
	bool CanRebuildFromXml(
		std::int32_t dataType,
		const std::vector<std::uint8_t>& originalData,
		std::uint32_t formId,
		std::uint32_t unitId,
		const std::vector<std::pair<std::string, std::string>>& xmlAttributes,
		const std::vector<FormControlPropertyXmlNode>& xmlChildren,
		std::string* outReason = nullptr);

	// 从支持库公开事件表读取名称和原生索引，包含 IDE 通用事件。
	bool ReadEvents(std::int32_t dataType, std::vector<FormControlEventDefinition>& events, std::string* outError);

	// 将读取到的属性值转换成 XML 属性文本。
	static std::string ValueToXmlText(const FormControlPropertyValue& value);

private:
	// 两种架构均通过 Win32 工作进程隔离原生控件接口。
	std::string BridgeContext() const;
	struct LibraryState;
	struct TypeContext;

	// 进程内执行 Apply（属性已去除保留字段）；outTargets 返回按定义顺序排列的 XML 目标值。
	// verifyPersisted 为假时跳过保存后的回读核对，仅供随后逐字节比较结果的重建检查使用。
	bool ApplyInProcess(
		std::int32_t dataType,
		const std::vector<std::uint8_t>& originalData,
		std::uint32_t formId,
		std::uint32_t unitId,
		const std::vector<std::pair<std::string, std::string>>& propertyAttributes,
		const std::vector<FormControlPropertyXmlNode>& xmlChildren,
		std::vector<std::uint8_t>& outData,
		std::string* outError,
		std::vector<FormControlPropertyValue>* outTargets,
		bool verifyPersisted);
	LibraryState* EnsureLibrary(std::uint16_t supportIndex);
	bool BuildTypeContext(
		std::int32_t dataType,
		TypeContext& out,
		std::string* outError, bool metadataOnly = false);
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
	const FormControlExecutionMode m_executionMode;
	std::vector<LibraryState> m_libraryStates;
	// 空数据创建的默认属性值，只用于推断值类型和补全结构化属性，按控件类型缓存。
	std::unordered_map<std::int32_t, std::vector<FormControlPropertyValue>> m_defaultValues;
	void* m_parentWindow = nullptr;
	// 复用隐藏的顶层宿主；每条请求使用独立子容器，销毁时连同临时控件一起释放。
	void* m_probeHostWindow = nullptr;
	// ActiveX 控件需要当前线程初始化 OLE。
	bool m_oleInitialized = false;
};

}  // namespace e2txt
