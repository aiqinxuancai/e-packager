#include "e2txt.h"

#include <Windows.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <charconv>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <limits>
#include <optional>
#include <sstream>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>

#include <lib2.h>

#include "EFolderCodec.h"
#include "PathHelper.h"

namespace e2txt {

namespace {

constexpr std::uint32_t kMagicFileHeader1 = 1415007811u;
constexpr std::uint32_t kMagicFileHeader2 = 1196576837u;
constexpr std::uint32_t kMagicSection = 353465113u;
constexpr std::uint32_t kSectionEndOfFile = 0x07007319u;

constexpr std::uint32_t kSectionSystemInfo = 0x02007319u;
constexpr std::uint32_t kSectionProjectConfig = 0x01007319u;
constexpr std::uint32_t kSectionResource = 0x04007319u;
constexpr std::uint32_t kSectionCode = 0x03007319u;
constexpr std::uint32_t kSectionLosable = 0x05007319u;
constexpr std::uint32_t kSectionInitEc = 0x08007319u;
constexpr std::uint32_t kSectionEditorInfo = 0x09007319u;
constexpr std::uint32_t kSectionEventIndices = 0x0A007319u;
constexpr std::uint32_t kSectionEPackageInfo = 0x0D007319u;
constexpr std::uint32_t kSectionClassPublicity = 0x0B007319u;
constexpr std::uint32_t kSectionEcDependencies = 0x0C007319u;
constexpr std::uint32_t kSectionFolder = 0x0E007319u;

constexpr std::array<std::uint8_t, 4> kSectionNameNoKey = { 25, 115, 0, 7 };

constexpr std::int32_t kProgramHeaderVersionFlag1 = 66279;
constexpr std::int32_t kProgramHeaderUnk1 = 51113791;

constexpr std::uint8_t kConstTypeEmpty = 22;
constexpr std::uint8_t kConstTypeNumber = 23;
constexpr std::uint8_t kConstTypeBool = 24;
constexpr std::uint8_t kConstTypeDate = 25;
constexpr std::uint8_t kConstTypeText = 26;

constexpr std::int16_t kVarAttrStatic = 0x0001;
constexpr std::int16_t kVarAttrByRef = 0x0002;
constexpr std::int16_t kVarAttrNullable = 0x0004;
constexpr std::int16_t kVarAttrArray = 0x0008;
constexpr std::int16_t kConstAttrPublic = 0x0002;
constexpr std::int16_t kConstAttrHidden = 0x0004;
constexpr std::int16_t kConstAttrLongText = 0x0010;
constexpr std::int16_t kGlobalAttrPublic = 0x0100;
constexpr std::int16_t kGlobalAttrHidden = 0x0200;

constexpr std::int32_t kConstPageValue = 1;
constexpr std::int32_t kConstPageImage = 2;
constexpr std::int32_t kConstPageSound = 3;

struct RestoreDependencyInfo {
	struct DefinedIdRange {
		std::int32_t start = 0;
		std::int32_t count = 0;
	};

	std::string name;
	std::string fileName;
	std::string guid;
	std::string versionText;
	std::string path;
	std::string resolvedPath;
	std::string localWorkspace;
	bool reExport = false;
	bool isSupportLibrary = false;
	std::vector<DefinedIdRange> definedIds;
	std::vector<NativeDependencyClassSymbol> nativeClasses;
	std::vector<NativeDependencyMethodSymbol> nativeMethods;
	std::int32_t childIdStart = 0;
	std::int32_t childIdEnd = 0;
};

struct RestoreVariable {
	std::int32_t id = 0;
	std::int32_t dataType = 0;
	std::int16_t attr = 0;
	std::vector<std::int32_t> arrayBounds;
	std::string name;
	std::string comment;
};

struct RestoreMethod {
	std::int32_t id = 0;
	std::int32_t memoryAddress = 0;
	std::int32_t ownerClass = 0;
	std::int32_t attr = 0;
	std::int32_t returnType = 0;
	std::string name;
	std::string comment;
	std::vector<RestoreVariable> params;
	std::vector<RestoreVariable> locals;
	std::vector<std::uint8_t> lineOffset;
	std::vector<std::uint8_t> blockOffset;
	std::vector<std::uint8_t> methodReference;
	std::vector<std::uint8_t> variableReference;
	std::vector<std::uint8_t> constantReference;
	std::vector<std::uint8_t> expressionData;
};

struct RestoreClass {
	std::int32_t id = 0;
	std::int32_t memoryAddress = 0;
	std::int32_t formId = 0;
	std::int32_t baseClass = -1;
	std::string name;
	std::string comment;
	std::vector<std::int32_t> functionIds;
	std::vector<RestoreVariable> vars;
	bool isFormClass = false;
	bool isPublic = false;
	bool isHidden = false;
};

struct RestoreStruct {
	std::int32_t id = 0;
	std::int32_t memoryAddress = 0;
	std::int32_t attr = 0;
	std::string name;
	std::string comment;
	std::vector<RestoreVariable> members;
	bool isPlaceholder = false;
};

struct RestoreDll {
	std::int32_t id = 0;
	std::int32_t memoryAddress = 0;
	std::int32_t attr = 0;
	std::int32_t returnType = 0;
	std::string name;
	std::string comment;
	std::string fileName;
	std::string commandName;
	std::vector<RestoreVariable> params;
};

struct RestoreConstant {
	std::int32_t id = 0;
	std::int16_t attr = 0;
	std::int32_t pageType = kConstPageValue;
	std::string name;
	std::string comment;
	std::string valueText;
	std::vector<std::uint8_t> rawData;
};

struct RestoreFormElement {
	std::int32_t id = 0;
	std::int32_t dataType = 0;
	bool isMenu = false;
	std::string name;
	bool visible = true;
	bool disable = false;

	std::string comment;
	std::int32_t cWndAddress = 0;
	std::int32_t left = 0;
	std::int32_t top = 0;
	std::int32_t width = 0;
	std::int32_t height = 0;
	std::int32_t unknownBeforeParent = 0;
	std::int32_t parent = 0;
	std::vector<std::int32_t> children;
	std::vector<std::uint8_t> cursor;
	std::string tag;
	std::int32_t unknownBeforeVisible = 0;
	bool tabStop = true;
	bool locked = false;
	std::int32_t tabIndex = 0;
	std::vector<std::pair<std::int32_t, std::int32_t>> events;
	std::vector<std::uint8_t> extensionData;

	std::int32_t hotKey = 0;
	std::int32_t level = 0;
	bool selected = false;
	std::string text;
	std::int32_t clickEvent = 0;
};

struct RestoreForm {
	std::int32_t id = 0;
	std::int32_t memoryAddress = 0;
	std::int32_t unknown1 = 0;
	std::int32_t classId = 0;
	std::string name;
	std::string comment;
	std::vector<RestoreFormElement> elements;
};

struct RestoreFolder {
	std::int32_t key = 0;
	std::int32_t parentKey = 0;
	bool expand = true;
	std::string name;
	std::vector<std::int32_t> children;
};

struct RestoreDocumentModel {
	std::string sourcePath;
	std::string projectName;
	std::string versionText;
	std::vector<RestoreDependencyInfo> dependencies;
	std::vector<RestoreClass> classes;
	std::vector<RestoreMethod> methods;
	std::vector<RestoreVariable> globals;
	std::vector<RestoreStruct> structs;
	std::vector<RestoreDll> dlls;
	std::vector<RestoreConstant> constants;
	std::vector<RestoreForm> forms;
	std::int32_t folderAllocatedKey = 0;
	std::vector<RestoreFolder> folders;
};

namespace epl_system_id {

constexpr std::int32_t kTypeMethod = 0x04000000;
constexpr std::int32_t kTypeGlobal = 0x05000000;
constexpr std::int32_t kIdNaV = 0x0500FFFE;
constexpr std::int32_t kTypeFormSelf = 0x06000000;
constexpr std::int32_t kTypeStaticClass = 0x09000000;
constexpr std::int32_t kTypeDll = 0x0A000000;
constexpr std::int32_t kTypeClassMember = 0x15000000;
constexpr std::int32_t kTypeFormControl = 0x16000000;
constexpr std::int32_t kTypeConstant = 0x18000000;
constexpr std::int32_t kTypeFormClass = 0x19000000;
constexpr std::int32_t kTypeLocal = 0x25000000;
constexpr std::int32_t kTypeImageResource = 0x28000000;
constexpr std::int32_t kTypeFormMenu = 0x26000000;
constexpr std::int32_t kTypeStructMember = 0x35000000;
constexpr std::int32_t kTypeSoundResource = 0x38000000;
constexpr std::int32_t kTypeStruct = 0x41000000;
constexpr std::int32_t kTypeDllParameter = 0x45000000;
constexpr std::int32_t kTypeClass = 0x49000000;
constexpr std::int32_t kTypeForm = 0x52000000;

constexpr std::int32_t kMaskType = static_cast<std::int32_t>(0xFF000000u);
constexpr std::int32_t kMaskNum = 0x00FFFFFF;

inline std::int32_t GetType(const std::int32_t id)
{
	return id & kMaskType;
}

inline bool IsLibDataType(const std::int32_t id)
{
	return (id & kMaskType) == 0 && id != 0;
}

}  // namespace epl_system_id

class ByteWriter {
public:
	void WriteU8(const std::uint8_t value)
	{
		m_bytes.push_back(value);
	}

	void WriteI16(const std::int16_t value)
	{
		WritePod(value);
	}

	void WriteU16(const std::uint16_t value)
	{
		WritePod(value);
	}

	void WriteI32(const std::int32_t value)
	{
		WritePod(value);
	}

	void WriteU32(const std::uint32_t value)
	{
		WritePod(value);
	}

	void WriteI64(const std::int64_t value)
	{
		WritePod(value);
	}

	void WriteDouble(const double value)
	{
		WritePod(value);
	}

	void WriteBool32(const bool value)
	{
		WriteI32(value ? 1 : 0);
	}

	void WriteRaw(const void* data, const size_t size)
	{
		if (size == 0) {
			return;
		}
		const auto* begin = static_cast<const std::uint8_t*>(data);
		m_bytes.insert(m_bytes.end(), begin, begin + static_cast<std::ptrdiff_t>(size));
	}

	void WriteBytes(const std::vector<std::uint8_t>& data)
	{
		if (!data.empty()) {
			m_bytes.insert(m_bytes.end(), data.begin(), data.end());
		}
	}

	void WriteDynamicBytes(const std::vector<std::uint8_t>& data)
	{
		WriteI32(static_cast<std::int32_t>(data.size()));
		WriteBytes(data);
	}

	void WriteDynamicText(const std::string& text)
	{
		WriteI32(static_cast<std::int32_t>(text.size()));
		WriteRaw(text.data(), text.size());
	}

	void WriteStandardText(const std::string& text)
	{
		WriteRaw(text.data(), text.size());
		WriteU8(0);
	}

	void WriteBStr(const std::optional<std::string>& text)
	{
		if (!text.has_value()) {
			WriteI32(0);
			return;
		}
		WriteI32(static_cast<std::int32_t>(text->size() + 1));
		WriteRaw(text->data(), text->size());
		WriteU8(0);
	}

	void WriteTextArray(const std::vector<std::string>& values)
	{
		WriteI16(static_cast<std::int16_t>(values.size()));
		for (const auto& value : values) {
			WriteDynamicText(value);
		}
	}

	size_t position() const
	{
		return m_bytes.size();
	}

	void PatchI32(const size_t offset, const std::int32_t value)
	{
		if (offset + sizeof(value) > m_bytes.size()) {
			return;
		}
		std::memcpy(m_bytes.data() + offset, &value, sizeof(value));
	}

	std::vector<std::uint8_t> TakeBytes()
	{
		return std::move(m_bytes);
	}

	const std::vector<std::uint8_t>& bytes() const
	{
		return m_bytes;
	}

private:
	template <typename T>
	void WritePod(const T value)
	{
		const auto* begin = reinterpret_cast<const std::uint8_t*>(&value);
		m_bytes.insert(m_bytes.end(), begin, begin + sizeof(T));
	}

	std::vector<std::uint8_t> m_bytes;
};

std::string TrimAsciiCopy(std::string text)
{
	size_t begin = 0;
	while (begin < text.size() && static_cast<unsigned char>(text[begin]) <= 0x20) {
		++begin;
	}

	size_t end = text.size();
	while (end > begin && static_cast<unsigned char>(text[end - 1]) <= 0x20) {
		--end;
	}
	return text.substr(begin, end - begin);
}

bool StartsWith(const std::string_view text, const std::string_view prefix)
{
	return text.size() >= prefix.size() && text.substr(0, prefix.size()) == prefix;
}

bool EndsWith(const std::string_view text, const std::string_view suffix)
{
	return text.size() >= suffix.size() &&
		text.substr(text.size() - suffix.size(), suffix.size()) == suffix;
}

bool ReadFileBytes(const std::string& path, std::vector<std::uint8_t>& outBytes)
{
	outBytes.clear();
	std::ifstream in(Utf8PathToPath(path), std::ios::binary);
	if (!in.is_open()) {
		return false;
	}

	in.seekg(0, std::ios::end);
	const std::streamoff size = in.tellg();
	if (size < 0) {
		return false;
	}
	in.seekg(0, std::ios::beg);

	outBytes.resize(static_cast<size_t>(size));
	in.read(reinterpret_cast<char*>(outBytes.data()), size);
	return in.good() || static_cast<size_t>(in.gcount()) == outBytes.size();
}

std::vector<std::string> SplitLines(const std::string& text)
{
	std::vector<std::string> lines;
	size_t start = 0;
	size_t index = 0;
	while (index < text.size()) {
		if (text[index] == '\r' || text[index] == '\n') {
			lines.push_back(text.substr(start, index - start));
			if (text[index] == '\r' && index + 1 < text.size() && text[index + 1] == '\n') {
				++index;
			}
			start = index + 1;
		}
		++index;
	}
	lines.push_back(text.substr(start));
	return lines;
}

std::string RemoveUtf8Bom(const std::string& text)
{
	if (text.size() >= 3 &&
		static_cast<unsigned char>(text[0]) == 0xEF &&
		static_cast<unsigned char>(text[1]) == 0xBB &&
		static_cast<unsigned char>(text[2]) == 0xBF) {
		return text.substr(3);
	}
	return text;
}

std::vector<std::string> SplitTopLevelCommaFields(const std::string& text)
{
	std::vector<std::string> fields;
	std::string current;
	bool inQuote = false;
	for (size_t i = 0; i < text.size(); ++i) {
		const char ch = text[i];
		if (ch == '"') {
			inQuote = !inQuote;
			current.push_back(ch);
			continue;
		}
		if (ch == ',' && !inQuote) {
			fields.push_back(TrimAsciiCopy(current));
			current.clear();
			continue;
		}
		current.push_back(ch);
	}
	fields.push_back(TrimAsciiCopy(current));
	return fields;
}

std::string Unquote(const std::string& text)
{
	if (text.size() >= 2 && text.front() == '"' && text.back() == '"') {
		return text.substr(1, text.size() - 2);
	}
	return text;
}

constexpr const char* kTextLiteralLeftQuote = "“";
constexpr const char* kTextLiteralRightQuote = "”";
constexpr const char* kEscapedTextLiteralPrefix = "#e2txt_text#";
constexpr const char* kEscapedLongTextLiteralPrefix = "#e2txt_long_text#";
constexpr const char* kEscapedBodyLinePrefix = "#e2txt_body_line#";

std::string StripExpectedIndent(const std::string& line, const int expectedIndent);

int ParseHexNibble(const char ch)
{
	if (ch >= '0' && ch <= '9') {
		return ch - '0';
	}
	if (ch >= 'a' && ch <= 'f') {
		return ch - 'a' + 10;
	}
	if (ch >= 'A' && ch <= 'F') {
		return ch - 'A' + 10;
	}
	return -1;
}

std::string UnescapeTextLiteralPayload(const std::string& text)
{
	std::string out;
	out.reserve(text.size());
	for (size_t i = 0; i < text.size(); ++i) {
		if (text[i] != '\\') {
			out.push_back(text[i]);
			continue;
		}
		if (i + 1 >= text.size()) {
			out.push_back('\\');
			break;
		}

		const char next = text[++i];
		switch (next) {
		case '\\':
			out.push_back('\\');
			break;
		case 'r':
			out.push_back('\r');
			break;
		case 'n':
			out.push_back('\n');
			break;
		case 't':
			out.push_back('\t');
			break;
		case 'x':
			if (i + 2 < text.size()) {
				const int high = ParseHexNibble(text[i + 1]);
				const int low = ParseHexNibble(text[i + 2]);
				if (high >= 0 && low >= 0) {
					out.push_back(static_cast<char>((high << 4) | low));
					i += 2;
					break;
				}
			}
			out += "\\x";
			break;
		default:
			out.push_back(next);
			break;
		}
	}
	return out;
}

bool TryParseInt32(const std::string& text, std::int32_t& outValue)
{
	const std::string trimmed = TrimAsciiCopy(text);
	if (trimmed.empty()) {
		return false;
	}
	const char* begin = trimmed.data();
	const char* end = trimmed.data() + trimmed.size();
	const auto [ptr, ec] = std::from_chars(begin, end, outValue);
	return ec == std::errc() && ptr == end;
}

bool TryParseDouble(const std::string& text, double& outValue)
{
	const std::string trimmed = TrimAsciiCopy(text);
	if (trimmed.empty()) {
		return false;
	}

	char* end = nullptr;
	outValue = std::strtod(trimmed.c_str(), &end);
	return end != nullptr && *end == '\0';
}

std::optional<bool> ParseBoolLiteral(const std::string& text)
{
	const std::string trimmed = TrimAsciiCopy(text);
	if (trimmed == "真" || trimmed == "true" || trimmed == "TRUE") {
		return true;
	}
	if (trimmed == "假" || trimmed == "false" || trimmed == "FALSE") {
		return false;
	}
	return std::nullopt;
}

std::string StripWrappedText(const std::string& text, const std::string& left, const std::string& right)
{
	if (text.size() < left.size() + right.size()) {
		return text;
	}
	if (!StartsWith(text, left) || !EndsWith(text, right)) {
		return text;
	}
	return text.substr(left.size(), text.size() - left.size() - right.size());
}

bool TryDecodeDumpTextLiteral(const std::string& valueText, std::string& outRawText, bool& outIsLongText)
{
	outRawText.clear();
	outIsLongText = false;
	if (!StartsWith(valueText, kTextLiteralLeftQuote) || !EndsWith(valueText, kTextLiteralRightQuote)) {
		return false;
	}

	const std::string payload = StripWrappedText(valueText, kTextLiteralLeftQuote, kTextLiteralRightQuote);
	if (StartsWith(payload, kEscapedLongTextLiteralPrefix)) {
		outIsLongText = true;
		outRawText = UnescapeTextLiteralPayload(payload.substr(std::strlen(kEscapedLongTextLiteralPrefix)));
		return true;
	}
	if (StartsWith(payload, kEscapedTextLiteralPrefix)) {
		outRawText = UnescapeTextLiteralPayload(payload.substr(std::strlen(kEscapedTextLiteralPrefix)));
		return true;
	}

	outRawText = payload;
	return true;
}

bool TryDecodeLegacyUnterminatedDumpTextLiteral(const std::string& valueText, std::string& outRawText, bool& outIsLongText)
{
	outRawText.clear();
	outIsLongText = false;
	if (!StartsWith(valueText, kTextLiteralLeftQuote) || EndsWith(valueText, kTextLiteralRightQuote)) {
		return false;
	}

	// 兼容历史导出里少了结尾右引号的占位文本。
	const std::string payload = valueText.substr(std::strlen(kTextLiteralLeftQuote));
	if (StartsWith(payload, kEscapedLongTextLiteralPrefix)) {
		outIsLongText = true;
		outRawText = UnescapeTextLiteralPayload(payload.substr(std::strlen(kEscapedLongTextLiteralPrefix)));
		return true;
	}
	if (StartsWith(payload, kEscapedTextLiteralPrefix)) {
		outRawText = UnescapeTextLiteralPayload(payload.substr(std::strlen(kEscapedTextLiteralPrefix)));
		return true;
	}
	return false;
}

bool TryDecodeEscapedBodyLine(const std::string& text, std::string& outBody)
{
	outBody.clear();

	std::string encodedText;
	bool masked = false;
	if (StartsWith(text, std::string("' ") + kEscapedBodyLinePrefix)) {
		masked = true;
		encodedText = text.substr(2 + std::strlen(kEscapedBodyLinePrefix));
	}
	else if (StartsWith(text, kEscapedBodyLinePrefix)) {
		encodedText = text.substr(std::strlen(kEscapedBodyLinePrefix));
	}
	else {
		return false;
	}

	bool isLongText = false;
	if (!TryDecodeDumpTextLiteral(encodedText, outBody, isLongText)) {
		if (!TryDecodeLegacyUnterminatedDumpTextLiteral(encodedText, outBody, isLongText)) {
			outBody.clear();
			return false;
		}
	}
	if (masked) {
		outBody = "' " + outBody;
	}
	return true;
}

std::string DecodeEscapedBodyLineForIndent(const std::string& line, const int expectedIndent)
{
	const std::string stripped = StripExpectedIndent(line, expectedIndent);
	std::string decodedBody;
	if (!TryDecodeEscapedBodyLine(stripped, decodedBody)) {
		return line;
	}
	return std::string(static_cast<size_t>((std::max)(expectedIndent, 0) * 4), ' ') + decodedBody;
}

std::vector<std::uint8_t> DecodeBase64(const std::string& text)
{
	static constexpr std::array<std::uint8_t, 256> kMap = [] {
		std::array<std::uint8_t, 256> map = {};
		map.fill(0xFFu);
		for (std::uint8_t i = 0; i < 26; ++i) {
			map[static_cast<unsigned char>('A' + i)] = i;
			map[static_cast<unsigned char>('a' + i)] = static_cast<std::uint8_t>(26 + i);
		}
		for (std::uint8_t i = 0; i < 10; ++i) {
			map[static_cast<unsigned char>('0' + i)] = static_cast<std::uint8_t>(52 + i);
		}
		map[static_cast<unsigned char>('+')] = 62;
		map[static_cast<unsigned char>('/')] = 63;
		return map;
	}();

	std::vector<std::uint8_t> out;
	std::array<std::uint8_t, 4> chunk = {};
	int chunkSize = 0;
	int padding = 0;
	for (const unsigned char ch : text) {
		if (std::isspace(ch) != 0) {
			continue;
		}
		if (ch == '=') {
			chunk[chunkSize++] = 0;
			++padding;
		}
		else {
			const std::uint8_t value = kMap[ch];
			if (value == 0xFFu) {
				return {};
			}
			chunk[chunkSize++] = value;
		}

		if (chunkSize == 4) {
			const std::uint32_t bits =
				(static_cast<std::uint32_t>(chunk[0]) << 18) |
				(static_cast<std::uint32_t>(chunk[1]) << 12) |
				(static_cast<std::uint32_t>(chunk[2]) << 6) |
				static_cast<std::uint32_t>(chunk[3]);
			out.push_back(static_cast<std::uint8_t>((bits >> 16) & 0xFFu));
			if (padding < 2) {
				out.push_back(static_cast<std::uint8_t>((bits >> 8) & 0xFFu));
			}
			if (padding == 0) {
				out.push_back(static_cast<std::uint8_t>(bits & 0xFFu));
			}
			chunkSize = 0;
			padding = 0;
		}
	}
	return out;
}

std::string DecodeXmlEntities(const std::string& text)
{
	std::string out;
	out.reserve(text.size());
	for (size_t i = 0; i < text.size(); ++i) {
		if (text[i] != '&') {
			out.push_back(text[i]);
			continue;
		}
		if (StartsWith(std::string_view(text).substr(i), "&amp;")) {
			out.push_back('&');
			i += 4;
		}
		else if (StartsWith(std::string_view(text).substr(i), "&lt;")) {
			out.push_back('<');
			i += 3;
		}
		else if (StartsWith(std::string_view(text).substr(i), "&gt;")) {
			out.push_back('>');
			i += 3;
		}
		else if (StartsWith(std::string_view(text).substr(i), "&quot;")) {
			out.push_back('"');
			i += 5;
		}
		else if (StartsWith(std::string_view(text).substr(i), "&apos;")) {
			out.push_back('\'');
			i += 5;
		}
		else {
			out.push_back(text[i]);
		}
	}
	return out;
}

struct DumpBlock {
	enum class Kind {
		Dependencies,
		Page,
		FormXml,
	};

	Kind kind = Kind::Page;
	std::string pageType;
	std::string name;
	std::vector<std::string> lines;
};

bool ParseHeaderValueLine(
	const std::string& line,
	const std::string& key,
	std::string& outValue)
{
	const std::string prefix = key + "=";
	if (!StartsWith(line, prefix)) {
		return false;
	}
	outValue = line.substr(prefix.size());
	return true;
}

std::string ExtractNamedSegment(
	const std::string& text,
	const std::string& key,
	const std::optional<std::string>& nextKey)
{
	const std::string marker = key + "=";
	const size_t begin = text.find(marker);
	if (begin == std::string::npos) {
		return std::string();
	}

	const size_t valueBegin = begin + marker.size();
	size_t valueEnd = text.size();
	if (nextKey.has_value()) {
		const size_t nextPos = text.find(" " + *nextKey + "=", valueBegin);
		if (nextPos != std::string::npos) {
			valueEnd = nextPos;
		}
	}
	return text.substr(valueBegin, valueEnd - valueBegin);
}

bool ParseDumpBlocks(const std::vector<std::string>& lines, std::vector<DumpBlock>& outBlocks, std::string* outError)
{
	outBlocks.clear();
	size_t index = 0;
	while (index < lines.size()) {
		if (lines[index] != "================================================================================") {
			++index;
			continue;
		}
		++index;
		if (index >= lines.size()) {
			break;
		}

		DumpBlock block;
		const std::string& titleLine = lines[index++];
		if (titleLine == "[Dependencies]") {
			block.kind = DumpBlock::Kind::Dependencies;
		}
		else if (StartsWith(titleLine, "[Form XML] ")) {
			block.kind = DumpBlock::Kind::FormXml;
			block.name = titleLine.substr(std::string("[Form XML] ").size());
		}
		else if (!titleLine.empty() && titleLine.front() == '[') {
			const size_t typePos = titleLine.find("] type=");
			const size_t namePos = titleLine.find(" name=");
			if (typePos == std::string::npos || namePos == std::string::npos || namePos <= typePos + 7) {
				if (outError != nullptr) {
					*outError = "dump_page_header_invalid";
				}
				return false;
			}
			block.kind = DumpBlock::Kind::Page;
			block.pageType = titleLine.substr(typePos + 7, namePos - (typePos + 7));
			block.name = titleLine.substr(namePos + 6);
		}
		else {
			if (outError != nullptr) {
				*outError = "dump_block_header_invalid";
			}
			return false;
		}

		if (index < lines.size() && lines[index] == "--------------------------------------------------------------------------------") {
			++index;
		}
		while (index < lines.size() && lines[index] != "================================================================================") {
			block.lines.push_back(lines[index++]);
		}
		outBlocks.push_back(std::move(block));
	}
	return true;
}

bool IsReadablePageProtection(const DWORD protect)
{
	if ((protect & PAGE_GUARD) != 0 || (protect & PAGE_NOACCESS) != 0) {
		return false;
	}

	switch (protect & 0xFFu) {
	case PAGE_READONLY:
	case PAGE_READWRITE:
	case PAGE_WRITECOPY:
	case PAGE_EXECUTE_READ:
	case PAGE_EXECUTE_READWRITE:
	case PAGE_EXECUTE_WRITECOPY:
		return true;
	default:
		return false;
	}
}

bool IsReadableMemoryRange(const void* address, size_t size)
{
	if (address == nullptr) {
		return false;
	}
	if (size == 0) {
		return true;
	}

	const auto* current = static_cast<const std::uint8_t*>(address);
	size_t remaining = size;
	while (remaining > 0) {
		MEMORY_BASIC_INFORMATION mbi = {};
		if (VirtualQuery(current, &mbi, sizeof(mbi)) != sizeof(mbi)) {
			return false;
		}
		if (mbi.State != MEM_COMMIT || !IsReadablePageProtection(mbi.Protect)) {
			return false;
		}

		const auto* regionBase = static_cast<const std::uint8_t*>(mbi.BaseAddress);
		const size_t offset = static_cast<size_t>(current - regionBase);
		if (offset >= mbi.RegionSize) {
			return false;
		}

		const size_t available = mbi.RegionSize - offset;
		if (available >= remaining) {
			return true;
		}

		current += available;
		remaining -= available;
	}

	return true;
}

size_t GetSafeCStringLength(const char* text, const size_t maxLength)
{
	if (text == nullptr) {
		return 0;
	}

#if defined(_MSC_VER)
	size_t length = 0;
	__try {
		for (; length < maxLength; ++length) {
			if (text[length] == '\0') {
				break;
			}
		}
	}
	__except (EXCEPTION_EXECUTE_HANDLER) {
		return static_cast<size_t>(-1);
	}
	return length;
#else
	size_t length = 0;
	for (; length < maxLength; ++length) {
		if (text[length] == '\0') {
			break;
		}
	}
	return length;
#endif
}

const LIB_INFO* CallGetLibInfoSafely(const PFN_GET_LIB_INFO getInfoProc)
{
#if defined(_MSC_VER)
	__try {
		return getInfoProc == nullptr ? nullptr : getInfoProc();
	}
	__except (EXCEPTION_EXECUTE_HANDLER) {
		return nullptr;
	}
#else
	return getInfoProc == nullptr ? nullptr : getInfoProc();
#endif
}

std::string ReadSupportLibraryName(const char* text)
{
	constexpr size_t kMaxSupportLibraryStringLength = 4096;
	const size_t length = GetSafeCStringLength(text, kMaxSupportLibraryStringLength);
	if (length == static_cast<size_t>(-1)) {
		return std::string();
	}
	return std::string(text, length);
}

std::vector<std::string> BuildSupportTypeMemberNames(const LIB_DATA_TYPE_INFO& dataType)
{
	constexpr int kMaxSupportLibraryArrayCount = 16384;
	std::vector<std::string> memberNames;
	if (dataType.m_nPropertyCount > 0 &&
		dataType.m_nPropertyCount <= kMaxSupportLibraryArrayCount &&
		dataType.m_pPropertyBegin != nullptr &&
		IsReadableMemoryRange(
			dataType.m_pPropertyBegin,
			sizeof(UNIT_PROPERTY) * static_cast<size_t>(dataType.m_nPropertyCount))) {
		memberNames.reserve(static_cast<size_t>(dataType.m_nPropertyCount));
		for (int propertyIndex = 0; propertyIndex < dataType.m_nPropertyCount; ++propertyIndex) {
			memberNames.emplace_back(ReadSupportLibraryName(dataType.m_pPropertyBegin[propertyIndex].m_szName));
		}
		return memberNames;
	}

	if (dataType.m_nElementCount > 0 &&
		dataType.m_nElementCount <= kMaxSupportLibraryArrayCount &&
		dataType.m_pElementBegin != nullptr &&
		IsReadableMemoryRange(
			dataType.m_pElementBegin,
			sizeof(LIB_DATA_TYPE_ELEMENT) * static_cast<size_t>(dataType.m_nElementCount))) {
		memberNames.reserve(static_cast<size_t>(dataType.m_nElementCount));
		for (int memberIndex = 0; memberIndex < dataType.m_nElementCount; ++memberIndex) {
			memberNames.emplace_back(ReadSupportLibraryName(dataType.m_pElementBegin[memberIndex].m_szName));
		}
	}
	return memberNames;
}

void PushUniqueCandidate(std::vector<std::filesystem::path>& candidates, const std::filesystem::path& candidate)
{
	if (candidate.empty()) {
		return;
	}

	const auto normalized = candidate.lexically_normal();
	for (const auto& item : candidates) {
		if (item.lexically_normal() == normalized) {
			return;
		}
	}
	candidates.push_back(normalized);
}

std::vector<std::filesystem::path> BuildSupportLibraryCandidatePaths(
	const std::string& sourcePath,
	const std::string& libraryFileName)
{
	std::vector<std::filesystem::path> candidates;
	if (libraryFileName.empty()) {
		return candidates;
	}

	std::filesystem::path filePath = std::filesystem::path(libraryFileName);
	if (!filePath.has_extension()) {
		filePath += ".fne";
	}

	if (filePath.is_absolute()) {
		PushUniqueCandidate(candidates, filePath);
		return candidates;
	}

	auto addBaseCandidates = [&](const std::filesystem::path& baseDir) {
		if (baseDir.empty()) {
			return;
		}
		PushUniqueCandidate(candidates, baseDir / filePath);
		PushUniqueCandidate(candidates, baseDir / "lib" / filePath);

		std::filesystem::path current = baseDir;
		while (!current.empty()) {
			PushUniqueCandidate(candidates, current / "lib" / filePath);
			if (current == current.root_path()) {
				break;
			}
			current = current.parent_path();
		}
	};

	std::error_code ec;
	if (!sourcePath.empty()) {
		addBaseCandidates(Utf8PathToPath(sourcePath).parent_path());
	}
	addBaseCandidates(std::filesystem::current_path(ec));
	addBaseCandidates(std::filesystem::path(GetBasePath()));
	for (const auto& registeredBaseDir : GetRegisteredEplOpenCommandBaseDirs()) {
		addBaseCandidates(registeredBaseDir);
	}
	return candidates;
}

bool IsCoreSupportLibraryFileName(const std::string& fileName)
{
	std::filesystem::path path(fileName);
	std::string normalized = TrimAsciiCopy(path.filename().string());
	if (normalized.empty()) {
		normalized = TrimAsciiCopy(fileName);
	}
	std::transform(
		normalized.begin(),
		normalized.end(),
		normalized.begin(),
		[](const unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
	if (normalized == "krnln" || normalized == "krnln.fne") {
		return true;
	}
	const std::filesystem::path normalizedPath(normalized);
	return normalizedPath.stem().string() == "krnln";
}

enum class CoreSupportLibraryIssue {
	NotFound,
	LoadFailed,
	SymbolReadFailed,
};

std::string BuildCoreSupportLibraryWarning(
	const CoreSupportLibraryIssue issue,
	const std::string& fileName,
	const std::string& candidatePath,
	const DWORD errorCode)
{
	const std::string displayName = fileName.empty() ? "krnln.fne" : fileName;
	std::ostringstream stream;
	switch (issue) {
	case CoreSupportLibraryIssue::NotFound:
		stream << Utf8Literal(u8"未找到核心支持库 ") << displayName
			<< Utf8Literal(u8"。它是所有易语言源码都必须引用的核心支持库。当前仍会继续处理，但支持库命令名可能退化为 _Lib0CmdXXX，AI 解读和部分回包场景会受影响。")
			<< Utf8Literal(u8"请检查易语言安装目录及其上级 lib，或注册表 E.Document\\Shell\\Open\\Command。");
		break;
	case CoreSupportLibraryIssue::LoadFailed:
		stream << Utf8Literal(u8"已找到核心支持库 ") << displayName;
		if (!candidatePath.empty()) {
			stream << Utf8Literal(u8"（") << candidatePath << Utf8Literal(u8"）");
		}
		stream << Utf8Literal(u8"，但加载失败");
		if (errorCode != ERROR_SUCCESS) {
			stream << Utf8Literal(u8"，Win32 错误=") << errorCode;
		}
		stream << Utf8Literal(u8"。");
		if (errorCode == ERROR_BAD_EXE_FORMAT) {
			stream << Utf8Literal(u8"这通常表示当前 e-packager 与支持库位数不匹配，例如 x64 的 e-packager 尝试加载 x86 的 krnln.fne。");
		}
		else {
			stream << Utf8Literal(u8"这通常表示文件损坏、依赖缺失，或当前进程无法直接加载该支持库。");
		}
		stream << Utf8Literal(u8"当前仍会继续处理，但支持库命令名可能退化为 _Lib0CmdXXX。");
		break;
	case CoreSupportLibraryIssue::SymbolReadFailed:
		stream << Utf8Literal(u8"已加载核心支持库 ") << displayName;
		if (!candidatePath.empty()) {
			stream << Utf8Literal(u8"（") << candidatePath << Utf8Literal(u8"）");
		}
		stream << Utf8Literal(u8"，但无法读取 GetLibInfo 导出的命令/类型符号。当前仍会继续处理，但支持库命令名可能退化为 _Lib0CmdXXX。");
		break;
	}
	return stream.str();
}

const std::vector<std::pair<std::string, std::int32_t>>& GetBuiltinTypes()
{
	static const std::vector<std::pair<std::string, std::int32_t>> kTypes = {
		{ "通用型", static_cast<std::int32_t>(0x80000000u) },
		{ "字节型", static_cast<std::int32_t>(0x80000101u) },
		{ "短整数型", static_cast<std::int32_t>(0x80000201u) },
		{ "整数型", static_cast<std::int32_t>(0x80000301u) },
		{ "长整数型", static_cast<std::int32_t>(0x80000401u) },
		{ "小数型", static_cast<std::int32_t>(0x80000501u) },
		{ "双精度小数型", static_cast<std::int32_t>(0x80000601u) },
		{ "逻辑型", static_cast<std::int32_t>(0x80000002u) },
		{ "日期时间型", static_cast<std::int32_t>(0x80000003u) },
		{ "文本型", static_cast<std::int32_t>(0x80000004u) },
		{ "字节集", static_cast<std::int32_t>(0x80000005u) },
		{ "子程序指针", static_cast<std::int32_t>(0x80000006u) },
		{ "条件语句型", static_cast<std::int32_t>(0x80000008u) },
		{ "窗口", 65537 },
		{ "菜单", 65539 },
		{ "字体", 65540 },
		{ "编辑框", 65541 },
		{ "图片框", 65542 },
		{ "外形框", 65543 },
		{ "画板", 65544 },
		{ "分组框", 65545 },
		{ "标签", 65546 },
		{ "按钮", 65547 },
		{ "选择框", 65548 },
		{ "单选框", 65549 },
		{ "组合框", 65550 },
		{ "列表框", 65551 },
		{ "选择列表框", 65552 },
		{ "横向滚动条", 65553 },
		{ "纵向滚动条", 65554 },
		{ "进度条", 65555 },
		{ "滑块条", 65556 },
		{ "选择夹", 65557 },
		{ "影像框", 65558 },
		{ "日期框", 65559 },
		{ "月历", 65560 },
		{ "驱动器框", 65561 },
		{ "目录框", 65562 },
		{ "文件框", 65563 },
		{ "颜色选择器", 65564 },
		{ "超级链接框", 65565 },
		{ "调节器", 65566 },
		{ "通用对话框", 65567 },
		{ "时钟", 65568 },
		{ "打印机", 65569 },
		{ "字段信息", 65570 },
		{ "数据报", 65572 },
		{ "客户", 65573 },
		{ "服务器", 65574 },
		{ "端口", 65575 },
		{ "打印设置信息", 65576 },
		{ "表格", 65577 },
		{ "数据源", 65578 },
		{ "通用提供者", 65579 },
		{ "数据库提供者", 65580 },
		{ "图形按钮", 65581 },
		{ "外部数据库", 65582 },
		{ "外部数据提供者", 65583 },
		{ "对象", 65584 },
		{ "变体型", 65585 },
		{ "变体类型", 65586 },
		{ "工具条", 196611 },
		{ "超级列表框", 196612 },
		{ "高级表格", 262145 },
	};
	return kTypes;
}

class IdAllocator {
public:
	std::int32_t Alloc(const std::int32_t typeMask)
	{
		return typeMask | (++m_next);
	}

	void Observe(const std::int32_t id)
	{
		m_next = (std::max)(m_next, id & epl_system_id::kMaskNum);
	}

private:
	std::int32_t m_next = 0xFFFF;
};

class SectionByteReader {
public:
	explicit SectionByteReader(const std::vector<std::uint8_t>& bytes)
		: m_bytes(bytes)
	{
	}

	bool ReadI32(std::int32_t& value)
	{
		if (m_pos + sizeof(value) > m_bytes.size()) {
			return false;
		}
		std::memcpy(&value, m_bytes.data() + m_pos, sizeof(value));
		m_pos += sizeof(value);
		return true;
	}

	bool ReadI64(std::int64_t& value)
	{
		if (m_pos + sizeof(value) > m_bytes.size()) {
			return false;
		}
		std::memcpy(&value, m_bytes.data() + m_pos, sizeof(value));
		m_pos += sizeof(value);
		return true;
	}

	bool ReadDynamicText(std::string& out)
	{
		std::int32_t size = 0;
		if (!ReadI32(size) || size < 0 || m_pos + static_cast<size_t>(size) > m_bytes.size()) {
			return false;
		}
		out.assign(reinterpret_cast<const char*>(m_bytes.data() + m_pos), static_cast<size_t>(size));
		m_pos += static_cast<size_t>(size);
		return true;
	}

	bool ReadInt32Array(std::vector<std::int32_t>& outValues)
	{
		outValues.clear();
		std::int32_t byteSize = 0;
		if (!ReadI32(byteSize) || byteSize < 0 || (byteSize % 4) != 0) {
			return false;
		}
		outValues.resize(static_cast<size_t>(byteSize / 4));
		for (auto& value : outValues) {
			if (!ReadI32(value)) {
				return false;
			}
		}
		return true;
	}

private:
	const std::vector<std::uint8_t>& m_bytes;
	size_t m_pos = 0;
};

struct OriginalEComDependencyRecord {
	std::string name;
	std::string path;
	bool reExport = false;
	std::vector<RestoreDependencyInfo::DefinedIdRange> definedIds;
};

bool ParseEComDependencySectionBytes(
	const std::vector<std::uint8_t>& bytes,
	std::vector<OriginalEComDependencyRecord>& outRecords)
{
	outRecords.clear();
	if (bytes.empty()) {
		return true;
	}

	SectionByteReader reader(bytes);
	std::int32_t dependencyCount = 0;
	if (!reader.ReadI32(dependencyCount) || dependencyCount < 0) {
		return false;
	}

	outRecords.reserve(static_cast<size_t>(dependencyCount));
	for (std::int32_t dependencyIndex = 0; dependencyIndex < dependencyCount; ++dependencyIndex) {
		std::int32_t infoVersion = 0;
		std::int32_t fileSize = 0;
		std::int64_t fileTime = 0;
		OriginalEComDependencyRecord record;
		if (!reader.ReadI32(infoVersion) ||
			infoVersion < 0 ||
			infoVersion > 2 ||
			!reader.ReadI32(fileSize) ||
			!reader.ReadI64(fileTime)) {
			return false;
		}
		if (infoVersion >= 2) {
			std::int32_t reExportValue = 0;
			if (!reader.ReadI32(reExportValue)) {
				return false;
			}
			record.reExport = reExportValue != 0;
		}
		if (!reader.ReadDynamicText(record.name) || !reader.ReadDynamicText(record.path)) {
			return false;
		}

		std::vector<std::int32_t> starts;
		std::vector<std::int32_t> counts;
		if (!reader.ReadInt32Array(starts) || !reader.ReadInt32Array(counts) || starts.size() != counts.size()) {
			return false;
		}
		record.definedIds.reserve(starts.size());
		for (size_t rangeIndex = 0; rangeIndex < starts.size(); ++rangeIndex) {
			if (counts[rangeIndex] <= 0) {
				continue;
			}
			record.definedIds.push_back(RestoreDependencyInfo::DefinedIdRange{
				starts[rangeIndex],
				counts[rangeIndex],
			});
		}
		outRecords.push_back(std::move(record));
	}
	return true;
}

std::string NormalizeDependencyMatchText(std::string text)
{
	text = TrimAsciiCopy(std::move(text));
	std::replace(text.begin(), text.end(), '/', '\\');
	std::transform(text.begin(), text.end(), text.begin(), [](const unsigned char ch) {
		return static_cast<char>(std::tolower(ch));
	});
	return text;
}

void ApplyNativeDependencyDefinedIds(
	const ProjectBundle& bundle,
	std::vector<RestoreDependencyInfo>& dependencies)
{
	if (bundle.nativeSourceBytes.empty()) {
		return;
	}

	std::vector<NativeDependencySymbolRecord> nativeRecords;
	std::string nativeError;
	if (ExtractNativeDependencySymbols(bundle.nativeSourceBytes, nativeRecords, &nativeError)) {
		std::vector<bool> used(nativeRecords.size(), false);
		for (auto& dependency : dependencies) {
			if (dependency.isSupportLibrary) {
				continue;
			}

			const std::string dependencyName = NormalizeDependencyMatchText(dependency.name);
			const std::string dependencyPath = NormalizeDependencyMatchText(dependency.path);
			size_t matchedIndex = nativeRecords.size();
			for (size_t index = 0; index < nativeRecords.size(); ++index) {
				if (used[index]) {
					continue;
				}
				const std::string recordName = NormalizeDependencyMatchText(nativeRecords[index].name);
				const std::string recordPath = NormalizeDependencyMatchText(nativeRecords[index].path);
				if ((!dependencyPath.empty() && dependencyPath == recordPath) ||
					(!dependencyName.empty() && dependencyName == recordName)) {
					matchedIndex = index;
					break;
				}
			}
			if (matchedIndex == nativeRecords.size()) {
				continue;
			}

			used[matchedIndex] = true;
			if (dependency.definedIds.empty()) {
				dependency.definedIds.reserve(nativeRecords[matchedIndex].definedIds.size());
				for (const auto& range : nativeRecords[matchedIndex].definedIds) {
					if (range.count > 0) {
						dependency.definedIds.push_back(RestoreDependencyInfo::DefinedIdRange{
							range.start,
							range.count,
						});
					}
				}
			}
			dependency.nativeClasses = nativeRecords[matchedIndex].classes;
			dependency.nativeMethods = nativeRecords[matchedIndex].methods;
		}
		return;
	}

	std::vector<NativeSectionSnapshot> snapshots;
	std::string ignoredError;
	if (!CaptureNativeSectionSnapshots(bundle.nativeSourceBytes, snapshots, &ignoredError)) {
		return;
	}

	const auto sectionIt = std::find_if(
		snapshots.begin(),
		snapshots.end(),
		[](const NativeSectionSnapshot& snapshot) { return snapshot.key == kSectionEcDependencies; });
	if (sectionIt == snapshots.end()) {
		return;
	}

	std::vector<OriginalEComDependencyRecord> records;
	if (!ParseEComDependencySectionBytes(sectionIt->data, records)) {
		return;
	}

	std::vector<bool> used(records.size(), false);
	for (auto& dependency : dependencies) {
		if (dependency.isSupportLibrary || !dependency.definedIds.empty()) {
			continue;
		}

		const std::string dependencyName = NormalizeDependencyMatchText(dependency.name);
		const std::string dependencyPath = NormalizeDependencyMatchText(dependency.path);
		size_t matchedIndex = records.size();
		for (size_t index = 0; index < records.size(); ++index) {
			if (used[index]) {
				continue;
			}
			const std::string recordName = NormalizeDependencyMatchText(records[index].name);
			const std::string recordPath = NormalizeDependencyMatchText(records[index].path);
			if ((!dependencyPath.empty() && dependencyPath == recordPath) ||
				(!dependencyName.empty() && dependencyName == recordName)) {
				matchedIndex = index;
				break;
			}
		}
		if (matchedIndex == records.size()) {
			continue;
		}

		used[matchedIndex] = true;
		dependency.definedIds = records[matchedIndex].definedIds;
	}
}

void AssignDependencyChildIdSpans(std::vector<RestoreDependencyInfo>& dependencies)
{
	struct Span {
		size_t dependencyIndex = 0;
		std::int32_t minTopIdNum = 0;
		std::int32_t maxTopIdNum = 0;
	};

	std::vector<Span> spans;
	for (size_t dependencyIndex = 0; dependencyIndex < dependencies.size(); ++dependencyIndex) {
		auto& dependency = dependencies[dependencyIndex];
		if (dependency.isSupportLibrary || dependency.definedIds.empty()) {
			continue;
		}

		std::int32_t minTopIdNum = (std::numeric_limits<std::int32_t>::max)();
		std::int32_t maxTopIdNum = 0;
		for (const auto& range : dependency.definedIds) {
			if (range.count <= 0) {
				continue;
			}
			const std::int32_t startNum = range.start & epl_system_id::kMaskNum;
			const std::int32_t endNum = startNum + range.count - 1;
			minTopIdNum = (std::min)(minTopIdNum, startNum);
			maxTopIdNum = (std::max)(maxTopIdNum, endNum);
		}
		if (minTopIdNum == (std::numeric_limits<std::int32_t>::max)()) {
			continue;
		}

		spans.push_back(Span{
			dependencyIndex,
			minTopIdNum,
			maxTopIdNum,
		});
	}

	std::sort(
		spans.begin(),
		spans.end(),
		[](const Span& left, const Span& right) {
			return left.minTopIdNum < right.minTopIdNum;
		});

	for (size_t index = 0; index < spans.size(); ++index) {
		auto& dependency = dependencies[spans[index].dependencyIndex];
		dependency.childIdStart = spans[index].maxTopIdNum + 1;
		dependency.childIdEnd = (std::numeric_limits<std::int32_t>::max)();
		if (index + 1 < spans.size() && spans[index + 1].minTopIdNum > dependency.childIdStart) {
			dependency.childIdEnd = spans[index + 1].minTopIdNum - 1;
		}
	}
}

class DependencyImportIdCursor {
public:
	explicit DependencyImportIdCursor(const RestoreDependencyInfo& dependency)
		: m_childNext(dependency.childIdStart)
		, m_childEnd(dependency.childIdEnd)
	{
		for (const auto& range : dependency.definedIds) {
			if (range.count <= 0) {
				continue;
			}
			m_hasOriginalRanges = true;
			auto& cursor = m_cursors[NormalizeTopLevelRangeType(epl_system_id::GetType(range.start))];
			cursor.ranges.push_back(range);
		}

		for (auto& [type, cursor] : m_cursors) {
			std::sort(
				cursor.ranges.begin(),
				cursor.ranges.end(),
				[](const RestoreDependencyInfo::DefinedIdRange& left, const RestoreDependencyInfo::DefinedIdRange& right) {
					return (left.start & epl_system_id::kMaskNum) < (right.start & epl_system_id::kMaskNum);
				});
		}
	}

	bool HasOriginalRanges() const
	{
		return m_hasOriginalRanges;
	}

	bool HasAvailable(const std::int32_t typeMask) const
	{
		const auto it = m_cursors.find(typeMask);
		if (it == m_cursors.end()) {
			return false;
		}
		const auto& cursor = it->second;
		if (cursor.rangeIndex >= cursor.ranges.size()) {
			return false;
		}
		return cursor.offset < cursor.ranges[cursor.rangeIndex].count;
	}

	void ObserveAll(IdAllocator& allocator) const
	{
		for (const auto& [type, cursor] : m_cursors) {
			for (const auto& range : cursor.ranges) {
				const std::int32_t endNum = (range.start & epl_system_id::kMaskNum) + range.count - 1;
				allocator.Observe(type | endNum);
			}
		}
	}

	std::int32_t AllocTopLevel(IdAllocator& allocator, const std::int32_t typeMask)
	{
		auto it = m_cursors.find(NormalizeTopLevelRangeType(typeMask));
		if (it != m_cursors.end()) {
			auto& cursor = it->second;
			while (cursor.rangeIndex < cursor.ranges.size()) {
				const auto& range = cursor.ranges[cursor.rangeIndex];
				if (cursor.offset < range.count) {
					const std::int32_t idNum = (range.start & epl_system_id::kMaskNum) + cursor.offset;
					++cursor.offset;
					const std::int32_t id = typeMask | idNum;
					if (MarkTopLevelUsed(typeMask, id)) {
						return id;
					}
					continue;
				}
				++cursor.rangeIndex;
				cursor.offset = 0;
			}
		}
		return allocator.Alloc(typeMask);
	}

	std::int32_t AllocTopLevel(IdAllocator& allocator, const std::int32_t typeMask, const std::int32_t preferredId)
	{
		if ((preferredId & epl_system_id::kMaskType) == typeMask &&
			IsInTopLevelRange(typeMask, preferredId) &&
			MarkTopLevelUsed(typeMask, preferredId)) {
			allocator.Observe(preferredId);
			return preferredId;
		}
		return AllocTopLevel(allocator, typeMask);
	}

	std::int32_t AllocChild(IdAllocator& allocator, const std::int32_t typeMask)
	{
		if (m_hasOriginalRanges && m_childNext > 0 && m_childNext <= m_childEnd) {
			const std::int32_t id = typeMask | m_childNext;
			++m_childNext;
			allocator.Observe(id);
			return id;
		}
		return allocator.Alloc(typeMask);
	}

	std::int32_t AllocChild(IdAllocator& allocator, const std::int32_t typeMask, const std::int32_t preferredId)
	{
		if ((preferredId & epl_system_id::kMaskType) == typeMask) {
			allocator.Observe(preferredId);
			return preferredId;
		}
		return AllocChild(allocator, typeMask);
	}

private:
	struct Cursor {
		std::vector<RestoreDependencyInfo::DefinedIdRange> ranges;
		size_t rangeIndex = 0;
		std::int32_t offset = 0;
	};

	bool IsInTopLevelRange(const std::int32_t typeMask, const std::int32_t id) const
	{
		const auto it = m_cursors.find(NormalizeTopLevelRangeType(typeMask));
		if (it == m_cursors.end()) {
			return !m_hasOriginalRanges;
		}

		const auto& cursor = it->second;
		const std::int32_t idNum = id & epl_system_id::kMaskNum;
		for (const auto& range : cursor.ranges) {
			const std::int32_t startNum = range.start & epl_system_id::kMaskNum;
			const std::int32_t endNum = startNum + range.count - 1;
			if (range.count > 0 && idNum >= startNum && idNum <= endNum) {
				return true;
			}
		}
		return false;
	}

	static std::int32_t NormalizeTopLevelRangeType(const std::int32_t typeMask)
	{
		if (typeMask == epl_system_id::kTypeClass ||
			typeMask == epl_system_id::kTypeStaticClass ||
			typeMask == epl_system_id::kTypeFormClass) {
			return epl_system_id::kTypeClass;
		}
		return typeMask;
	}

	static std::int64_t BuildUsedTopLevelKey(const std::int32_t typeMask, const std::int32_t id)
	{
		const std::uint32_t normalizedType =
			static_cast<std::uint32_t>(NormalizeTopLevelRangeType(typeMask));
		const std::uint32_t idNum = static_cast<std::uint32_t>(id & epl_system_id::kMaskNum);
		return (static_cast<std::int64_t>(normalizedType) << 32) | idNum;
	}

	bool MarkTopLevelUsed(const std::int32_t typeMask, const std::int32_t id)
	{
		return m_usedTopLevelSlots.insert(BuildUsedTopLevelKey(typeMask, id)).second;
	}

	bool m_hasOriginalRanges = false;
	std::int32_t m_childNext = 0;
	std::int32_t m_childEnd = 0;
	std::unordered_map<std::int32_t, Cursor> m_cursors;
	std::unordered_set<std::int64_t> m_usedTopLevelSlots;
};

struct SupportLibraryCommandInfo {
	std::int16_t libraryId = 0;
	std::int32_t commandId = 0;
};

struct SupportLibraryTypeInfo {
	std::int32_t typeId = 0;
	bool isTabControl = false;
	std::unordered_map<std::string, SupportLibraryCommandInfo> methodsByName;
};

class TypeResolver {
public:
	TypeResolver(const std::string& sourcePath, const std::vector<RestoreDependencyInfo>& dependencies)
		: m_sourcePath(sourcePath)
	{
		for (const auto& [name, value] : GetBuiltinTypes()) {
			m_builtinTypes.emplace(name, value);
		}

		int supportIndex = 1;
		for (const auto& dependency : dependencies) {
			if (!dependency.isSupportLibrary) {
				continue;
			}
			m_supportLibraryOrder.push_back(&dependency);
			LoadSupportLibrary(dependency, supportIndex++);
		}
		RegisterBuiltinSupportCommands();
	}

	void RegisterUserType(const std::string& name, const std::int32_t typeId)
	{
		const std::string key = NormalizeTypeName(name);
		if (!key.empty()) {
			m_userTypes[key] = typeId;
		}
	}

	void RegisterPlaceholderType(const std::string& name, const std::int32_t typeId)
	{
		RegisterUserType(name, typeId);
		m_placeholderTypeNames.insert(NormalizeTypeName(name));
	}

	bool IsPlaceholderType(const std::string& name) const
	{
		return m_placeholderTypeNames.contains(NormalizeTypeName(name));
	}

	std::int32_t ResolveTypeId(const std::string& rawTypeName) const
	{
		const std::string typeName = NormalizeTypeName(rawTypeName);
		if (typeName.empty()) {
			return 0;
		}
		if (const auto it = m_builtinTypes.find(typeName); it != m_builtinTypes.end()) {
			return it->second;
		}
		if (const auto it = m_userTypes.find(typeName); it != m_userTypes.end()) {
			return it->second;
		}
		if (const auto it = m_supportTypes.find(typeName); it != m_supportTypes.end()) {
			return it->second.typeId;
		}
		return 0;
	}

	bool IsTabControlType(const std::int32_t typeId) const
	{
		if (typeId == 65557) {
			return true;
		}
		for (const auto& [_, info] : m_supportTypes) {
			if (info.typeId == typeId) {
				return info.isTabControl;
			}
		}
		return false;
	}

	bool TryResolveSupportCommand(const std::string& rawCommandName, SupportLibraryCommandInfo& outInfo) const
	{
		const std::string commandName = NormalizeTypeName(rawCommandName);
		const auto it = m_supportCommands.find(commandName);
		if (it == m_supportCommands.end()) {
			outInfo = {};
			return false;
		}
		outInfo = it->second;
		return true;
	}

	bool TryResolveSupportTypeMethod(
		const std::int32_t typeId,
		const std::string& rawMethodName,
		SupportLibraryCommandInfo& outInfo) const
	{
		const std::string methodName = NormalizeTypeName(rawMethodName);
		if (methodName.empty()) {
			outInfo = {};
			return false;
		}
		if (typeId == 65537 && methodName == NormalizeTypeName("取窗口句柄")) {
			outInfo = SupportLibraryCommandInfo{ 0, 215 };
			return true;
		}
		for (const auto& [_, info] : m_supportTypes) {
			if (info.typeId != typeId) {
				continue;
			}
			const auto methodIt = info.methodsByName.find(methodName);
			if (methodIt == info.methodsByName.end()) {
				break;
			}
			outInfo = methodIt->second;
			return true;
		}
		outInfo = {};
		return false;
	}

	static std::string NormalizeTypeName(std::string value)
	{
		value = TrimAsciiCopy(std::move(value));
		if (value.size() >= 2 && value.front() == '<' && value.back() == '>') {
			value = TrimAsciiCopy(value.substr(1, value.size() - 2));
		}
		return value;
	}

private:
	void RegisterBuiltinSupportCommands()
	{
		const auto addCoreCommand = [this](const char* name, const std::int32_t commandId) {
			m_supportCommands.emplace(NormalizeTypeName(name), SupportLibraryCommandInfo{ 0, commandId });
		};

		// 核心支持库的基础命令在 x64 进程无法加载 x86 krnln.fne 时仍需要可回包。
		addCoreCommand("取运行目录", 65);
	}

	void LoadSupportLibrary(const RestoreDependencyInfo& dependency, const int supportIndex)
	{
		constexpr int kMaxSupportLibraryArrayCount = 16384;
		const bool isCoreSupportLibrary = IsCoreSupportLibraryFileName(dependency.fileName);
		const auto candidates = BuildSupportLibraryCandidatePaths(m_sourcePath, dependency.fileName);
		HMODULE module = nullptr;
		bool candidateExists = false;
		DWORD lastLoadError = ERROR_SUCCESS;
		std::string lastCandidatePath;
		for (const auto& path : candidates) {
			std::error_code ec;
			if (!std::filesystem::exists(path, ec)) {
				continue;
			}
			candidateExists = true;
			lastCandidatePath = PathToUtf8(path);
			module = LoadLibraryExA(path.string().c_str(), nullptr, 0);
			if (module != nullptr) {
				break;
			}
			lastLoadError = GetLastError();
		}
		if (module == nullptr) {
			if (isCoreSupportLibrary) {
				AddRuntimeWarning(BuildCoreSupportLibraryWarning(
					candidateExists ? CoreSupportLibraryIssue::LoadFailed : CoreSupportLibraryIssue::NotFound,
					dependency.fileName,
					lastCandidatePath,
					lastLoadError));
			}
			return;
		}

		const auto* getInfoProc = reinterpret_cast<PFN_GET_LIB_INFO>(GetProcAddress(module, FUNCNAME_GET_LIB_INFO));
		if (getInfoProc == nullptr) {
			if (isCoreSupportLibrary) {
				AddRuntimeWarning(BuildCoreSupportLibraryWarning(
					CoreSupportLibraryIssue::SymbolReadFailed,
					dependency.fileName,
					lastCandidatePath,
					ERROR_SUCCESS));
			}
			return;
		}

		const LIB_INFO* libInfo = CallGetLibInfoSafely(getInfoProc);
		if (libInfo == nullptr ||
			!IsReadableMemoryRange(libInfo, sizeof(LIB_INFO)) ||
			libInfo->m_nDataTypeCount <= 0 ||
			libInfo->m_nDataTypeCount > kMaxSupportLibraryArrayCount ||
			libInfo->m_pDataType == nullptr ||
			!IsReadableMemoryRange(
				libInfo->m_pDataType,
				sizeof(LIB_DATA_TYPE_INFO) * static_cast<size_t>(libInfo->m_nDataTypeCount))) {
			if (isCoreSupportLibrary) {
				AddRuntimeWarning(BuildCoreSupportLibraryWarning(
					CoreSupportLibraryIssue::SymbolReadFailed,
					dependency.fileName,
					lastCandidatePath,
					ERROR_SUCCESS));
			}
			return;
		}

		for (int i = 0; i < libInfo->m_nDataTypeCount; ++i) {
			const LIB_DATA_TYPE_INFO& dataType = libInfo->m_pDataType[i];
			SupportLibraryTypeInfo info;
			info.typeId = (supportIndex << 16) | (i + 1);
			info.isTabControl = (dataType.m_dwState & LDT_IS_TAB_UNIT) != 0;
			if (dataType.m_nCmdCount > 0 &&
				dataType.m_nCmdCount <= kMaxSupportLibraryArrayCount &&
				dataType.m_pnCmdsIndex != nullptr &&
				libInfo->m_pBeginCmdInfo != nullptr &&
				IsReadableMemoryRange(
					dataType.m_pnCmdsIndex,
					sizeof(int) * static_cast<size_t>(dataType.m_nCmdCount)) &&
				IsReadableMemoryRange(
					libInfo->m_pBeginCmdInfo,
					sizeof(CMD_INFO) * static_cast<size_t>(libInfo->m_nCmdCount))) {
				const auto libraryId = static_cast<std::int16_t>(supportIndex - 1);
				for (int cmdIndex = 0; cmdIndex < dataType.m_nCmdCount; ++cmdIndex) {
					const int globalCmdIndex = dataType.m_pnCmdsIndex[cmdIndex];
					if (globalCmdIndex < 0 || globalCmdIndex >= libInfo->m_nCmdCount) {
						continue;
					}
					const std::string methodName =
						NormalizeTypeName(ReadSupportLibraryName(libInfo->m_pBeginCmdInfo[globalCmdIndex].m_szName));
					if (!methodName.empty()) {
						info.methodsByName.insert_or_assign(
							methodName,
							SupportLibraryCommandInfo{ libraryId, globalCmdIndex });
					}
				}
			}
			const std::string name = NormalizeTypeName(ReadSupportLibraryName(dataType.m_szName));
			if (!name.empty()) {
				m_supportTypes.insert_or_assign(name, info);
			}
		}
		if (libInfo->m_nCmdCount > 0 &&
			libInfo->m_nCmdCount <= kMaxSupportLibraryArrayCount &&
			libInfo->m_pBeginCmdInfo != nullptr &&
			IsReadableMemoryRange(
				libInfo->m_pBeginCmdInfo,
				sizeof(CMD_INFO) * static_cast<size_t>(libInfo->m_nCmdCount))) {
			const auto libraryId = static_cast<std::int16_t>(supportIndex - 1);
			for (int i = 0; i < libInfo->m_nCmdCount; ++i) {
				const std::string name = NormalizeTypeName(ReadSupportLibraryName(libInfo->m_pBeginCmdInfo[i].m_szName));
				if (!name.empty()) {
					m_supportCommands.insert_or_assign(name, SupportLibraryCommandInfo{ libraryId, i });
				}
			}
		}
		// Intentionally keep support-library modules loaded for the rest of the process
		// lifetime. Some third-party .fne libraries corrupt the process heap during
		// DLL detach.
	}

	std::string m_sourcePath;
	std::unordered_map<std::string, std::int32_t> m_builtinTypes;
	std::unordered_map<std::string, std::int32_t> m_userTypes;
	std::unordered_map<std::string, SupportLibraryTypeInfo> m_supportTypes;
	std::unordered_map<std::string, SupportLibraryCommandInfo> m_supportCommands;
	std::unordered_set<std::string> m_placeholderTypeNames;
	std::vector<const RestoreDependencyInfo*> m_supportLibraryOrder;
};

std::optional<std::pair<std::string, std::string>> SplitFixedCodeComment(const std::string& text)
{
	const size_t pos = text.find("  ' ");
	if (pos == std::string::npos) {
		return std::make_pair(text, std::string());
	}
	return std::make_pair(text.substr(0, pos), text.substr(pos + 4));
}

struct BodyStatement;

struct BodySwitchCase {
	bool mask = false;
	std::string code;
	std::vector<BodyStatement> block;
};

enum class BodyStatementKind {
	Raw,
	IfTrue,
	IfElse,
	WhileLoop,
	DoWhileLoop,
	CounterLoop,
	ForLoop,
	SwitchBlock,
};

struct BodyStatement {
	BodyStatementKind kind = BodyStatementKind::Raw;
	bool mask = false;
	bool maskOnEnd = false;
	std::string code;
	std::string fixedComment;
	std::string fixedEndComment;
	std::string endCode;
	std::vector<BodyStatement> block;
	std::vector<BodyStatement> elseBlock;
	std::vector<BodySwitchCase> cases;
	std::vector<BodyStatement> defaultBlock;
};

int CountIndentLevel(const std::string& line)
{
	size_t count = 0;
	while (count < line.size() && line[count] == ' ') {
		++count;
	}
	return static_cast<int>(count / 4);
}

std::string StripIndent(const std::string& line)
{
	size_t index = 0;
	while (index < line.size() && line[index] == ' ') {
		++index;
	}
	return line.substr(index);
}

std::string StripExpectedIndent(const std::string& line, const int expectedIndent)
{
	size_t index = 0;
	size_t remain = static_cast<size_t>((std::max)(expectedIndent, 0) * 4);
	while (index < line.size() && remain > 0 && line[index] == ' ') {
		++index;
		--remain;
	}
	return line.substr(index);
}

std::string TrimRightAsciiCopy(std::string text)
{
	while (!text.empty() && (text.back() == ' ' || text.back() == '\t')) {
		text.pop_back();
	}
	return text;
}

bool ExtractMaskPrefix(const std::string& line, bool& outMask, std::string& outCode)
{
	outMask = false;
	outCode = StripIndent(line);
	if (StartsWith(outCode, "' ")) {
		outMask = true;
		outCode.erase(0, 2);
	}
	return true;
}

bool ExtractMaskPrefixPreserveIndent(const std::string& line, bool& outMask, std::string& outCode)
{
	outMask = false;
	outCode = line;
	size_t indent = 0;
	while (indent < outCode.size() && outCode[indent] == ' ') {
		++indent;
	}
	if (outCode.compare(indent, 2, "' ") == 0) {
		outMask = true;
		outCode.erase(indent, 2);
	}
	return true;
}

bool IsBlankLine(const std::string& line)
{
	return TrimAsciiCopy(line).empty();
}

bool StartsWithControl(const std::string& line, const std::string& token)
{
	return StartsWith(TrimAsciiCopy(line), token);
}

bool MatchesBodyTokenBoundary(const std::string& code, const std::string_view token)
{
	if (!StartsWith(code, token)) {
		return false;
	}
	if (code.size() == token.size()) {
		return true;
	}
	const char next = code[token.size()];
	return next == ' ' || next == '(';
}

bool MatchesBodyEndToken(const std::string& code, const std::unordered_set<std::string>& endTokens)
{
	for (const auto& token : endTokens) {
		if (!token.empty() && token.back() == '*') {
			if (MatchesBodyTokenBoundary(code, std::string_view(token).substr(0, token.size() - 1))) {
				return true;
			}
			continue;
		}
		if (MatchesBodyTokenBoundary(code, token)) {
			return true;
		}
	}
	return false;
}

class MethodCodeWriter {
public:
	void BeginBlock(const std::uint8_t type)
	{
		m_blockOffset.WriteU8(type);
		m_blockOffset.WriteI32(static_cast<std::int32_t>(m_expressionData.position()));
		m_blockStack.push_back(m_blockOffset.position());
		m_blockOffset.WriteI32(0);
	}

	void EndBlock()
	{
		if (m_blockStack.empty()) {
			return;
		}
		const size_t patchPos = m_blockStack.back();
		m_blockStack.pop_back();
		m_blockOffset.PatchI32(patchPos, static_cast<std::int32_t>(m_expressionData.position()));
	}

	void WriteRawStatement(const bool mask, const std::string& code)
	{
		const auto offset = static_cast<std::int32_t>(m_expressionData.position());
		m_lineOffset.WriteI32(offset);
		if (LooksLikeObjectMethodCallReference(code)) {
			m_methodReference.WriteI32(offset);
			m_variableReference.WriteI32(offset);
		}
		WriteUnexaminedCall(0x6A, -1, 0, mask, code);
	}

	void WriteNativeExpressionStatement(
		const std::vector<std::uint8_t>& data,
		const std::vector<std::int32_t>& methodReferences = {},
		const std::vector<std::int32_t>& variableReferences = {},
		const std::vector<std::int32_t>& constantReferences = {})
	{
		const auto offset = static_cast<std::int32_t>(m_expressionData.position());
		m_lineOffset.WriteI32(offset);
		for (const auto reference : methodReferences) {
			m_methodReference.WriteI32(offset + reference);
		}
		for (const auto reference : variableReferences) {
			m_variableReference.WriteI32(offset + reference);
		}
		for (const auto reference : constantReferences) {
			m_constantReference.WriteI32(offset + reference);
		}
		m_expressionData.WriteBytes(data);
	}

	void WriteIfTrue(const BodyStatement& statement)
	{
		BeginBlock(2);
		m_lineOffset.WriteI32(static_cast<std::int32_t>(m_expressionData.position()));
		WriteUnexaminedCall(0x6C, 0, 1, statement.mask, statement.code);
		WriteBlock(statement.block);
		m_expressionData.WriteU8(0x52);
		EndBlock();
		m_expressionData.WriteU8(0x73);
	}

	void WriteIfElse(const BodyStatement& statement)
	{
		BeginBlock(1);
		m_lineOffset.WriteI32(static_cast<std::int32_t>(m_expressionData.position()));
		WriteUnexaminedCall(0x6B, 0, 0, statement.mask, statement.code);
		WriteBlock(statement.block);
		m_expressionData.WriteU8(0x50);
		WriteBlock(statement.elseBlock);
		m_expressionData.WriteU8(0x51);
		EndBlock();
		m_expressionData.WriteU8(0x72);
	}

	void WriteWhile(const BodyStatement& statement)
	{
		BeginBlock(3);
		m_lineOffset.WriteI32(static_cast<std::int32_t>(m_expressionData.position()));
		WriteUnexaminedCall(0x70, 0, 3, statement.mask, statement.code);
		WriteBlock(statement.block);
		m_expressionData.WriteU8(0x55);
		EndBlock();
		WriteFixedCall(0x71, 0, 4, statement.maskOnEnd, statement.fixedEndComment);
	}

	void WriteDoWhile(const BodyStatement& statement)
	{
		BeginBlock(3);
		WriteFixedCall(0x70, 0, 5, statement.mask, statement.fixedComment);
		WriteBlock(statement.block);
		m_expressionData.WriteU8(0x55);
		EndBlock();
		m_lineOffset.WriteI32(static_cast<std::int32_t>(m_expressionData.position()));
		WriteUnexaminedCall(0x71, 0, 6, statement.maskOnEnd, statement.endCode);
	}

	void WriteCounter(const BodyStatement& statement)
	{
		BeginBlock(3);
		m_lineOffset.WriteI32(static_cast<std::int32_t>(m_expressionData.position()));
		WriteUnexaminedCall(0x70, 0, 7, statement.mask, statement.code);
		WriteBlock(statement.block);
		m_expressionData.WriteU8(0x55);
		EndBlock();
		WriteFixedCall(0x71, 0, 8, statement.maskOnEnd, statement.fixedEndComment);
	}

	void WriteFor(const BodyStatement& statement)
	{
		BeginBlock(3);
		m_lineOffset.WriteI32(static_cast<std::int32_t>(m_expressionData.position()));
		WriteUnexaminedCall(0x70, 0, 9, statement.mask, statement.code);
		WriteBlock(statement.block);
		m_expressionData.WriteU8(0x55);
		EndBlock();
		WriteFixedCall(0x71, 0, 10, statement.maskOnEnd, statement.fixedEndComment);
	}

	void WriteSwitch(const BodyStatement& statement)
	{
		BeginBlock(4);
		m_expressionData.WriteU8(0x6D);
		for (const auto& caseItem : statement.cases) {
			m_lineOffset.WriteI32(static_cast<std::int32_t>(m_expressionData.position()));
			WriteUnexaminedCall(0x6E, 0, 2, caseItem.mask, caseItem.code);
			WriteBlock(caseItem.block);
			m_expressionData.WriteU8(0x53);
		}
		m_expressionData.WriteU8(0x6F);
		WriteBlock(statement.defaultBlock);
		m_expressionData.WriteU8(0x54);
		EndBlock();
		m_expressionData.WriteU8(0x74);
	}

	void WriteBlock(const std::vector<BodyStatement>& statements)
	{
		for (const auto& statement : statements) {
			switch (statement.kind) {
			case BodyStatementKind::Raw:
				WriteRawStatement(statement.mask, statement.code);
				break;
			case BodyStatementKind::IfTrue:
				WriteIfTrue(statement);
				break;
			case BodyStatementKind::IfElse:
				WriteIfElse(statement);
				break;
			case BodyStatementKind::WhileLoop:
				WriteWhile(statement);
				break;
			case BodyStatementKind::DoWhileLoop:
				WriteDoWhile(statement);
				break;
			case BodyStatementKind::CounterLoop:
				WriteCounter(statement);
				break;
			case BodyStatementKind::ForLoop:
				WriteFor(statement);
				break;
			case BodyStatementKind::SwitchBlock:
				WriteSwitch(statement);
				break;
			}
		}
	}

	template <typename RawWriter>
	void WriteBlockWithRawHandler(const std::vector<BodyStatement>& statements, RawWriter&& writeRaw)
	{
		for (const auto& statement : statements) {
			switch (statement.kind) {
			case BodyStatementKind::Raw:
				writeRaw(statement);
				break;
			case BodyStatementKind::IfTrue:
				WriteIfTrueWithRawHandler(statement, writeRaw);
				break;
			case BodyStatementKind::IfElse:
				WriteIfElseWithRawHandler(statement, writeRaw);
				break;
			case BodyStatementKind::WhileLoop:
				WriteWhileWithRawHandler(statement, writeRaw);
				break;
			case BodyStatementKind::DoWhileLoop:
				WriteDoWhileWithRawHandler(statement, writeRaw);
				break;
			case BodyStatementKind::CounterLoop:
				WriteCounterWithRawHandler(statement, writeRaw);
				break;
			case BodyStatementKind::ForLoop:
				WriteForWithRawHandler(statement, writeRaw);
				break;
			case BodyStatementKind::SwitchBlock:
				WriteSwitchWithRawHandler(statement, writeRaw);
				break;
			}
		}
	}

	std::vector<std::uint8_t> TakeLineOffset() { return m_lineOffset.TakeBytes(); }
	std::vector<std::uint8_t> TakeBlockOffset() { return m_blockOffset.TakeBytes(); }
	std::vector<std::uint8_t> TakeMethodReference() { return m_methodReference.TakeBytes(); }
	std::vector<std::uint8_t> TakeVariableReference() { return m_variableReference.TakeBytes(); }
	std::vector<std::uint8_t> TakeConstantReference() { return m_constantReference.TakeBytes(); }
	std::vector<std::uint8_t> TakeExpressionData() { return m_expressionData.TakeBytes(); }

private:
	template <typename RawWriter>
	void WriteIfTrueWithRawHandler(const BodyStatement& statement, RawWriter& writeRaw)
	{
		BeginBlock(2);
		m_lineOffset.WriteI32(static_cast<std::int32_t>(m_expressionData.position()));
		WriteUnexaminedCall(0x6C, 0, 1, statement.mask, statement.code);
		WriteBlockWithRawHandler(statement.block, writeRaw);
		m_expressionData.WriteU8(0x52);
		EndBlock();
		m_expressionData.WriteU8(0x73);
	}

	template <typename RawWriter>
	void WriteIfElseWithRawHandler(const BodyStatement& statement, RawWriter& writeRaw)
	{
		BeginBlock(1);
		m_lineOffset.WriteI32(static_cast<std::int32_t>(m_expressionData.position()));
		WriteUnexaminedCall(0x6B, 0, 0, statement.mask, statement.code);
		WriteBlockWithRawHandler(statement.block, writeRaw);
		m_expressionData.WriteU8(0x50);
		WriteBlockWithRawHandler(statement.elseBlock, writeRaw);
		m_expressionData.WriteU8(0x51);
		EndBlock();
		m_expressionData.WriteU8(0x72);
	}

	template <typename RawWriter>
	void WriteWhileWithRawHandler(const BodyStatement& statement, RawWriter& writeRaw)
	{
		BeginBlock(3);
		m_lineOffset.WriteI32(static_cast<std::int32_t>(m_expressionData.position()));
		WriteUnexaminedCall(0x70, 0, 3, statement.mask, statement.code);
		WriteBlockWithRawHandler(statement.block, writeRaw);
		m_expressionData.WriteU8(0x55);
		EndBlock();
		WriteFixedCall(0x71, 0, 4, statement.maskOnEnd, statement.fixedEndComment);
	}

	template <typename RawWriter>
	void WriteDoWhileWithRawHandler(const BodyStatement& statement, RawWriter& writeRaw)
	{
		BeginBlock(3);
		WriteFixedCall(0x70, 0, 5, statement.mask, statement.fixedComment);
		WriteBlockWithRawHandler(statement.block, writeRaw);
		m_expressionData.WriteU8(0x55);
		EndBlock();
		m_lineOffset.WriteI32(static_cast<std::int32_t>(m_expressionData.position()));
		WriteUnexaminedCall(0x71, 0, 6, statement.maskOnEnd, statement.endCode);
	}

	template <typename RawWriter>
	void WriteCounterWithRawHandler(const BodyStatement& statement, RawWriter& writeRaw)
	{
		BeginBlock(3);
		m_lineOffset.WriteI32(static_cast<std::int32_t>(m_expressionData.position()));
		WriteUnexaminedCall(0x70, 0, 7, statement.mask, statement.code);
		WriteBlockWithRawHandler(statement.block, writeRaw);
		m_expressionData.WriteU8(0x55);
		EndBlock();
		WriteFixedCall(0x71, 0, 8, statement.maskOnEnd, statement.fixedEndComment);
	}

	template <typename RawWriter>
	void WriteForWithRawHandler(const BodyStatement& statement, RawWriter& writeRaw)
	{
		BeginBlock(3);
		m_lineOffset.WriteI32(static_cast<std::int32_t>(m_expressionData.position()));
		WriteUnexaminedCall(0x70, 0, 9, statement.mask, statement.code);
		WriteBlockWithRawHandler(statement.block, writeRaw);
		m_expressionData.WriteU8(0x55);
		EndBlock();
		WriteFixedCall(0x71, 0, 10, statement.maskOnEnd, statement.fixedEndComment);
	}

	template <typename RawWriter>
	void WriteSwitchWithRawHandler(const BodyStatement& statement, RawWriter& writeRaw)
	{
		BeginBlock(4);
		m_expressionData.WriteU8(0x6D);
		for (const auto& caseItem : statement.cases) {
			m_lineOffset.WriteI32(static_cast<std::int32_t>(m_expressionData.position()));
			WriteUnexaminedCall(0x6E, 0, 2, caseItem.mask, caseItem.code);
			WriteBlockWithRawHandler(caseItem.block, writeRaw);
			m_expressionData.WriteU8(0x53);
		}
		m_expressionData.WriteU8(0x6F);
		WriteBlockWithRawHandler(statement.defaultBlock, writeRaw);
		m_expressionData.WriteU8(0x54);
		EndBlock();
		m_expressionData.WriteU8(0x74);
	}

	void WriteFixedCall(
		const std::uint8_t type,
		const std::int16_t libraryId,
		const std::int32_t methodId,
		const bool mask,
		const std::string& comment)
	{
		m_lineOffset.WriteI32(static_cast<std::int32_t>(m_expressionData.position()));
		m_expressionData.WriteU8(type);
		m_expressionData.WriteI32(methodId);
		m_expressionData.WriteI16(libraryId);
		m_expressionData.WriteI16(static_cast<std::int16_t>(mask ? 0x20 : 0));
		m_expressionData.WriteBStr(std::nullopt);
		m_expressionData.WriteBStr(comment.empty() ? std::nullopt : std::make_optional(comment));
		m_expressionData.WriteU8(0x36);
		m_expressionData.WriteU8(0x01);
	}

	void WriteUnexaminedCall(
		const std::uint8_t type,
		const std::int16_t libraryId,
		const std::int32_t methodId,
		const bool mask,
		const std::string& code)
	{
		m_expressionData.WriteU8(type);
		m_expressionData.WriteI32(methodId);
		m_expressionData.WriteI16(libraryId);
		m_expressionData.WriteI16(static_cast<std::int16_t>(mask ? 0x20 : 0x40));
		m_expressionData.WriteBStr(std::make_optional(code));
		m_expressionData.WriteBStr(std::nullopt);
		m_expressionData.WriteU8(0x36);
		m_expressionData.WriteU8(0x01);
	}

	ByteWriter m_lineOffset;
	ByteWriter m_blockOffset;
	ByteWriter m_methodReference;
	ByteWriter m_variableReference;
	ByteWriter m_constantReference;
	ByteWriter m_expressionData;
	std::vector<size_t> m_blockStack;

	static bool LooksLikeObjectMethodCallReference(const std::string& code)
	{
		const std::string trimmed = TrimAsciiCopy(code);
		if (trimmed.empty() || trimmed.front() == '\'') {
			return false;
		}
		const size_t callPos = trimmed.find('(');
		if (callPos == std::string::npos) {
			return false;
		}
		const size_t dotPos = trimmed.rfind('.', callPos);
		return dotPos != std::string::npos && dotPos + 1 < callPos;
	}
};

size_t NormalizeErrorLineIndex(const size_t preferredIndex, const size_t lineCount);

bool ParseBodyBlock(
	const std::vector<std::string>& lines,
	size_t& index,
	const int expectedIndent,
	const std::unordered_set<std::string>& endTokens,
	std::vector<BodyStatement>& outStatements,
	std::string* outError,
	size_t* outErrorLineIndex);

bool ParseSwitchBlock(
	const std::vector<std::string>& lines,
	size_t& index,
	const int indent,
	const bool firstMask,
	const std::string& firstCaseCode,
	std::vector<BodyStatement>& outStatements,
	std::string* outError,
	size_t* outErrorLineIndex)
{
	BodyStatement statement;
	statement.kind = BodyStatementKind::SwitchBlock;
	statement.cases.push_back(BodySwitchCase{ firstMask, firstCaseCode, {} });

	if (!ParseBodyBlock(
			lines,
			index,
			indent + 1,
			{ ".判断*", ".默认", ".判断结束" },
			statement.cases.back().block,
			outError,
			outErrorLineIndex)) {
		return false;
	}

	while (index < lines.size()) {
		if (IsBlankLine(lines[index])) {
			++index;
			continue;
		}

		bool mask = false;
		std::string code;
		const std::string effectiveLine = DecodeEscapedBodyLineForIndent(lines[index], indent);
		ExtractMaskPrefix(effectiveLine, mask, code);
		code = TrimAsciiCopy(code);
		if (!StartsWith(code, ".")) {
			if (outError != nullptr) {
				*outError = "switch_marker_invalid";
			}
			if (outErrorLineIndex != nullptr) {
				*outErrorLineIndex = NormalizeErrorLineIndex(index, lines.size());
			}
			return false;
		}
		if (code == ".默认") {
			++index;
			break;
		}
		if (!StartsWith(code, ".判断")) {
			if (outError != nullptr) {
				*outError = "switch_case_missing";
			}
			if (outErrorLineIndex != nullptr) {
				*outErrorLineIndex = NormalizeErrorLineIndex(index, lines.size());
			}
			return false;
		}

		BodySwitchCase nextCase;
		nextCase.mask = mask;
		nextCase.code = code.substr(1);
		++index;
		if (!ParseBodyBlock(
				lines,
				index,
				indent + 1,
				{ ".判断*", ".默认", ".判断结束" },
				nextCase.block,
				outError,
				outErrorLineIndex)) {
			return false;
		}
		statement.cases.push_back(std::move(nextCase));
	}

	if (!ParseBodyBlock(
			lines,
			index,
			indent + 1,
			{ ".判断结束" },
			statement.defaultBlock,
			outError,
			outErrorLineIndex)) {
		return false;
	}
	if (index >= lines.size()) {
		if (outError != nullptr) {
			*outError = "switch_end_missing";
		}
		if (outErrorLineIndex != nullptr) {
			*outErrorLineIndex = lines.empty() ? 0 : (lines.size() - 1);
		}
		return false;
	}

	bool endMask = false;
	std::string endCode;
	const std::string effectiveEndLine = DecodeEscapedBodyLineForIndent(lines[index], indent);
	ExtractMaskPrefix(effectiveEndLine, endMask, endCode);
	endCode = TrimAsciiCopy(endCode);
	if (endCode != ".判断结束") {
		if (outError != nullptr) {
			*outError = "switch_end_invalid";
		}
		if (outErrorLineIndex != nullptr) {
			*outErrorLineIndex = NormalizeErrorLineIndex(index, lines.size());
		}
		return false;
	}
	++index;
	outStatements.push_back(std::move(statement));
	return true;
}

bool ParseBodyBlock(
	const std::vector<std::string>& lines,
	size_t& index,
	const int expectedIndent,
	const std::unordered_set<std::string>& endTokens,
	std::vector<BodyStatement>& outStatements,
	std::string* outError,
	size_t* outErrorLineIndex)
{
	outStatements.clear();
	while (index < lines.size()) {
		const std::string& rawLine = lines[index];
		if (IsBlankLine(rawLine)) {
			++index;
			continue;
		}
		const int currentIndent = CountIndentLevel(rawLine);
		if (currentIndent < expectedIndent) {
			break;
		}

		const std::string effectiveLine = DecodeEscapedBodyLineForIndent(rawLine, expectedIndent);
		bool mask = false;
		std::string code;
		ExtractMaskPrefix(effectiveLine, mask, code);
		const std::string trimmedCode = TrimAsciiCopy(code);
		if (!StartsWith(trimmedCode, ".")) {
			bool rawMask = false;
			std::string rawCode;
			ExtractMaskPrefixPreserveIndent(StripExpectedIndent(effectiveLine, expectedIndent), rawMask, rawCode);
			outStatements.push_back(BodyStatement{ BodyStatementKind::Raw, rawMask, false, TrimRightAsciiCopy(rawCode) });
			++index;
			continue;
		}
		code = trimmedCode;
		if (currentIndent == expectedIndent && MatchesBodyEndToken(code, endTokens)) {
			break;
		}

		if (StartsWith(code, ".如果真 ")) {
			BodyStatement statement;
			statement.kind = BodyStatementKind::IfTrue;
			statement.mask = mask;
			statement.code = code.substr(1);
			++index;
			if (!ParseBodyBlock(lines, index, expectedIndent + 1, { ".如果真结束" }, statement.block, outError, outErrorLineIndex)) {
				return false;
			}
			if (index >= lines.size()) {
				if (outError != nullptr) {
					*outError = "if_true_end_missing";
				}
				if (outErrorLineIndex != nullptr) {
					*outErrorLineIndex = lines.empty() ? 0 : (lines.size() - 1);
				}
				return false;
			}
			bool endMask = false;
			std::string endCode;
			ExtractMaskPrefix(lines[index], endMask, endCode);
			if (TrimAsciiCopy(endCode) != ".如果真结束") {
				if (outError != nullptr) {
					*outError = "if_true_end_invalid";
				}
				if (outErrorLineIndex != nullptr) {
					*outErrorLineIndex = NormalizeErrorLineIndex(index, lines.size());
				}
				return false;
			}
			++index;
			outStatements.push_back(std::move(statement));
			continue;
		}

		if (StartsWith(code, ".如果 ")) {
			BodyStatement statement;
			statement.kind = BodyStatementKind::IfElse;
			statement.mask = mask;
			statement.code = code.substr(1);
			++index;
			if (!ParseBodyBlock(lines, index, expectedIndent + 1, { ".否则" }, statement.block, outError, outErrorLineIndex)) {
				return false;
			}
			if (index >= lines.size()) {
				if (outError != nullptr) {
					*outError = "if_else_marker_missing";
				}
				if (outErrorLineIndex != nullptr) {
					*outErrorLineIndex = lines.empty() ? 0 : (lines.size() - 1);
				}
				return false;
			}
			bool elseMask = false;
			std::string elseCode;
			ExtractMaskPrefix(lines[index], elseMask, elseCode);
			if (TrimAsciiCopy(elseCode) != ".否则") {
				if (outError != nullptr) {
					*outError = "if_else_invalid";
				}
				if (outErrorLineIndex != nullptr) {
					*outErrorLineIndex = NormalizeErrorLineIndex(index, lines.size());
				}
				return false;
			}
			++index;
			if (!ParseBodyBlock(lines, index, expectedIndent + 1, { ".如果结束" }, statement.elseBlock, outError, outErrorLineIndex)) {
				return false;
			}
			if (index >= lines.size()) {
				if (outError != nullptr) {
					*outError = "if_end_missing";
				}
				if (outErrorLineIndex != nullptr) {
					*outErrorLineIndex = lines.empty() ? 0 : (lines.size() - 1);
				}
				return false;
			}
			bool endMask = false;
			std::string endCode;
			ExtractMaskPrefix(lines[index], endMask, endCode);
			if (TrimAsciiCopy(endCode) != ".如果结束") {
				if (outError != nullptr) {
					*outError = "if_end_invalid";
				}
				if (outErrorLineIndex != nullptr) {
					*outErrorLineIndex = NormalizeErrorLineIndex(index, lines.size());
				}
				return false;
			}
			++index;
			outStatements.push_back(std::move(statement));
			continue;
		}

		if (StartsWith(code, ".判断循环首 ")) {
			BodyStatement statement;
			statement.kind = BodyStatementKind::WhileLoop;
			statement.mask = mask;
			statement.code = code.substr(1);
			++index;
			if (!ParseBodyBlock(lines, index, expectedIndent + 1, { ".判断循环尾 ()" }, statement.block, outError, outErrorLineIndex)) {
				return false;
			}
			if (index >= lines.size()) {
				if (outError != nullptr) {
					*outError = "while_end_missing";
				}
				if (outErrorLineIndex != nullptr) {
					*outErrorLineIndex = lines.empty() ? 0 : (lines.size() - 1);
				}
				return false;
			}
			bool endMask = false;
			std::string endCode;
			ExtractMaskPrefix(lines[index], endMask, endCode);
			const auto split = SplitFixedCodeComment(TrimAsciiCopy(endCode));
			if (!split.has_value() || split->first != ".判断循环尾 ()") {
				if (outError != nullptr) {
					*outError = "while_end_invalid";
				}
				if (outErrorLineIndex != nullptr) {
					*outErrorLineIndex = NormalizeErrorLineIndex(index, lines.size());
				}
				return false;
			}
			statement.maskOnEnd = endMask;
			statement.fixedEndComment = split->second;
			++index;
			outStatements.push_back(std::move(statement));
			continue;
		}

		if (StartsWith(code, ".循环判断首 ()")) {
			BodyStatement statement;
			statement.kind = BodyStatementKind::DoWhileLoop;
			statement.mask = mask;
			const auto split = SplitFixedCodeComment(code);
			statement.fixedComment = split.has_value() ? split->second : std::string();
			++index;
			if (!ParseBodyBlock(lines, index, expectedIndent + 1, { ".循环判断尾*" }, statement.block, outError, outErrorLineIndex)) {
				return false;
			}
			if (index >= lines.size()) {
				if (outError != nullptr) {
					*outError = "do_while_end_missing";
				}
				if (outErrorLineIndex != nullptr) {
					*outErrorLineIndex = lines.empty() ? 0 : (lines.size() - 1);
				}
				return false;
			}
			bool endMask = false;
			std::string endCode;
			ExtractMaskPrefix(lines[index], endMask, endCode);
			statement.maskOnEnd = endMask;
			const auto endSplit = SplitFixedCodeComment(TrimAsciiCopy(endCode));
			if (!endSplit.has_value() || !StartsWith(endSplit->first, ".循环判断尾")) {
				if (outError != nullptr) {
					*outError = "do_while_end_invalid";
				}
				if (outErrorLineIndex != nullptr) {
					*outErrorLineIndex = NormalizeErrorLineIndex(index, lines.size());
				}
				return false;
			}
			statement.endCode = endSplit->first.substr(1);
			if (statement.fixedComment.empty()) {
				statement.fixedComment = endSplit->second;
			}
			++index;
			outStatements.push_back(std::move(statement));
			continue;
		}

		if (StartsWith(code, ".计次循环首 ")) {
			BodyStatement statement;
			statement.kind = BodyStatementKind::CounterLoop;
			statement.mask = mask;
			statement.code = code.substr(1);
			++index;
			if (!ParseBodyBlock(lines, index, expectedIndent + 1, { ".计次循环尾 ()" }, statement.block, outError, outErrorLineIndex)) {
				return false;
			}
			if (index >= lines.size()) {
				if (outError != nullptr) {
					*outError = "counter_end_missing";
				}
				if (outErrorLineIndex != nullptr) {
					*outErrorLineIndex = lines.empty() ? 0 : (lines.size() - 1);
				}
				return false;
			}
			bool endMask = false;
			std::string endCode;
			ExtractMaskPrefix(lines[index], endMask, endCode);
			const auto split = SplitFixedCodeComment(TrimAsciiCopy(endCode));
			if (!split.has_value() || split->first != ".计次循环尾 ()") {
				if (outError != nullptr) {
					*outError = "counter_end_invalid";
				}
				if (outErrorLineIndex != nullptr) {
					*outErrorLineIndex = NormalizeErrorLineIndex(index, lines.size());
				}
				return false;
			}
			statement.maskOnEnd = endMask;
			statement.fixedEndComment = split->second;
			++index;
			outStatements.push_back(std::move(statement));
			continue;
		}

		if (StartsWith(code, ".变量循环首 ")) {
			BodyStatement statement;
			statement.kind = BodyStatementKind::ForLoop;
			statement.mask = mask;
			statement.code = code.substr(1);
			++index;
			if (!ParseBodyBlock(lines, index, expectedIndent + 1, { ".变量循环尾 ()" }, statement.block, outError, outErrorLineIndex)) {
				return false;
			}
			if (index >= lines.size()) {
				if (outError != nullptr) {
					*outError = "for_end_missing";
				}
				if (outErrorLineIndex != nullptr) {
					*outErrorLineIndex = lines.empty() ? 0 : (lines.size() - 1);
				}
				return false;
			}
			bool endMask = false;
			std::string endCode;
			ExtractMaskPrefix(lines[index], endMask, endCode);
			const auto split = SplitFixedCodeComment(TrimAsciiCopy(endCode));
			if (!split.has_value() || split->first != ".变量循环尾 ()") {
				if (outError != nullptr) {
					*outError = "for_end_invalid";
				}
				if (outErrorLineIndex != nullptr) {
					*outErrorLineIndex = NormalizeErrorLineIndex(index, lines.size());
				}
				return false;
			}
			statement.maskOnEnd = endMask;
			statement.fixedEndComment = split->second;
			++index;
			outStatements.push_back(std::move(statement));
			continue;
		}

		if (StartsWith(code, ".判断开始")) {
			++index;
			std::string firstCaseCode = "判断" + code.substr(std::string(".判断开始").size());
			if (!ParseSwitchBlock(lines, index, expectedIndent, mask, firstCaseCode, outStatements, outError, outErrorLineIndex)) {
				return false;
			}
			continue;
		}

		outStatements.push_back(BodyStatement{ BodyStatementKind::Raw, mask, false, code });
		++index;
	}
	return true;
}

bool BuildMethodCodeData(
	const std::vector<std::string>& lines,
	RestoreMethod& outMethod,
	std::string* outError,
	size_t* outErrorLineIndex)
{
	std::vector<BodyStatement> statements;
	size_t index = 0;
	if (!ParseBodyBlock(lines, index, 0, {}, statements, outError, outErrorLineIndex)) {
		return false;
	}

	MethodCodeWriter writer;
	writer.WriteBlock(statements);
	outMethod.lineOffset = writer.TakeLineOffset();
	outMethod.blockOffset = writer.TakeBlockOffset();
	outMethod.methodReference = writer.TakeMethodReference();
	outMethod.variableReference = writer.TakeVariableReference();
	outMethod.constantReference = writer.TakeConstantReference();
	outMethod.expressionData = writer.TakeExpressionData();
	return true;
}

bool DecodeNativeLineOffsets(const std::vector<std::uint8_t>& bytes, std::vector<std::int32_t>& outOffsets)
{
	outOffsets.clear();
	if ((bytes.size() % sizeof(std::int32_t)) != 0) {
		return false;
	}

	outOffsets.resize(bytes.size() / sizeof(std::int32_t));
	for (size_t index = 0; index < outOffsets.size(); ++index) {
		std::memcpy(
			&outOffsets[index],
			bytes.data() + index * sizeof(std::int32_t),
			sizeof(std::int32_t));
		if (outOffsets[index] < 0 ||
			static_cast<size_t>(outOffsets[index]) > bytes.size() + outOffsets[index]) {
			return false;
		}
	}
	return true;
}

bool IsFlatRawStatementList(const std::vector<BodyStatement>& statements)
{
	return std::all_of(
		statements.begin(),
		statements.end(),
		[](const BodyStatement& statement) {
			return statement.kind == BodyStatementKind::Raw;
		});
}

bool AreRawStatementsEquivalent(const BodyStatement& left, const BodyStatement& right)
{
	return left.kind == BodyStatementKind::Raw &&
		right.kind == BodyStatementKind::Raw &&
		left.mask == right.mask &&
		left.code == right.code;
}

struct FlatNativeReuseStatement {
	BodyStatementKind kind = BodyStatementKind::Raw;
	bool mask = false;
	std::string code;
	const BodyStatement* source = nullptr;
};

bool FlattenStatementsForNativeReuse(
	const std::vector<BodyStatement>& statements,
	std::vector<FlatNativeReuseStatement>& out)
{
	for (const auto& statement : statements) {
		if (statement.kind == BodyStatementKind::Raw) {
			out.push_back(FlatNativeReuseStatement{ statement.kind, statement.mask, statement.code, &statement });
			continue;
		}
		if (statement.kind == BodyStatementKind::IfTrue) {
			out.push_back(FlatNativeReuseStatement{ statement.kind, statement.mask, statement.code, &statement });
			if (!FlattenStatementsForNativeReuse(statement.block, out)) {
				return false;
			}
			continue;
		}
		return false;
	}
	return true;
}

bool AreFlatStatementsEquivalent(const FlatNativeReuseStatement& left, const FlatNativeReuseStatement& right)
{
	return left.kind == right.kind &&
		left.mask == right.mask &&
		left.code == right.code;
}

bool DecodeNativeBlockMaxEnd(
	const std::vector<std::uint8_t>& blockOffset,
	std::int32_t& outMaxEnd)
{
	outMaxEnd = 0;
	if (blockOffset.empty()) {
		return true;
	}
	if ((blockOffset.size() % 9) != 0) {
		return false;
	}
	for (size_t offset = 0; offset < blockOffset.size(); offset += 9) {
		std::int32_t begin = 0;
		std::int32_t end = 0;
		std::memcpy(&begin, blockOffset.data() + offset + 1, sizeof(begin));
		std::memcpy(&end, blockOffset.data() + offset + 5, sizeof(end));
		if (begin < 0 || end < begin) {
			return false;
		}
		outMaxEnd = (std::max)(outMaxEnd, end);
	}
	return true;
}

std::vector<std::int32_t> CollectRelativeReferencesForSegment(
	const std::vector<std::int32_t>& references,
	const size_t begin,
	const size_t end)
{
	std::vector<std::int32_t> out;
	for (const auto reference : references) {
		if (reference < 0) {
			continue;
		}
		const size_t value = static_cast<size_t>(reference);
		if (value >= begin && value < end) {
			out.push_back(static_cast<std::int32_t>(value - begin));
		}
	}
	return out;
}

std::string NormalizeNativeSourceLineForReuse(const std::string& line)
{
	return TrimAsciiCopy(line);
}

bool CollectNativeReusableSourceLines(
	const std::vector<std::string>& lines,
	std::vector<std::string>& outLines)
{
	outLines.clear();
	for (const auto& line : lines) {
		if (IsBlankLine(line)) {
			continue;
		}
		outLines.push_back(NormalizeNativeSourceLineForReuse(line));
	}
	return true;
}

bool LooksLikeReusableObjectMethodLine(const std::string& code);

bool TryBuildMethodCodeDataWithNativeSourceLineReuse(
	const std::vector<std::string>& currentLines,
	const std::vector<std::string>& originalLines,
	const BundleNativeMethodSnapshot& nativeMethod,
	RestoreMethod& outMethod)
{
	if (nativeMethod.expressionData.empty()) {
		return false;
	}

	std::vector<std::string> currentReusableLines;
	std::vector<std::string> originalReusableLines;
	CollectNativeReusableSourceLines(currentLines, currentReusableLines);
	CollectNativeReusableSourceLines(originalLines, originalReusableLines);
	if (currentReusableLines.size() != originalReusableLines.size()) {
		return false;
	}

	std::vector<std::int32_t> offsets;
	std::vector<std::int32_t> methodReferences;
	std::vector<std::int32_t> variableReferences;
	std::vector<std::int32_t> constantReferences;
	if (!DecodeNativeLineOffsets(nativeMethod.lineOffset, offsets) ||
		!DecodeNativeLineOffsets(nativeMethod.methodReference, methodReferences) ||
		!DecodeNativeLineOffsets(nativeMethod.variableReference, variableReferences) ||
		!DecodeNativeLineOffsets(nativeMethod.constantReference, constantReferences) ||
		offsets.size() != originalReusableLines.size()) {
		return false;
	}

	std::int32_t maxBlockEnd = 0;
	if (!DecodeNativeBlockMaxEnd(nativeMethod.blockOffset, maxBlockEnd)) {
		return false;
	}

	MethodCodeWriter writer;
	bool changedInsideNativeBlock = false;
	for (size_t lineIndex = 0; lineIndex < currentReusableLines.size(); ++lineIndex) {
		const size_t begin = static_cast<size_t>(offsets[lineIndex]);
		const size_t end =
			lineIndex + 1 < offsets.size()
				? static_cast<size_t>(offsets[lineIndex + 1])
				: nativeMethod.expressionData.size();
		if (begin > end || end > nativeMethod.expressionData.size()) {
			return false;
		}

		if (currentReusableLines[lineIndex] == originalReusableLines[lineIndex]) {
			writer.WriteNativeExpressionStatement(
				std::vector<std::uint8_t>(
					nativeMethod.expressionData.begin() + static_cast<std::ptrdiff_t>(begin),
					nativeMethod.expressionData.begin() + static_cast<std::ptrdiff_t>(end)),
				CollectRelativeReferencesForSegment(methodReferences, begin, end),
				CollectRelativeReferencesForSegment(variableReferences, begin, end),
				CollectRelativeReferencesForSegment(constantReferences, begin, end));
			continue;
		}

		if (LooksLikeReusableObjectMethodLine(currentReusableLines[lineIndex])) {
			return false;
		}
		if (offsets[lineIndex] < maxBlockEnd) {
			changedInsideNativeBlock = true;
		}
		bool mask = false;
		std::string code;
		ExtractMaskPrefix(currentReusableLines[lineIndex], mask, code);
		code = TrimAsciiCopy(code);
		if (StartsWith(code, ".")) {
			return false;
		}
		writer.WriteRawStatement(mask, code);
	}

	outMethod.lineOffset = writer.TakeLineOffset();
	outMethod.blockOffset = changedInsideNativeBlock ? writer.TakeBlockOffset() : nativeMethod.blockOffset;
	outMethod.methodReference = writer.TakeMethodReference();
	outMethod.variableReference = writer.TakeVariableReference();
	outMethod.constantReference = writer.TakeConstantReference();
	outMethod.expressionData = writer.TakeExpressionData();
	return true;
}

struct NativeExpressionSegment {
	size_t begin = 0;
	size_t end = 0;
};

struct EncodedNativeExpression {
	std::vector<std::uint8_t> data;
	std::vector<std::int32_t> methodReferences;
	std::vector<std::int32_t> variableReferences;
	std::vector<std::int32_t> constantReferences;
};

struct NativeObjectVariableSymbol {
	std::int32_t id = 0;
	std::int32_t typeId = 0;
};

struct NativeFunctionSymbol {
	std::int16_t libraryId = 0;
	std::int32_t methodId = 0;
};

struct NativeObjectMethodEncodeContext {
	std::unordered_map<std::string, NativeObjectVariableSymbol> variablesByName;
	std::unordered_map<std::string, NativeFunctionSymbol> functionsByName;
	std::unordered_map<std::int32_t, std::unordered_map<std::string, NativeFunctionSymbol>> methodsByOwnerType;
	const TypeResolver* typeResolver = nullptr;
};

bool StartsWithAt(const std::string& text, const size_t offset, const std::string_view token)
{
	return offset <= text.size() &&
		token.size() <= text.size() - offset &&
		std::string_view(text.data() + offset, token.size()) == token;
}

bool TryGetNativeTextQuoteLength(const std::string& text, const size_t offset, size_t& outLength)
{
	if (StartsWithAt(text, offset, kTextLiteralLeftQuote)) {
		outLength = std::strlen(kTextLiteralLeftQuote);
		return true;
	}
	if (StartsWithAt(text, offset, kTextLiteralRightQuote)) {
		outLength = std::strlen(kTextLiteralRightQuote);
		return true;
	}
	outLength = 0;
	return false;
}

bool SplitTopLevelExpressionByChar(
	const std::string& text,
	const char separator,
	std::vector<std::string>& outParts)
{
	outParts.clear();
	std::string current;
	int parenDepth = 0;
	bool inChineseQuote = false;
	bool inAsciiQuote = false;
	for (size_t index = 0; index < text.size(); ++index) {
		size_t quoteLength = 0;
		if (!inAsciiQuote && TryGetNativeTextQuoteLength(text, index, quoteLength)) {
			inChineseQuote = !inChineseQuote;
			current.append(text, index, quoteLength);
			index += quoteLength - 1;
			continue;
		}
		if (!inChineseQuote && text[index] == '"') {
			inAsciiQuote = !inAsciiQuote;
			current.push_back(text[index]);
			continue;
		}
		if (!inChineseQuote && !inAsciiQuote) {
			if (text[index] == '(') {
				++parenDepth;
			}
			else if (text[index] == ')' && parenDepth > 0) {
				--parenDepth;
			}
			else if (text[index] == separator && parenDepth == 0) {
				outParts.push_back(TrimAsciiCopy(current));
				current.clear();
				continue;
			}
		}
		current.push_back(text[index]);
	}
	outParts.push_back(TrimAsciiCopy(current));
	return !inChineseQuote && !inAsciiQuote && parenDepth == 0;
}

std::string ExtractReusableObjectMethodKey(const std::string& code)
{
	const std::string trimmed = TrimAsciiCopy(code);
	if (trimmed.empty() || trimmed.front() == '\'') {
		return {};
	}
	const size_t callPos = trimmed.find('(');
	if (callPos == std::string::npos) {
		return {};
	}
	const size_t dotPos = trimmed.rfind('.', callPos);
	if (dotPos == std::string::npos || dotPos + 1 >= callPos) {
		return {};
	}
	std::string memberName = TrimAsciiCopy(trimmed.substr(dotPos + 1, callPos - dotPos - 1));
	if (memberName.empty() || memberName.find('=') != std::string::npos) {
		return {};
	}
	return TypeResolver::NormalizeTypeName(memberName);
}

struct ParsedNativeObjectCallLine {
	std::string objectName;
	std::string methodName;
	std::vector<std::string> args;
};

bool SplitNativeObjectCallArguments(const std::string& text, std::vector<std::string>& outArgs)
{
	outArgs.clear();
	if (TrimAsciiCopy(text).empty()) {
		return true;
	}
	std::string current;
	int parenDepth = 0;
	bool inChineseQuote = false;
	bool inAsciiQuote = false;
	for (size_t index = 0; index < text.size(); ++index) {
		size_t quoteLength = 0;
		if (!inAsciiQuote && TryGetNativeTextQuoteLength(text, index, quoteLength)) {
			inChineseQuote = !inChineseQuote;
			current.append(text, index, quoteLength);
			index += quoteLength - 1;
			continue;
		}
		if (!inChineseQuote && text[index] == '"') {
			inAsciiQuote = !inAsciiQuote;
			current.push_back(text[index]);
			continue;
		}
		if (!inChineseQuote && !inAsciiQuote) {
			if (text[index] == '(') {
				++parenDepth;
			}
			else if (text[index] == ')' && parenDepth > 0) {
				--parenDepth;
			}
			else if (text[index] == ',' && parenDepth == 0) {
				outArgs.push_back(TrimAsciiCopy(current));
				current.clear();
				continue;
			}
		}
		current.push_back(text[index]);
	}
	outArgs.push_back(TrimAsciiCopy(current));
	return !inChineseQuote && !inAsciiQuote && parenDepth == 0;
}

bool ParseNativeObjectCallLine(const std::string& code, ParsedNativeObjectCallLine& outCall)
{
	outCall = {};
	const std::string trimmed = TrimAsciiCopy(code);
	if (trimmed.empty() || trimmed.front() == '\'') {
		return false;
	}
	const size_t callPos = trimmed.find('(');
	if (callPos == std::string::npos) {
		return false;
	}
	const size_t closePos = trimmed.rfind(')');
	if (closePos == std::string::npos || closePos < callPos) {
		return false;
	}
	const size_t dotPos = trimmed.rfind('.', callPos);
	if (dotPos == std::string::npos || dotPos == 0 || dotPos + 1 >= callPos) {
		return false;
	}
	outCall.objectName = TrimAsciiCopy(trimmed.substr(0, dotPos));
	outCall.methodName = TrimAsciiCopy(trimmed.substr(dotPos + 1, callPos - dotPos - 1));
	if (outCall.objectName.empty() || outCall.methodName.empty()) {
		return false;
	}
	const std::string argText = trimmed.substr(callPos + 1, closePos - callPos - 1);
	return SplitNativeObjectCallArguments(argText, outCall.args);
}

struct ParsedNativeFunctionCallExpression {
	std::string name;
	std::vector<std::string> args;
};

bool ParseNativeFunctionCallExpression(
	const std::string& rawExpression,
	ParsedNativeFunctionCallExpression& outCall)
{
	outCall = {};
	const std::string expression = TrimAsciiCopy(rawExpression);
	if (expression.empty() || expression.back() != ')') {
		return false;
	}

	int parenDepth = 0;
	bool inChineseQuote = false;
	bool inAsciiQuote = false;
	size_t callPos = std::string::npos;
	for (size_t index = 0; index < expression.size(); ++index) {
		size_t quoteLength = 0;
		if (!inAsciiQuote && TryGetNativeTextQuoteLength(expression, index, quoteLength)) {
			inChineseQuote = !inChineseQuote;
			index += quoteLength - 1;
			continue;
		}
		if (!inChineseQuote && expression[index] == '"') {
			inAsciiQuote = !inAsciiQuote;
			continue;
		}
		if (inChineseQuote || inAsciiQuote) {
			continue;
		}
		if (expression[index] == '(') {
			if (parenDepth == 0 && callPos == std::string::npos) {
				callPos = index;
			}
			++parenDepth;
		}
		else if (expression[index] == ')') {
			--parenDepth;
			if (parenDepth < 0) {
				return false;
			}
			if (parenDepth == 0 && index + 1 != expression.size()) {
				return false;
			}
		}
	}
	if (callPos == std::string::npos || parenDepth != 0 || inChineseQuote || inAsciiQuote) {
		return false;
	}

	outCall.name = TrimAsciiCopy(expression.substr(0, callPos));
	if (outCall.name.empty()) {
		return false;
	}
	const std::string argText = expression.substr(callPos + 1, expression.size() - callPos - 2);
	return SplitNativeObjectCallArguments(argText, outCall.args);
}

bool FindTopLevelNativeAssignmentOperator(const std::string& text, size_t& outOffset, size_t& outLength)
{
	constexpr const char* kFullWidthAssign = "＝";
	outOffset = std::string::npos;
	outLength = 0;
	int parenDepth = 0;
	bool inChineseQuote = false;
	bool inAsciiQuote = false;
	for (size_t index = 0; index < text.size(); ++index) {
		size_t quoteLength = 0;
		if (!inAsciiQuote && TryGetNativeTextQuoteLength(text, index, quoteLength)) {
			inChineseQuote = !inChineseQuote;
			index += quoteLength - 1;
			continue;
		}
		if (!inChineseQuote && text[index] == '"') {
			inAsciiQuote = !inAsciiQuote;
			continue;
		}
		if (inChineseQuote || inAsciiQuote) {
			continue;
		}
		if (text[index] == '(') {
			++parenDepth;
			continue;
		}
		if (text[index] == ')' && parenDepth > 0) {
			--parenDepth;
			continue;
		}
		if (parenDepth != 0) {
			continue;
		}
		if (StartsWithAt(text, index, kFullWidthAssign)) {
			outOffset = index;
			outLength = std::strlen(kFullWidthAssign);
			return true;
		}
		if (text[index] != '=') {
			continue;
		}
		const char previous = index == 0 ? '\0' : text[index - 1];
		const char next = index + 1 >= text.size() ? '\0' : text[index + 1];
		if (previous == '=' || previous == '!' || previous == '<' || previous == '>' || next == '=') {
			continue;
		}
		outOffset = index;
		outLength = 1;
		return true;
	}
	return false;
}

void WriteNativeCallHeader(
	ByteWriter& writer,
	const std::int32_t methodId,
	const std::int16_t libraryId,
	const std::int16_t flags)
{
	writer.WriteI32(methodId);
	writer.WriteI16(libraryId);
	writer.WriteI16(flags);
	writer.WriteBStr(std::nullopt);
	writer.WriteBStr(std::nullopt);
}

std::string StripOuterParentheses(std::string expression)
{
	expression = TrimAsciiCopy(std::move(expression));
	while (expression.size() >= 2 && expression.front() == '(' && expression.back() == ')') {
		int parenDepth = 0;
		bool inChineseQuote = false;
		bool inAsciiQuote = false;
		bool wrapsWholeExpression = true;
		for (size_t index = 0; index < expression.size(); ++index) {
			size_t quoteLength = 0;
			if (!inAsciiQuote && TryGetNativeTextQuoteLength(expression, index, quoteLength)) {
				inChineseQuote = !inChineseQuote;
				index += quoteLength - 1;
				continue;
			}
			if (!inChineseQuote && expression[index] == '"') {
				inAsciiQuote = !inAsciiQuote;
				continue;
			}
			if (inChineseQuote || inAsciiQuote) {
				continue;
			}
			if (expression[index] == '(') {
				++parenDepth;
			}
			else if (expression[index] == ')') {
				--parenDepth;
				if (parenDepth == 0 && index + 1 != expression.size()) {
					wrapsWholeExpression = false;
					break;
				}
			}
		}
		if (!wrapsWholeExpression || parenDepth != 0 || inChineseQuote || inAsciiQuote) {
			break;
		}
		expression = TrimAsciiCopy(expression.substr(1, expression.size() - 2));
	}
	return expression;
}

bool TryResolveNativeFunction(
	const std::string& rawName,
	const NativeObjectMethodEncodeContext& context,
	NativeFunctionSymbol& outSymbol)
{
	const std::string functionKey = TypeResolver::NormalizeTypeName(rawName);
	const auto functionIt = context.functionsByName.find(functionKey);
	if (functionIt != context.functionsByName.end()) {
		outSymbol = functionIt->second;
		return true;
	}
	if (context.typeResolver != nullptr) {
		SupportLibraryCommandInfo commandInfo;
		if (context.typeResolver->TryResolveSupportCommand(rawName, commandInfo)) {
			outSymbol = NativeFunctionSymbol{ commandInfo.libraryId, commandInfo.commandId };
			return true;
		}
	}
	outSymbol = {};
	return false;
}

bool TryResolveNativeOwnerMethod(
	const std::int32_t ownerType,
	const std::string& rawName,
	const NativeObjectMethodEncodeContext& context,
	NativeFunctionSymbol& outSymbol)
{
	const std::string methodKey = TypeResolver::NormalizeTypeName(rawName);
	const auto ownerIt = context.methodsByOwnerType.find(ownerType);
	if (ownerIt != context.methodsByOwnerType.end()) {
		const auto methodIt = ownerIt->second.find(methodKey);
		if (methodIt != ownerIt->second.end()) {
			outSymbol = methodIt->second;
			return true;
		}
	}
	if (context.typeResolver != nullptr) {
		SupportLibraryCommandInfo supportMethod;
		if (context.typeResolver->TryResolveSupportTypeMethod(ownerType, rawName, supportMethod)) {
			outSymbol = NativeFunctionSymbol{ supportMethod.libraryId, supportMethod.commandId };
			return true;
		}
	}
	outSymbol = {};
	return false;
}

bool TryResolveNativeExpressionType(
	const std::string& rawExpression,
	const NativeObjectMethodEncodeContext& context,
	std::int32_t& outTypeId)
{
	const std::string expression = StripOuterParentheses(rawExpression);
	const auto variableIt = context.variablesByName.find(TypeResolver::NormalizeTypeName(expression));
	if (variableIt != context.variablesByName.end()) {
		outTypeId = variableIt->second.typeId;
		return outTypeId != 0;
	}
	outTypeId = 0;
	return false;
}

bool TryEncodeNativeIn38Expression(
	const std::string& rawExpression,
	const NativeObjectMethodEncodeContext& context,
	ByteWriter& writer,
	std::vector<std::int32_t>& variableReferences,
	const bool includeExpressionWrapper,
	std::string* outError = nullptr)
{
	const std::string expression = StripOuterParentheses(rawExpression);
	const auto variableIt = context.variablesByName.find(TypeResolver::NormalizeTypeName(expression));
	if (variableIt != context.variablesByName.end() && variableIt->second.id != 0) {
		if (includeExpressionWrapper) {
			variableReferences.push_back(static_cast<std::int32_t>(writer.position()));
			writer.WriteU8(0x1D);
			writer.WriteU8(0x38);
		}
		writer.WriteI32(variableIt->second.id);
		if (includeExpressionWrapper) {
			writer.WriteU8(0x37);
		}
		return true;
	}
	if (outError != nullptr) {
		*outError = "in38_expression_not_found: " + expression;
	}
	return false;
}

bool TryEncodeNativeCallExpression(
	const NativeFunctionSymbol& functionSymbol,
	const std::optional<std::string>& targetExpression,
	const std::vector<std::string>& args,
	const NativeObjectMethodEncodeContext& context,
	ByteWriter& writer,
	std::vector<std::int32_t>& methodReferences,
	std::vector<std::int32_t>& variableReferences,
	std::string* outError = nullptr);

bool TryEncodeNativeExpression(
	const std::string& rawExpression,
	const NativeObjectMethodEncodeContext& context,
	ByteWriter& writer,
	std::vector<std::int32_t>& methodReferences,
	std::vector<std::int32_t>& variableReferences,
	std::string* outError = nullptr)
{
	const std::string expression = StripOuterParentheses(rawExpression);
	if (expression.empty()) {
		writer.WriteU8(0x16);
		return true;
	}

	if (expression.front() == '&') {
		NativeFunctionSymbol functionSymbol;
		const std::string methodName = TrimAsciiCopy(expression.substr(1));
		if (!TryResolveNativeFunction(methodName, context, functionSymbol) ||
			(functionSymbol.libraryId != -2 && functionSymbol.libraryId != -3)) {
			if (outError != nullptr) {
				*outError = "method_pointer_not_found: " + methodName;
			}
			return false;
		}
		methodReferences.push_back(static_cast<std::int32_t>(writer.position()));
		writer.WriteU8(0x1E);
		writer.WriteI32(functionSymbol.methodId);
		return true;
	}

	const std::string variableKey = TypeResolver::NormalizeTypeName(expression);
	const auto variableIt = context.variablesByName.find(variableKey);
	if (variableIt != context.variablesByName.end() && variableIt->second.id != 0) {
		return TryEncodeNativeIn38Expression(expression, context, writer, variableReferences, true, outError);
	}

	std::string textValue;
	bool isLongText = false;
	if (TryDecodeDumpTextLiteral(expression, textValue, isLongText) && !isLongText) {
		writer.WriteU8(0x1A);
		writer.WriteBStr(std::make_optional(textValue));
		return true;
	}

	if (const auto boolValue = ParseBoolLiteral(expression); boolValue.has_value()) {
		writer.WriteU8(0x18);
		writer.WriteI16(static_cast<std::int16_t>(*boolValue ? 1 : 0));
		return true;
	}

	std::int32_t integerValue = 0;
	const auto [ptr, ec] = std::from_chars(expression.data(), expression.data() + expression.size(), integerValue);
	if (ec == std::errc() && ptr == expression.data() + expression.size()) {
		writer.WriteU8(0x3B);
		writer.WriteI32(integerValue);
		return true;
	}

	double doubleValue = 0.0;
	if (TryParseDouble(expression, doubleValue)) {
		writer.WriteU8(0x17);
		writer.WriteDouble(doubleValue);
		return true;
	}

	std::vector<std::string> plusParts;
	if (SplitTopLevelExpressionByChar(expression, '+', plusParts) && plusParts.size() > 1) {
		writer.WriteU8(0x21);
		WriteNativeCallHeader(writer, 19, 0, 0);
		writer.WriteU8(0x36);
		for (const auto& part : plusParts) {
			if (!TryEncodeNativeExpression(part, context, writer, methodReferences, variableReferences, outError)) {
				if (outError != nullptr && outError->empty()) {
					*outError = "plus_argument_encode_failed: " + part;
				}
				return false;
			}
		}
		writer.WriteU8(0x01);
		return true;
	}

	ParsedNativeFunctionCallExpression functionCall;
	if (ParseNativeFunctionCallExpression(expression, functionCall)) {
		ParsedNativeObjectCallLine memberCall;
		if (ParseNativeObjectCallLine(expression, memberCall)) {
			std::int32_t targetTypeId = 0;
			if (!TryResolveNativeExpressionType(memberCall.objectName, context, targetTypeId)) {
				if (outError != nullptr) {
					*outError = "member_call_target_type_not_found: " + memberCall.objectName;
				}
				return false;
			}
			NativeFunctionSymbol methodSymbol;
			if (!TryResolveNativeOwnerMethod(targetTypeId, memberCall.methodName, context, methodSymbol)) {
				if (outError != nullptr) {
					*outError = "member_call_method_not_found: " + memberCall.objectName + "." + memberCall.methodName +
						" type=" + std::to_string(targetTypeId);
				}
				return false;
			}
			return TryEncodeNativeCallExpression(
				methodSymbol,
				memberCall.objectName,
				memberCall.args,
				context,
				writer,
				methodReferences,
				variableReferences,
				outError);
		}

		NativeFunctionSymbol functionSymbol;
		if (!TryResolveNativeFunction(functionCall.name, context, functionSymbol)) {
			if (outError != nullptr) {
				*outError = "function_not_found: " + functionCall.name;
			}
			return false;
		}
		return TryEncodeNativeCallExpression(
			functionSymbol,
			std::nullopt,
			functionCall.args,
			context,
			writer,
			methodReferences,
			variableReferences,
			outError);
	}

	if (outError != nullptr) {
		*outError = "unsupported_expression: " + expression;
	}
	return false;
}

bool TryEncodeNativeCallExpression(
	const NativeFunctionSymbol& functionSymbol,
	const std::optional<std::string>& targetExpression,
	const std::vector<std::string>& args,
	const NativeObjectMethodEncodeContext& context,
	ByteWriter& writer,
	std::vector<std::int32_t>& methodReferences,
	std::vector<std::int32_t>& variableReferences,
	std::string* outError)
{
	const auto callOffset = static_cast<std::int32_t>(writer.position());
	if (targetExpression.has_value()) {
		variableReferences.push_back(callOffset);
	}
	if (functionSymbol.libraryId == -2 || functionSymbol.libraryId == -3) {
		methodReferences.push_back(callOffset);
	}

	writer.WriteU8(0x21);
	WriteNativeCallHeader(writer, functionSymbol.methodId, functionSymbol.libraryId, 0);
	if (targetExpression.has_value()) {
		writer.WriteU8(0x38);
		if (!TryEncodeNativeIn38Expression(*targetExpression, context, writer, variableReferences, false, outError)) {
			return false;
		}
		writer.WriteU8(0x37);
	}
	else {
		writer.WriteU8(0x36);
	}
	for (const auto& arg : args) {
		if (!TryEncodeNativeExpression(arg, context, writer, methodReferences, variableReferences, outError)) {
			if (outError != nullptr && outError->empty()) {
				*outError = "call_argument_encode_failed: " + arg;
			}
			return false;
		}
	}
	writer.WriteU8(0x01);
	return true;
}

bool TryEncodeNativeObjectMethodCallLine(
	const BodyStatement& statement,
	const NativeObjectMethodEncodeContext& context,
	EncodedNativeExpression& outExpression,
	std::string* outError = nullptr)
{
	outExpression = {};
	if (statement.kind != BodyStatementKind::Raw) {
		if (outError != nullptr) {
			*outError = "statement_is_not_raw";
		}
		return false;
	}

	ParsedNativeObjectCallLine call;
	if (!ParseNativeObjectCallLine(statement.code, call)) {
		if (outError != nullptr) {
			*outError = "object_call_parse_failed: " + statement.code;
		}
		return false;
	}

	const std::string objectKey = TypeResolver::NormalizeTypeName(call.objectName);
	const auto variableIt = context.variablesByName.find(objectKey);
	if (variableIt == context.variablesByName.end() ||
		variableIt->second.id == 0) {
		if (outError != nullptr) {
			*outError = "object_variable_not_found: " + call.objectName;
		}
		return false;
	}
	if (variableIt->second.typeId == 0) {
		if (outError != nullptr) {
			*outError = "object_variable_type_missing: " + call.objectName;
		}
		return false;
	}

	const auto ownerIt = context.methodsByOwnerType.find(variableIt->second.typeId);
	if (ownerIt == context.methodsByOwnerType.end()) {
		if (outError != nullptr) {
			*outError = "owner_type_methods_missing: " + call.objectName + " type=" + std::to_string(variableIt->second.typeId);
		}
		return false;
	}
	const std::string methodKey = TypeResolver::NormalizeTypeName(call.methodName);
	NativeFunctionSymbol methodSymbol;
	bool hasMethodSymbol = false;
	const auto methodIt = ownerIt->second.find(methodKey);
	if (methodIt != ownerIt->second.end()) {
		methodSymbol = methodIt->second;
		hasMethodSymbol = true;
	}
	else if (context.typeResolver != nullptr) {
		SupportLibraryCommandInfo supportMethod;
		if (context.typeResolver->TryResolveSupportTypeMethod(variableIt->second.typeId, call.methodName, supportMethod)) {
			methodSymbol = NativeFunctionSymbol{ supportMethod.libraryId, supportMethod.commandId };
			hasMethodSymbol = true;
		}
	}
	if (!hasMethodSymbol) {
		if (outError != nullptr) {
			*outError = "object_method_not_found: " + call.objectName + "." + call.methodName +
				" type=" + std::to_string(variableIt->second.typeId);
		}
		return false;
	}

	ByteWriter writer;
	writer.WriteU8(0x6A);
	WriteNativeCallHeader(writer, methodSymbol.methodId, methodSymbol.libraryId, static_cast<std::int16_t>(statement.mask ? 0x20 : 0));
	if (methodSymbol.libraryId == -2 || methodSymbol.libraryId == -3) {
		outExpression.methodReferences.push_back(0);
	}
	outExpression.variableReferences.push_back(0);

	writer.WriteU8(0x38);
	writer.WriteI32(variableIt->second.id);
	writer.WriteU8(0x37);
	for (const auto& arg : call.args) {
		std::string expressionError;
		if (!TryEncodeNativeExpression(arg, context, writer, outExpression.methodReferences, outExpression.variableReferences, &expressionError)) {
			if (outError != nullptr) {
				*outError = "argument_encode_failed: " + call.objectName + "." + call.methodName +
					" arg=\"" + arg + "\" " + expressionError;
			}
			return false;
		}
	}
	writer.WriteU8(0x01);
	outExpression.data = writer.TakeBytes();
	return true;
}

bool TryEncodeNativeFunctionCallStatementLine(
	const BodyStatement& statement,
	const NativeObjectMethodEncodeContext& context,
	EncodedNativeExpression& outExpression,
	std::string* outError = nullptr)
{
	outExpression = {};
	if (statement.kind != BodyStatementKind::Raw) {
		if (outError != nullptr) {
			*outError = "statement_is_not_raw";
		}
		return false;
	}

	ParsedNativeFunctionCallExpression call;
	if (!ParseNativeFunctionCallExpression(statement.code, call) ||
		call.name.find('.') != std::string::npos) {
		if (outError != nullptr) {
			*outError = "function_call_parse_failed: " + statement.code;
		}
		return false;
	}

	NativeFunctionSymbol functionSymbol;
	if (!TryResolveNativeFunction(call.name, context, functionSymbol)) {
		if (outError != nullptr) {
			*outError = "function_not_found: " + call.name;
		}
		return false;
	}

	ByteWriter writer;
	const auto callOffset = static_cast<std::int32_t>(writer.position());
	if (functionSymbol.libraryId == -2 || functionSymbol.libraryId == -3) {
		outExpression.methodReferences.push_back(callOffset);
	}
	writer.WriteU8(0x6A);
	WriteNativeCallHeader(writer, functionSymbol.methodId, functionSymbol.libraryId, static_cast<std::int16_t>(statement.mask ? 0x20 : 0));
	writer.WriteU8(0x36);
	for (const auto& arg : call.args) {
		std::string expressionError;
		if (!TryEncodeNativeExpression(arg, context, writer, outExpression.methodReferences, outExpression.variableReferences, &expressionError)) {
			if (outError != nullptr) {
				*outError = "function_argument_encode_failed: " + call.name +
					" arg=\"" + arg + "\" " + expressionError;
			}
			return false;
		}
	}
	writer.WriteU8(0x01);
	outExpression.data = writer.TakeBytes();
	return true;
}

bool TryEncodeNativeAssignmentLine(
	const BodyStatement& statement,
	const NativeObjectMethodEncodeContext& context,
	EncodedNativeExpression& outExpression,
	std::string* outError = nullptr)
{
	outExpression = {};
	if (statement.kind != BodyStatementKind::Raw) {
		if (outError != nullptr) {
			*outError = "statement_is_not_raw";
		}
		return false;
	}

	size_t assignOffset = std::string::npos;
	size_t assignLength = 0;
	if (!FindTopLevelNativeAssignmentOperator(statement.code, assignOffset, assignLength)) {
		if (outError != nullptr) {
			*outError = "assignment_parse_failed: " + statement.code;
		}
		return false;
	}
	const std::string leftExpression = TrimAsciiCopy(statement.code.substr(0, assignOffset));
	const std::string rightExpression = TrimAsciiCopy(statement.code.substr(assignOffset + assignLength));
	if (leftExpression.empty() || rightExpression.empty()) {
		if (outError != nullptr) {
			*outError = "assignment_empty_side: " + statement.code;
		}
		return false;
	}

	ByteWriter writer;
	writer.WriteU8(0x6A);
	WriteNativeCallHeader(writer, 52, 0, static_cast<std::int16_t>(statement.mask ? 0x20 : 0));
	writer.WriteU8(0x36);
	std::string expressionError;
	if (!TryEncodeNativeExpression(leftExpression, context, writer, outExpression.methodReferences, outExpression.variableReferences, &expressionError)) {
		if (outError != nullptr) {
			*outError = "assignment_left_encode_failed: " + leftExpression + " " + expressionError;
		}
		return false;
	}
	expressionError.clear();
	if (!TryEncodeNativeExpression(rightExpression, context, writer, outExpression.methodReferences, outExpression.variableReferences, &expressionError)) {
		if (outError != nullptr) {
			*outError = "assignment_right_encode_failed: " + rightExpression + " " + expressionError;
		}
		return false;
	}
	writer.WriteU8(0x01);
	outExpression.data = writer.TakeBytes();
	return true;
}

bool TryEncodeNativeRawStatementLine(
	const BodyStatement& statement,
	const NativeObjectMethodEncodeContext& context,
	EncodedNativeExpression& outExpression,
	std::string* outError = nullptr)
{
	std::string lastError;
	if (TryEncodeNativeObjectMethodCallLine(statement, context, outExpression, &lastError)) {
		return true;
	}
	if (TryEncodeNativeAssignmentLine(statement, context, outExpression, &lastError)) {
		return true;
	}
	if (TryEncodeNativeFunctionCallStatementLine(statement, context, outExpression, &lastError)) {
		return true;
	}
	if (outError != nullptr) {
		*outError = lastError;
	}
	return false;
}

bool LooksLikeReusableObjectMethodLine(const std::string& code)
{
	return !ExtractReusableObjectMethodKey(code).empty();
}

struct ReusableObjectMethodLine {
	std::string normalizedLine;
	std::string methodKey;
};

void CollectTopLevelReusableObjectMethodLines(
	const std::vector<BodyStatement>& statements,
	std::vector<ReusableObjectMethodLine>& outLines)
{
	for (const auto& statement : statements) {
		if (statement.kind != BodyStatementKind::Raw) {
			continue;
		}
		const std::string methodKey = ExtractReusableObjectMethodKey(statement.code);
		if (methodKey.empty()) {
			continue;
		}
		outLines.push_back(ReusableObjectMethodLine{
			NormalizeNativeSourceLineForReuse(statement.code),
			methodKey,
		});
	}
}

bool ParseTopLevelReusableObjectMethodLines(
	const std::vector<std::string>& lines,
	std::vector<ReusableObjectMethodLine>& outLines)
{
	outLines.clear();
	std::vector<BodyStatement> statements;
	size_t index = 0;
	std::string ignoredError;
	if (!ParseBodyBlock(lines, index, 0, {}, statements, &ignoredError, nullptr) ||
		index < lines.size()) {
		return false;
	}
	CollectTopLevelReusableObjectMethodLines(statements, outLines);
	return true;
}

bool BuildNativeMethodReferenceSegments(
	const BundleNativeMethodSnapshot& nativeMethod,
	std::vector<NativeExpressionSegment>& outSegments);

bool HasChangedNativeObjectMethodLine(
	const std::vector<std::string>& currentLines,
	const std::vector<std::string>& originalLines,
	const BundleNativeMethodSnapshot& nativeMethod,
	const NativeObjectMethodEncodeContext* encodeContext,
	std::string* outUnsupportedReason = nullptr)
{
	if (outUnsupportedReason != nullptr) {
		outUnsupportedReason->clear();
	}
	std::vector<NativeExpressionSegment> nativeSegments;
	if (!BuildNativeMethodReferenceSegments(nativeMethod, nativeSegments)) {
		return false;
	}

	std::vector<ReusableObjectMethodLine> currentObjectLines;
	std::vector<ReusableObjectMethodLine> originalObjectLines;
	if (!ParseTopLevelReusableObjectMethodLines(currentLines, currentObjectLines) ||
		!ParseTopLevelReusableObjectMethodLines(originalLines, originalObjectLines) ||
		originalObjectLines.empty()) {
		return false;
	}

	const size_t reusableSegmentCount = (std::min)(nativeSegments.size(), originalObjectLines.size());
	size_t originalIndex = 0;
	for (const auto& currentLine : currentObjectLines) {
		size_t exactMatch = reusableSegmentCount;
		for (size_t candidateIndex = originalIndex; candidateIndex < reusableSegmentCount; ++candidateIndex) {
			if (originalObjectLines[candidateIndex].normalizedLine == currentLine.normalizedLine) {
				exactMatch = candidateIndex;
				break;
			}
		}
		if (exactMatch < reusableSegmentCount) {
			originalIndex = exactMatch + 1;
			continue;
		}

		if (encodeContext != nullptr) {
			BodyStatement syntheticStatement;
			syntheticStatement.kind = BodyStatementKind::Raw;
			syntheticStatement.code = currentLine.normalizedLine;
			EncodedNativeExpression ignoredExpression;
			std::string encodeError;
			if (TryEncodeNativeObjectMethodCallLine(syntheticStatement, *encodeContext, ignoredExpression, &encodeError)) {
				for (size_t candidateIndex = originalIndex; candidateIndex < reusableSegmentCount; ++candidateIndex) {
					if (originalObjectLines[candidateIndex].methodKey == currentLine.methodKey) {
						originalIndex = candidateIndex + 1;
						break;
					}
				}
				continue;
			}
			if (outUnsupportedReason != nullptr && outUnsupportedReason->empty()) {
				*outUnsupportedReason = encodeError;
			}
		}

		for (size_t candidateIndex = originalIndex; candidateIndex < reusableSegmentCount; ++candidateIndex) {
			if (originalObjectLines[candidateIndex].methodKey == currentLine.methodKey) {
				return true;
			}
		}

		return true;
	}
	return false;
}

bool BuildNativeMethodReferenceSegments(
	const BundleNativeMethodSnapshot& nativeMethod,
	std::vector<NativeExpressionSegment>& outSegments)
{
	outSegments.clear();
	std::vector<std::int32_t> lineOffsets;
	std::vector<std::int32_t> methodReferences;
	if (!DecodeNativeLineOffsets(nativeMethod.lineOffset, lineOffsets) ||
		!DecodeNativeLineOffsets(nativeMethod.methodReference, methodReferences) ||
		lineOffsets.empty()) {
		return false;
	}

	std::unordered_set<size_t> usedBegins;
	for (const auto reference : methodReferences) {
		if (reference < 0) {
			continue;
		}
		const size_t refOffset = static_cast<size_t>(reference);
		for (size_t lineIndex = 0; lineIndex < lineOffsets.size(); ++lineIndex) {
			const size_t begin = static_cast<size_t>(lineOffsets[lineIndex]);
			const size_t end =
				lineIndex + 1 < lineOffsets.size()
					? static_cast<size_t>(lineOffsets[lineIndex + 1])
					: nativeMethod.expressionData.size();
			if (refOffset < begin || refOffset >= end) {
				continue;
			}
			if (begin <= end &&
				end <= nativeMethod.expressionData.size() &&
				usedBegins.insert(begin).second) {
				outSegments.push_back(NativeExpressionSegment{ begin, end });
			}
			break;
		}
	}
	return !outSegments.empty();
}

bool TryBuildMethodCodeDataWithNativeObjectCallReuse(
	const std::vector<std::string>& currentLines,
	const std::vector<std::string>& originalLines,
	const BundleNativeMethodSnapshot& nativeMethod,
	RestoreMethod& outMethod,
	const NativeObjectMethodEncodeContext* encodeContext)
{
	std::vector<BodyStatement> statements;
	size_t index = 0;
	std::string ignoredError;
	if (!ParseBodyBlock(currentLines, index, 0, {}, statements, &ignoredError, nullptr) ||
		index < currentLines.size()) {
		return false;
	}

	std::vector<BodyStatement> originalStatements;
	index = 0;
	if (!ParseBodyBlock(originalLines, index, 0, {}, originalStatements, &ignoredError, nullptr) ||
		index < originalLines.size()) {
		return false;
	}

	std::vector<NativeExpressionSegment> nativeSegments;
	if (!BuildNativeMethodReferenceSegments(nativeMethod, nativeSegments)) {
		return false;
	}

	std::vector<std::string> originalObjectLines;
	std::vector<std::string> originalObjectMethodKeys;
	for (const auto& statement : originalStatements) {
		if (statement.kind != BodyStatementKind::Raw) {
			continue;
		}
		const std::string methodKey = ExtractReusableObjectMethodKey(statement.code);
		if (methodKey.empty()) {
			continue;
		}
		originalObjectLines.push_back(NormalizeNativeSourceLineForReuse(statement.code));
		originalObjectMethodKeys.push_back(methodKey);
	}
	const size_t reusableSegmentCount = (std::min)(nativeSegments.size(), originalObjectLines.size());
	if (reusableSegmentCount == 0) {
		return false;
	}

	std::vector<std::int32_t> methodReferences;
	std::vector<std::int32_t> variableReferences;
	std::vector<std::int32_t> constantReferences;
	if (!DecodeNativeLineOffsets(nativeMethod.methodReference, methodReferences) ||
		!DecodeNativeLineOffsets(nativeMethod.variableReference, variableReferences) ||
		!DecodeNativeLineOffsets(nativeMethod.constantReference, constantReferences)) {
		return false;
	}

	MethodCodeWriter writer;
	size_t nativeSegmentIndex = 0;
	size_t reusedNativeSegmentCount = 0;
	size_t encodedNativeSegmentCount = 0;
	bool unsupportedChangedObjectCall = false;
	const auto writeNativeOrRaw = [&](const BodyStatement& statement) {
		const std::string methodKey = ExtractReusableObjectMethodKey(statement.code);
		if (!methodKey.empty() && nativeSegmentIndex < reusableSegmentCount) {
			const std::string normalizedLine = NormalizeNativeSourceLineForReuse(statement.code);
			size_t matchedIndex = reusableSegmentCount;
			for (size_t candidateIndex = nativeSegmentIndex; candidateIndex < reusableSegmentCount; ++candidateIndex) {
				if (originalObjectLines[candidateIndex] == normalizedLine) {
					matchedIndex = candidateIndex;
					break;
				}
			}
			if (matchedIndex < reusableSegmentCount) {
				const auto segment = nativeSegments[matchedIndex];
				nativeSegmentIndex = matchedIndex + 1;
				++reusedNativeSegmentCount;
				writer.WriteNativeExpressionStatement(
					std::vector<std::uint8_t>(
						nativeMethod.expressionData.begin() + static_cast<std::ptrdiff_t>(segment.begin),
						nativeMethod.expressionData.begin() + static_cast<std::ptrdiff_t>(segment.end)),
					CollectRelativeReferencesForSegment(methodReferences, segment.begin, segment.end),
					CollectRelativeReferencesForSegment(variableReferences, segment.begin, segment.end),
					CollectRelativeReferencesForSegment(constantReferences, segment.begin, segment.end));
				return;
			}

			if (originalObjectMethodKeys[nativeSegmentIndex] == methodKey) {
				if (encodeContext != nullptr) {
					EncodedNativeExpression encodedExpression;
					if (TryEncodeNativeObjectMethodCallLine(statement, *encodeContext, encodedExpression)) {
						++nativeSegmentIndex;
						writer.WriteNativeExpressionStatement(
							encodedExpression.data,
							encodedExpression.methodReferences,
							encodedExpression.variableReferences,
							encodedExpression.constantReferences);
						++encodedNativeSegmentCount;
						return;
					}
				}
				++nativeSegmentIndex;
				unsupportedChangedObjectCall = true;
				return;
			}

			if (encodeContext != nullptr) {
				EncodedNativeExpression encodedExpression;
				if (TryEncodeNativeObjectMethodCallLine(statement, *encodeContext, encodedExpression)) {
					writer.WriteNativeExpressionStatement(
						encodedExpression.data,
						encodedExpression.methodReferences,
						encodedExpression.variableReferences,
						encodedExpression.constantReferences);
					++encodedNativeSegmentCount;
					return;
				}
			}
		}
		writer.WriteRawStatement(statement.mask, statement.code);
	};

	for (const auto& statement : statements) {
		switch (statement.kind) {
		case BodyStatementKind::Raw:
			writeNativeOrRaw(statement);
			break;
		case BodyStatementKind::IfTrue:
			writer.WriteIfTrue(statement);
			break;
		case BodyStatementKind::IfElse:
			writer.WriteIfElse(statement);
			break;
		case BodyStatementKind::WhileLoop:
			writer.WriteWhile(statement);
			break;
		case BodyStatementKind::DoWhileLoop:
			writer.WriteDoWhile(statement);
			break;
		case BodyStatementKind::CounterLoop:
			writer.WriteCounter(statement);
			break;
		case BodyStatementKind::ForLoop:
			writer.WriteFor(statement);
			break;
		case BodyStatementKind::SwitchBlock:
			writer.WriteSwitch(statement);
			break;
		}
	}

	if (unsupportedChangedObjectCall) {
		return false;
	}
	if (nativeSegmentIndex == 0 && encodedNativeSegmentCount == 0) {
		return false;
	}
	if (reusedNativeSegmentCount == 0 && encodedNativeSegmentCount == 0) {
		return false;
	}
	outMethod.lineOffset = writer.TakeLineOffset();
	outMethod.blockOffset = writer.TakeBlockOffset();
	outMethod.methodReference = writer.TakeMethodReference();
	outMethod.variableReference = writer.TakeVariableReference();
	outMethod.constantReference = writer.TakeConstantReference();
	outMethod.expressionData = writer.TakeExpressionData();
	return true;
}

bool BuildMethodCodeDataWithSemanticNativeObjectCalls(
	const std::vector<std::string>& lines,
	RestoreMethod& outMethod,
	const NativeObjectMethodEncodeContext& encodeContext,
	std::string* outError)
{
	std::vector<BodyStatement> statements;
	size_t index = 0;
	std::string parseError;
	if (!ParseBodyBlock(lines, index, 0, {}, statements, &parseError, nullptr) ||
		index < lines.size()) {
		if (outError != nullptr) {
			*outError = parseError.empty() ? "method_body_parse_failed" : parseError;
		}
		return false;
	}

	MethodCodeWriter writer;
	bool ok = true;
	std::string semanticError;
	auto writeRaw = [&](const BodyStatement& statement) {
		if (!ok) {
			return;
		}
		EncodedNativeExpression encodedExpression;
		std::string encodeError;
		if (TryEncodeNativeRawStatementLine(statement, encodeContext, encodedExpression, &encodeError)) {
			writer.WriteNativeExpressionStatement(
				encodedExpression.data,
				encodedExpression.methodReferences,
				encodedExpression.variableReferences,
				encodedExpression.constantReferences);
			return;
		}
		if (LooksLikeReusableObjectMethodLine(statement.code)) {
			ok = false;
			semanticError = encodeError.empty()
				? "native_object_method_encode_failed: " + statement.code
				: encodeError;
			return;
		}
		writer.WriteRawStatement(statement.mask, statement.code);
	};
	writer.WriteBlockWithRawHandler(statements, writeRaw);
	if (!ok) {
		if (outError != nullptr) {
			*outError = semanticError;
		}
		return false;
	}

	outMethod.lineOffset = writer.TakeLineOffset();
	outMethod.blockOffset = writer.TakeBlockOffset();
	outMethod.methodReference = writer.TakeMethodReference();
	outMethod.variableReference = writer.TakeVariableReference();
	outMethod.constantReference = writer.TakeConstantReference();
	outMethod.expressionData = writer.TakeExpressionData();
	return true;
}

bool TryBuildMethodCodeDataWithNativeLineReuse(
	const std::vector<std::string>& currentLines,
	const std::vector<std::string>& originalLines,
	const BundleNativeMethodSnapshot& nativeMethod,
	RestoreMethod& outMethod)
{
	if (nativeMethod.expressionData.empty()) {
		return false;
	}

	std::vector<BodyStatement> currentStatements;
	std::vector<BodyStatement> originalStatements;
	size_t index = 0;
	std::string ignoredError;
	if (!ParseBodyBlock(currentLines, index, 0, {}, currentStatements, &ignoredError, nullptr) ||
		index < currentLines.size()) {
		return false;
	}
	index = 0;
	if (!ParseBodyBlock(originalLines, index, 0, {}, originalStatements, &ignoredError, nullptr) ||
		index < originalLines.size()) {
		return false;
	}
	std::vector<FlatNativeReuseStatement> currentFlatStatements;
	std::vector<FlatNativeReuseStatement> originalFlatStatements;
	if (!FlattenStatementsForNativeReuse(currentStatements, currentFlatStatements) ||
		!FlattenStatementsForNativeReuse(originalStatements, originalFlatStatements) ||
		currentFlatStatements.size() != originalFlatStatements.size()) {
		return false;
	}

	std::vector<std::int32_t> offsets;
	if (!DecodeNativeLineOffsets(nativeMethod.lineOffset, offsets) ||
		offsets.size() != originalFlatStatements.size()) {
		return false;
	}

	std::int32_t maxBlockEnd = 0;
	if (!DecodeNativeBlockMaxEnd(nativeMethod.blockOffset, maxBlockEnd)) {
		return false;
	}

	MethodCodeWriter writer;
	bool changedInsideNativeBlock = false;
	for (size_t statementIndex = 0; statementIndex < currentFlatStatements.size(); ++statementIndex) {
		if (AreFlatStatementsEquivalent(currentFlatStatements[statementIndex], originalFlatStatements[statementIndex])) {
			const size_t begin = static_cast<size_t>(offsets[statementIndex]);
			const size_t end =
				statementIndex + 1 < offsets.size()
					? static_cast<size_t>(offsets[statementIndex + 1])
					: nativeMethod.expressionData.size();
			if (begin > end || end > nativeMethod.expressionData.size()) {
				return false;
			}
			writer.WriteNativeExpressionStatement(std::vector<std::uint8_t>(
				nativeMethod.expressionData.begin() + static_cast<std::ptrdiff_t>(begin),
				nativeMethod.expressionData.begin() + static_cast<std::ptrdiff_t>(end)));
		}
		else {
			if (offsets[statementIndex] < maxBlockEnd) {
				changedInsideNativeBlock = true;
			}
			if (currentFlatStatements[statementIndex].kind != BodyStatementKind::Raw ||
				currentFlatStatements[statementIndex].source == nullptr) {
				return false;
			}
			if (LooksLikeReusableObjectMethodLine(currentFlatStatements[statementIndex].source->code)) {
				return false;
			}
			writer.WriteRawStatement(
				currentFlatStatements[statementIndex].source->mask,
				currentFlatStatements[statementIndex].source->code);
		}
	}

	outMethod.lineOffset = writer.TakeLineOffset();
	outMethod.blockOffset = changedInsideNativeBlock ? writer.TakeBlockOffset() : nativeMethod.blockOffset;
	outMethod.methodReference = nativeMethod.methodReference;
	outMethod.variableReference = nativeMethod.variableReference;
	outMethod.constantReference = nativeMethod.constantReference;
	outMethod.expressionData = writer.TakeExpressionData();
	return true;
}

struct XmlNode {
	std::string name;
	std::unordered_map<std::string, std::string> attributes;
	std::vector<XmlNode> children;
};

class SimpleXmlParser {
public:
	explicit SimpleXmlParser(const std::string& text)
		: m_text(text)
	{
	}

	bool Parse(XmlNode& outRoot, std::string* outError)
	{
		SkipWhitespace();
		if (StartsWith(std::string_view(m_text).substr(m_pos), "<?xml")) {
			const size_t end = m_text.find("?>", m_pos);
			if (end == std::string::npos) {
				if (outError != nullptr) {
					*outError = "xml_declaration_invalid";
				}
				return false;
			}
			m_pos = end + 2;
		}
		SkipWhitespace();
		if (!ParseNode(outRoot, outError)) {
			return false;
		}
		SkipWhitespace();
		return true;
	}

	size_t CurrentLineIndex() const
	{
		size_t lineIndex = 0;
		for (size_t i = 0; i < (std::min)(m_pos, m_text.size()); ++i) {
			if (m_text[i] == '\n') {
				++lineIndex;
			}
		}
		return lineIndex;
	}

private:
	void SkipWhitespace()
	{
		while (m_pos < m_text.size() && std::isspace(static_cast<unsigned char>(m_text[m_pos])) != 0) {
			++m_pos;
		}
	}

	bool ParseName(std::string& outName)
	{
		const size_t start = m_pos;
		while (m_pos < m_text.size()) {
			const unsigned char ch = static_cast<unsigned char>(m_text[m_pos]);
			if (std::isspace(ch) != 0 || ch == '/' || ch == '>' || ch == '=' || ch == '?') {
				break;
			}
			++m_pos;
		}
		if (m_pos == start) {
			return false;
		}
		outName = m_text.substr(start, m_pos - start);
		return true;
	}

	bool ParseQuotedValue(std::string& outValue)
	{
		if (m_pos >= m_text.size() || m_text[m_pos] != '"') {
			return false;
		}
		++m_pos;
		const size_t start = m_pos;
		while (m_pos < m_text.size() && m_text[m_pos] != '"') {
			++m_pos;
		}
		if (m_pos >= m_text.size()) {
			return false;
		}
		outValue = DecodeXmlEntities(m_text.substr(start, m_pos - start));
		++m_pos;
		return true;
	}

	bool ParseAttributes(
		std::unordered_map<std::string, std::string>& outAttributes,
		bool& outSelfClosing,
		std::string* outError)
	{
		outSelfClosing = false;
		while (m_pos < m_text.size()) {
			SkipWhitespace();
			if (m_pos >= m_text.size()) {
				break;
			}
			if (m_text[m_pos] == '/') {
				++m_pos;
				if (m_pos >= m_text.size() || m_text[m_pos] != '>') {
					if (outError != nullptr) {
						*outError = "xml_self_closing_invalid";
					}
					return false;
				}
				++m_pos;
				outSelfClosing = true;
				return true;
			}
			if (m_text[m_pos] == '>') {
				++m_pos;
				return true;
			}

			std::string key;
			if (!ParseName(key)) {
				if (outError != nullptr) {
					*outError = "xml_attr_name_invalid";
				}
				return false;
			}
			SkipWhitespace();
			if (m_pos >= m_text.size() || m_text[m_pos] != '=') {
				if (outError != nullptr) {
					*outError = "xml_attr_assign_missing";
				}
				return false;
			}
			++m_pos;
			SkipWhitespace();
			std::string value;
			if (!ParseQuotedValue(value)) {
				if (outError != nullptr) {
					*outError = "xml_attr_value_invalid";
				}
				return false;
			}
			outAttributes.insert_or_assign(key, value);
		}

		if (outError != nullptr) {
			*outError = "xml_attr_eof";
		}
		return false;
	}

	bool ParseNode(XmlNode& outNode, std::string* outError)
	{
		if (m_pos >= m_text.size() || m_text[m_pos] != '<') {
			if (outError != nullptr) {
				*outError = "xml_tag_missing";
			}
			return false;
		}
		++m_pos;
		if (!ParseName(outNode.name)) {
			if (outError != nullptr) {
				*outError = "xml_tag_name_invalid";
			}
			return false;
		}

		bool selfClosing = false;
		if (!ParseAttributes(outNode.attributes, selfClosing, outError)) {
			return false;
		}
		if (selfClosing) {
			return true;
		}

		while (m_pos < m_text.size()) {
			SkipWhitespace();
			if (StartsWith(std::string_view(m_text).substr(m_pos), "</")) {
				m_pos += 2;
				std::string closeName;
				if (!ParseName(closeName) || closeName != outNode.name) {
					if (outError != nullptr) {
						*outError = "xml_close_tag_invalid";
					}
					return false;
				}
				SkipWhitespace();
				if (m_pos >= m_text.size() || m_text[m_pos] != '>') {
					if (outError != nullptr) {
						*outError = "xml_close_tag_end_missing";
					}
					return false;
				}
				++m_pos;
				return true;
			}
			if (m_pos < m_text.size() && m_text[m_pos] == '<') {
				XmlNode child;
				if (!ParseNode(child, outError)) {
					return false;
				}
				outNode.children.push_back(std::move(child));
				continue;
			}
			while (m_pos < m_text.size() && m_text[m_pos] != '<') {
				++m_pos;
			}
		}

		if (outError != nullptr) {
			*outError = "xml_close_tag_missing";
		}
		return false;
	}

	const std::string& m_text;
	size_t m_pos = 0;
};

struct ParsedVariableDef {
	std::string name;
	std::string typeName;
	std::string flagsText;
	std::string arrayText;
	std::string comment;
};

struct ParsedMethodDef {
	std::string name;
	std::string returnTypeName;
	bool isPublic = false;
	std::string comment;
	size_t bodyStartLineIndex = 0;
	std::vector<ParsedVariableDef> params;
	std::vector<ParsedVariableDef> locals;
	std::vector<std::string> bodyLines;
};

std::int32_t ComputeDefaultMethodAttr(const ParsedMethodDef& method);

struct ParsedClassDef {
	std::string name;
	std::string sourcePath;
	std::string baseClassName;
	bool isPublic = false;
	bool isFormClass = false;
	bool isUserClass = false;
	std::string comment;
	std::vector<ParsedVariableDef> vars;
	std::vector<ParsedMethodDef> methods;
};

struct ParsedStructDef {
	std::string name;
	bool isPublic = false;
	std::string comment;
	std::vector<ParsedVariableDef> members;
};

struct ParsedDllDef {
	std::string name;
	std::string returnTypeName;
	std::string fileName;
	std::string commandName;
	bool isPublic = false;
	std::string comment;
	std::vector<ParsedVariableDef> params;
};

struct ParsedConstantDef {
	std::string name;
	std::string valueText;
	bool isLongText = false;
	bool isPublic = false;
	std::string comment;
};

struct ParsedFormDef {
	std::string name;
	std::string comment;
	const FormXml* formXml = nullptr;
};

size_t NormalizeErrorLineIndex(const size_t preferredIndex, const size_t lineCount)
{
	if (lineCount == 0) {
		return 0;
	}
	return (std::min)(preferredIndex, lineCount - 1);
}

int ToDisplayLineNumber(const size_t lineIndex)
{
	return static_cast<int>(lineIndex) + 1;
}

std::string BuildSourceLocationLabel(
	const std::string& sourcePath,
	const std::string& pageType,
	const std::string& pageName)
{
	if (!sourcePath.empty()) {
		return sourcePath;
	}
	if (!pageType.empty() && !pageName.empty()) {
		return pageType + ":" + pageName;
	}
	return pageName;
}

std::string FormatPageSyntaxError(
	const Page& page,
	const size_t lineIndex,
	const std::string& detail,
	const std::string& methodName = std::string())
{
	std::ostringstream stream;
	stream << "source_syntax_error: file="
		<< BuildSourceLocationLabel(page.sourcePath, page.typeName, page.name)
		<< ", line=" << ToDisplayLineNumber(lineIndex)
		<< ", page_type=" << page.typeName
		<< ", page_name=" << page.name;
	if (!methodName.empty()) {
		stream << ", method=" << methodName;
	}
	if (!detail.empty()) {
		stream << ", detail=" << detail;
	}
	return stream.str();
}

std::string FormatFormXmlSyntaxError(
	const FormXml& formXml,
	const size_t lineIndex,
	const std::string& detail)
{
	std::ostringstream stream;
	stream << "xml_syntax_error: file="
		<< BuildSourceLocationLabel(formXml.sourcePath, "窗口XML", formXml.name)
		<< ", line=" << ToDisplayLineNumber(lineIndex)
		<< ", form_name=" << formXml.name;
	if (!detail.empty()) {
		stream << ", detail=" << detail;
	}
	return stream.str();
}

std::string ComputeParsedVariableDigest(const ParsedVariableDef& variable)
{
	std::ostringstream stream;
	stream << "name=" << variable.name << "\n";
	stream << "type=" << variable.typeName << "\n";
	stream << "flags=" << variable.flagsText << "\n";
	stream << "array=" << variable.arrayText << "\n";
	stream << "comment=" << variable.comment;
	return ComputeTextDigest(stream.str());
}

std::string ComputeParsedMethodDigest(const ParsedMethodDef& method)
{
	std::ostringstream stream;
	stream << "name=" << method.name << "\n";
	stream << "return=" << method.returnTypeName << "\n";
	stream << "public=" << (method.isPublic ? 1 : 0) << "\n";
	stream << "comment=" << method.comment << "\n";
	stream << "params=" << method.params.size() << "\n";
	for (const auto& item : method.params) {
		stream << ComputeParsedVariableDigest(item) << "\n";
	}
	stream << "locals=" << method.locals.size() << "\n";
	for (const auto& item : method.locals) {
		stream << ComputeParsedVariableDigest(item) << "\n";
	}
	stream << "body=";
	bool firstBodyLine = true;
	for (const auto& line : method.bodyLines) {
		const std::string trimmed = TrimAsciiCopy(line);
		if (trimmed.empty() || (!trimmed.empty() && trimmed.front() == '\'')) {
			continue;
		}
		if (!firstBodyLine) {
			stream << "\r\n";
		}
		firstBodyLine = false;
		stream << line;
	}
	return ComputeTextDigest(stream.str());
}

std::string ComputeParsedClassShapeDigest(const ParsedClassDef& parsedClass)
{
	std::ostringstream stream;
	stream << "name=" << parsedClass.name << "\n";
	stream << "base=" << parsedClass.baseClassName << "\n";
	stream << "public=" << (parsedClass.isPublic ? 1 : 0) << "\n";
	stream << "comment=" << parsedClass.comment << "\n";
	stream << "vars=" << parsedClass.vars.size() << "\n";
	for (const auto& item : parsedClass.vars) {
		stream << ComputeParsedVariableDigest(item) << "\n";
	}
	return ComputeTextDigest(stream.str());
}

std::string ComputeParsedDllDigest(const ParsedDllDef& dll)
{
	std::ostringstream stream;
	stream << "name=" << dll.name << "\n";
	stream << "return=" << dll.returnTypeName << "\n";
	stream << "file=" << dll.fileName << "\n";
	stream << "command=" << dll.commandName << "\n";
	stream << "public=" << (dll.isPublic ? 1 : 0) << "\n";
	stream << "comment=" << dll.comment << "\n";
	stream << "params=" << dll.params.size() << "\n";
	for (const auto& item : dll.params) {
		stream << ComputeParsedVariableDigest(item) << "\n";
	}
	return ComputeTextDigest(stream.str());
}

std::string ComputeParsedStructDigest(const ParsedStructDef& item)
{
	std::ostringstream stream;
	stream << "name=" << item.name << "\n";
	stream << "public=" << (item.isPublic ? 1 : 0) << "\n";
	stream << "comment=" << item.comment << "\n";
	stream << "members=" << item.members.size() << "\n";
	for (const auto& member : item.members) {
		stream << ComputeParsedVariableDigest(member) << "\n";
	}
	return ComputeTextDigest(stream.str());
}

std::string ComputeParsedConstantDigest(const ParsedConstantDef& item)
{
	std::ostringstream stream;
	stream << "name=" << item.name << "\n";
	stream << "value=" << item.valueText << "\n";
	stream << "longText=" << (item.isLongText ? 1 : 0) << "\n";
	stream << "public=" << (item.isPublic ? 1 : 0) << "\n";
	stream << "comment=" << item.comment;
	return ComputeTextDigest(stream.str());
}

std::string ComputeBundleResourceDigest(const BundleBinaryResource& resource)
{
	const std::string dataDigest = ComputeTextDigest(std::string(
		reinterpret_cast<const char*>(resource.data.data()),
		resource.data.size()));
	std::ostringstream stream;
	stream << "pageType=" << (resource.kind == BundleResourceKind::Image ? kConstPageImage : kConstPageSound) << "\n";
	stream << "name=" << resource.logicalName << "\n";
	stream << "public=" << (resource.isPublic ? 1 : 0) << "\n";
	stream << "comment=" << resource.comment << "\n";
	stream << "data=" << dataDigest;
	return ComputeTextDigest(stream.str());
}

bool IsLikelyFormClassName(const std::string& rawName)
{
	return StartsWith(TypeResolver::NormalizeTypeName(rawName), "窗口程序集");
}

bool ParseDefinitionFields(
	const std::string& line,
	const std::string& keyword,
	std::vector<std::string>& outFields)
{
	const std::string prefix = "." + keyword;
	if (!StartsWith(line, prefix)) {
		return false;
	}
	std::string rest = TrimAsciiCopy(line.substr(prefix.size()));
	if (rest.empty()) {
		outFields.clear();
		return true;
	}
	outFields = SplitTopLevelCommaFields(rest);
	return true;
}

std::string GetFieldOrEmpty(const std::vector<std::string>& fields, const size_t index)
{
	return index < fields.size() ? fields[index] : std::string();
}

std::string JoinRemainingFields(const std::vector<std::string>& fields, const size_t startIndex)
{
	if (startIndex >= fields.size()) {
		return std::string();
	}

	std::string text = fields[startIndex];
	for (size_t index = startIndex + 1; index < fields.size(); ++index) {
		text += ", ";
		text += fields[index];
	}
	return text;
}

std::string ExtractRemainingDefinitionFieldText(
	const std::string& line,
	const std::string& keyword,
	const size_t startFieldIndex)
{
	const std::string prefix = "." + keyword;
	if (!StartsWith(line, prefix)) {
		return std::string();
	}

	const std::string rest = TrimAsciiCopy(line.substr(prefix.size()));
	if (rest.empty()) {
		return std::string();
	}
	if (startFieldIndex == 0) {
		return rest;
	}

	size_t currentFieldIndex = 0;
	bool inQuote = false;
	for (size_t index = 0; index < rest.size(); ++index) {
		const char ch = rest[index];
		if (ch == '"') {
			inQuote = !inQuote;
			continue;
		}
		if (ch == ',' && !inQuote) {
			++currentFieldIndex;
			if (currentFieldIndex == startFieldIndex) {
				return TrimAsciiCopy(rest.substr(index + 1));
			}
		}
	}
	return std::string();
}

std::vector<std::int32_t> ParseArrayBounds(const std::string& text)
{
	std::vector<std::int32_t> bounds;
	const std::string raw = Unquote(TrimAsciiCopy(text));
	if (raw.empty()) {
		return bounds;
	}

	size_t start = 0;
	while (start <= raw.size()) {
		const size_t commaPos = raw.find(',', start);
		const std::string part = raw.substr(start, commaPos == std::string::npos ? std::string::npos : commaPos - start);
		if (TrimAsciiCopy(part).empty()) {
			bounds.push_back(0);
		}
		else {
			std::int32_t value = 0;
			bounds.push_back(TryParseInt32(part, value) ? value : 0);
		}
		if (commaPos == std::string::npos) {
			break;
		}
		start = commaPos + 1;
	}
	return bounds;
}

bool HasWordFlag(const std::string& flagsText, const std::string& word)
{
	if (flagsText.empty()) {
		return false;
	}
	std::istringstream stream(flagsText);
	std::string token;
	while (stream >> token) {
		if (token == word) {
			return true;
		}
	}
	return false;
}

bool ParseProgramPage(const Page& page, const std::unordered_set<std::string>& formNames, ParsedClassDef& outClass, std::string* outError)
{
	outClass = {};
	size_t index = 0;
	while (index < page.lines.size() && TrimAsciiCopy(page.lines[index]) != ".版本 2") {
		++index;
	}
	if (index < page.lines.size()) {
		++index;
	}
	while (index < page.lines.size()) {
		const std::string trimmed = TrimAsciiCopy(page.lines[index]);
		if (trimmed.empty() || StartsWith(trimmed, ".支持库 ")) {
			++index;
			continue;
		}
		break;
	}

	std::vector<std::string> fields;
	if (index >= page.lines.size() || !ParseDefinitionFields(TrimAsciiCopy(page.lines[index]), "程序集", fields)) {
		if (outError != nullptr) {
			const size_t lineIndex = page.lines.empty() ? 0 : NormalizeErrorLineIndex(index, page.lines.size());
			*outError = FormatPageSyntaxError(page, lineIndex, "program_page_header_missing");
		}
		return false;
	}

	outClass.sourcePath = page.sourcePath;
	outClass.name = fields.size() > 0 ? fields[0] : page.name;
	outClass.baseClassName = GetFieldOrEmpty(fields, 1);
	outClass.isPublic = GetFieldOrEmpty(fields, 2) == "公开";
	outClass.comment = ExtractRemainingDefinitionFieldText(TrimAsciiCopy(page.lines[index]), "程序集", 3);
	outClass.isUserClass = TypeResolver::NormalizeTypeName(outClass.baseClassName) == "对象";
	outClass.isFormClass =
		formNames.contains(outClass.name) ||
		TypeResolver::NormalizeTypeName(outClass.baseClassName) == "窗口" ||
		IsLikelyFormClassName(outClass.name);
	++index;

	while (index < page.lines.size()) {
		const std::string trimmed = TrimAsciiCopy(page.lines[index]);
		if (trimmed.empty()) {
			++index;
			continue;
		}
		if (StartsWith(trimmed, ".程序集变量")) {
			if (!ParseDefinitionFields(trimmed, "程序集变量", fields)) {
				++index;
				continue;
			}
			ParsedVariableDef variable;
			variable.name = GetFieldOrEmpty(fields, 0);
			variable.typeName = GetFieldOrEmpty(fields, 1);
			variable.arrayText = GetFieldOrEmpty(fields, 3);
			variable.comment = ExtractRemainingDefinitionFieldText(trimmed, "程序集变量", 4);
			outClass.vars.push_back(std::move(variable));
			++index;
			continue;
		}
		if (StartsWith(trimmed, ".子程序")) {
			ParsedMethodDef method;
			ParseDefinitionFields(trimmed, "子程序", fields);
			method.name = GetFieldOrEmpty(fields, 0);
			method.returnTypeName = GetFieldOrEmpty(fields, 1);
			method.isPublic = GetFieldOrEmpty(fields, 2) == "公开";
			method.comment = ExtractRemainingDefinitionFieldText(trimmed, "子程序", 3);
			++index;

			while (index < page.lines.size()) {
				const std::string line = TrimAsciiCopy(page.lines[index]);
				if (StartsWith(line, ".参数")) {
					ParseDefinitionFields(line, "参数", fields);
					ParsedVariableDef variable;
					variable.name = GetFieldOrEmpty(fields, 0);
					variable.typeName = GetFieldOrEmpty(fields, 1);
					variable.flagsText = GetFieldOrEmpty(fields, 2);
					variable.comment = ExtractRemainingDefinitionFieldText(line, "参数", 3);
					method.params.push_back(std::move(variable));
					++index;
					continue;
				}
				if (StartsWith(line, ".局部变量")) {
					ParseDefinitionFields(line, "局部变量", fields);
					ParsedVariableDef variable;
					variable.name = GetFieldOrEmpty(fields, 0);
					variable.typeName = GetFieldOrEmpty(fields, 1);
					variable.flagsText = GetFieldOrEmpty(fields, 2);
					variable.arrayText = GetFieldOrEmpty(fields, 3);
					variable.comment = ExtractRemainingDefinitionFieldText(line, "局部变量", 4);
					method.locals.push_back(std::move(variable));
					++index;
					continue;
				}
				break;
			}

			const size_t bodyStartIndex = index;
			method.bodyStartLineIndex = bodyStartIndex;
			while (index < page.lines.size()) {
				const std::string line = page.lines[index];
				const std::string trimmedLine = TrimAsciiCopy(line);
				if (StartsWith(trimmedLine, ".子程序")) {
					break;
				}
				method.bodyLines.push_back(line);
				++index;
			}

			if (!method.bodyLines.empty()) {
				RestoreMethod methodProbe;
				std::string methodError;
				size_t methodErrorLineIndex = 0;
				if (!BuildMethodCodeData(method.bodyLines, methodProbe, &methodError, &methodErrorLineIndex)) {
					if (outError != nullptr) {
						const size_t pageLineIndex =
							bodyStartIndex + NormalizeErrorLineIndex(methodErrorLineIndex, method.bodyLines.size());
						*outError = FormatPageSyntaxError(page, pageLineIndex, methodError, method.name);
					}
					return false;
				}
			}

			outClass.methods.push_back(std::move(method));
			continue;
		}
		++index;
	}
	return true;
}

void ParseGlobalPage(const Page& page, std::vector<ParsedVariableDef>& outGlobals)
{
	std::vector<std::string> fields;
	for (const auto& line : page.lines) {
		const std::string trimmed = TrimAsciiCopy(line);
		if (!StartsWith(trimmed, ".全局变量")) {
			continue;
		}
		ParseDefinitionFields(trimmed, "全局变量", fields);
		ParsedVariableDef variable;
		variable.name = GetFieldOrEmpty(fields, 0);
		variable.typeName = GetFieldOrEmpty(fields, 1);
		variable.flagsText = GetFieldOrEmpty(fields, 2);
		variable.arrayText = GetFieldOrEmpty(fields, 3);
		variable.comment = ExtractRemainingDefinitionFieldText(trimmed, "全局变量", 4);
		outGlobals.push_back(std::move(variable));
	}
}

void ParseStructPage(const Page& page, std::vector<ParsedStructDef>& outStructs)
{
	std::vector<std::string> fields;
	ParsedStructDef* current = nullptr;
	for (const auto& line : page.lines) {
		const std::string trimmed = TrimAsciiCopy(line);
		if (StartsWith(trimmed, ".数据类型")) {
			ParseDefinitionFields(trimmed, "数据类型", fields);
			ParsedStructDef item;
			item.name = GetFieldOrEmpty(fields, 0);
			item.isPublic = GetFieldOrEmpty(fields, 1) == "公开";
			item.comment = ExtractRemainingDefinitionFieldText(trimmed, "数据类型", 2);
			outStructs.push_back(std::move(item));
			current = &outStructs.back();
			continue;
		}
		if (current != nullptr && StartsWith(trimmed, ".成员")) {
			ParseDefinitionFields(trimmed, "成员", fields);
			ParsedVariableDef member;
			member.name = GetFieldOrEmpty(fields, 0);
			member.typeName = GetFieldOrEmpty(fields, 1);
			member.flagsText = GetFieldOrEmpty(fields, 2);
			member.arrayText = GetFieldOrEmpty(fields, 3);
			member.comment = ExtractRemainingDefinitionFieldText(trimmed, "成员", 4);
			current->members.push_back(std::move(member));
		}
	}
}

void ParseDllPage(const Page& page, std::vector<ParsedDllDef>& outDlls)
{
	std::vector<std::string> fields;
	ParsedDllDef* current = nullptr;
	for (const auto& line : page.lines) {
		const std::string trimmed = TrimAsciiCopy(line);
		if (StartsWith(trimmed, ".DLL命令")) {
			ParseDefinitionFields(trimmed, "DLL命令", fields);
			ParsedDllDef dll;
			dll.name = GetFieldOrEmpty(fields, 0);
			dll.returnTypeName = GetFieldOrEmpty(fields, 1);
			dll.fileName = Unquote(GetFieldOrEmpty(fields, 2));
			dll.commandName = Unquote(GetFieldOrEmpty(fields, 3));
			dll.isPublic = GetFieldOrEmpty(fields, 4) == "公开";
			dll.comment = ExtractRemainingDefinitionFieldText(trimmed, "DLL命令", 5);
			outDlls.push_back(std::move(dll));
			current = &outDlls.back();
			continue;
		}
		if (current != nullptr && StartsWith(trimmed, ".参数")) {
			ParseDefinitionFields(trimmed, "参数", fields);
			ParsedVariableDef param;
			param.name = GetFieldOrEmpty(fields, 0);
			param.typeName = GetFieldOrEmpty(fields, 1);
			param.flagsText = GetFieldOrEmpty(fields, 2);
			param.comment = ExtractRemainingDefinitionFieldText(trimmed, "参数", 3);
			current->params.push_back(std::move(param));
		}
	}
}

void ParseConstantPage(const Page& page, std::vector<ParsedConstantDef>& outConstants)
{
	std::vector<std::string> fields;
	for (const auto& line : page.lines) {
		const std::string trimmed = TrimAsciiCopy(line);
		if (!StartsWith(trimmed, ".常量")) {
			continue;
		}
		ParseDefinitionFields(trimmed, "常量", fields);
		ParsedConstantDef item;
		item.name = GetFieldOrEmpty(fields, 0);
		item.valueText = Unquote(GetFieldOrEmpty(fields, 1));
		std::string decodedText;
		bool decodedLongText = false;
		if (TryDecodeDumpTextLiteral(item.valueText, decodedText, decodedLongText)) {
			item.isLongText = decodedLongText;
		}
		else if (StartsWith(item.valueText, "<文本长度:") && EndsWith(item.valueText, ">")) {
			item.isLongText = true;
		}
		(void)decodedText;
		item.isPublic = GetFieldOrEmpty(fields, 3) == "公开";
		item.comment = ExtractRemainingDefinitionFieldText(trimmed, "常量", 4);
		outConstants.push_back(std::move(item));
	}
}

void ParseWindowPage(const Page& page, std::vector<ParsedFormDef>& outForms)
{
	std::vector<std::string> fields;
	for (const auto& line : page.lines) {
		const std::string trimmed = TrimAsciiCopy(line);
		if (!StartsWith(trimmed, ".窗口")) {
			continue;
		}
		ParseDefinitionFields(trimmed, "窗口", fields);
		ParsedFormDef item;
		item.name = GetFieldOrEmpty(fields, 0);
		item.comment = ExtractRemainingDefinitionFieldText(trimmed, "窗口", 1);
		outForms.push_back(std::move(item));
	}
}

std::string GetXmlAttribute(const XmlNode& node, const std::string& key)
{
	if (const auto it = node.attributes.find(key); it != node.attributes.end()) {
		return it->second;
	}
	return std::string();
}

std::int32_t GetXmlIntAttribute(const XmlNode& node, const std::string& key, const std::int32_t defaultValue)
{
	std::int32_t value = 0;
	return TryParseInt32(GetXmlAttribute(node, key), value) ? value : defaultValue;
}

bool GetXmlBoolAttribute(const XmlNode& node, const std::string& key, const bool defaultValue)
{
	const auto value = ParseBoolLiteral(GetXmlAttribute(node, key));
	return value.has_value() ? *value : defaultValue;
}

bool SplitQualifiedHandlerName(const std::string& rawText, std::string& outOwnerName, std::string& outMethodName)
{
	const std::string trimmed = TrimAsciiCopy(rawText);
	if (trimmed.empty()) {
		outOwnerName.clear();
		outMethodName.clear();
		return false;
	}

	const size_t sepPos = trimmed.rfind("::");
	if (sepPos == std::string::npos) {
		outOwnerName.clear();
		outMethodName = TypeResolver::NormalizeTypeName(trimmed);
		return !outMethodName.empty();
	}

	outOwnerName = TypeResolver::NormalizeTypeName(trimmed.substr(0, sepPos));
	outMethodName = TypeResolver::NormalizeTypeName(trimmed.substr(sepPos + 2));
	return !outMethodName.empty();
}

std::int32_t ResolveHandlerMethodId(
	const std::string& rawHandlerName,
	const std::int32_t preferredOwnerClassId,
	const RestoreDocumentModel& model)
{
	std::string ownerName;
	std::string methodName;
	if (!SplitQualifiedHandlerName(rawHandlerName, ownerName, methodName)) {
		return 0;
	}

	std::int32_t resolvedOwnerClassId = 0;
	if (!ownerName.empty()) {
		for (const auto& item : model.classes) {
			if (TypeResolver::NormalizeTypeName(item.name) == ownerName) {
				resolvedOwnerClassId = item.id;
				break;
			}
		}
	}

	if (resolvedOwnerClassId == 0 && preferredOwnerClassId != 0) {
		for (const auto& method : model.methods) {
			if (method.ownerClass == preferredOwnerClassId &&
				TypeResolver::NormalizeTypeName(method.name) == methodName) {
				return method.id;
			}
		}
	}

	if (resolvedOwnerClassId != 0) {
		for (const auto& method : model.methods) {
			if (method.ownerClass == resolvedOwnerClassId &&
				TypeResolver::NormalizeTypeName(method.name) == methodName) {
				return method.id;
			}
		}
	}

	std::int32_t uniqueMatch = 0;
	for (const auto& method : model.methods) {
		if (TypeResolver::NormalizeTypeName(method.name) != methodName) {
			continue;
		}
		if (uniqueMatch != 0) {
			return 0;
		}
		uniqueMatch = method.id;
	}
	return uniqueMatch;
}

std::vector<std::pair<std::int32_t, std::int32_t>> ReadFormControlEventsFromXml(
	const XmlNode& node,
	const std::string& eventNodeName,
	const std::int32_t preferredOwnerClassId,
	const RestoreDocumentModel& model)
{
	std::vector<std::pair<std::int32_t, std::int32_t>> events;
	for (const auto& child : node.children) {
		if (child.name != eventNodeName) {
			continue;
		}
		const std::int32_t eventKey = GetXmlIntAttribute(child, "索引", -1);
		if (eventKey < 0) {
			continue;
		}
		const std::int32_t handlerId = ResolveHandlerMethodId(GetXmlAttribute(child, "处理器"), preferredOwnerClassId, model);
		events.emplace_back(eventKey, handlerId);
	}
	return events;
}

std::int32_t ReadFormMenuClickEventFromXml(
	const XmlNode& node,
	const std::int32_t preferredOwnerClassId,
	const RestoreDocumentModel& model)
{
	for (const auto& child : node.children) {
		if (child.name == "菜单.事件") {
			return ResolveHandlerMethodId(GetXmlAttribute(child, "处理器"), preferredOwnerClassId, model);
		}
	}
	return 0;
}

std::int16_t BuildVariableAttr(const ParsedVariableDef& definition, const bool allowStatic, const bool allowPublic)
{
	std::int16_t attr = 0;
	if (allowStatic && HasWordFlag(definition.flagsText, "静态")) {
		attr |= kVarAttrStatic;
	}
	if (HasWordFlag(definition.flagsText, "参考") || HasWordFlag(definition.flagsText, "传址")) {
		attr |= kVarAttrByRef;
	}
	if (HasWordFlag(definition.flagsText, "可空")) {
		attr |= kVarAttrNullable;
	}
	if (HasWordFlag(definition.flagsText, "数组")) {
		attr |= kVarAttrArray;
	}
	if (allowPublic && HasWordFlag(definition.flagsText, "公开")) {
		attr |= kGlobalAttrPublic;
	}
	if (!definition.arrayText.empty()) {
		attr |= kVarAttrArray;
	}
	return attr;
}

std::int32_t ResolveFormElementTypeId(const std::string& tagName, TypeResolver& resolver)
{
	const std::string normalized = TypeResolver::NormalizeTypeName(tagName);
	if (StartsWith(normalized, "未知类型.Lib")) {
		const size_t dotPos = normalized.find('.', std::string("未知类型.Lib").size());
		if (dotPos != std::string::npos) {
			std::int32_t libraryId = 0;
			std::int32_t typeId = 0;
			if (TryParseInt32(normalized.substr(std::string("未知类型.Lib").size(), dotPos - std::string("未知类型.Lib").size()), libraryId) &&
				TryParseInt32(normalized.substr(dotPos + 1), typeId)) {
				return ((libraryId + 1) << 16) | (typeId + 1);
			}
		}
	}
	if (StartsWith(normalized, "未知类型.")) {
		std::int32_t rawType = 0;
		if (TryParseInt32(normalized.substr(std::string("未知类型.").size()), rawType)) {
			return rawType;
		}
	}
	return resolver.ResolveTypeId(normalized);
}

void BuildFormControlTree(
	const XmlNode& node,
	const std::int32_t parentId,
	const std::int32_t preferredOwnerClassId,
	const RestoreDocumentModel& model,
	TypeResolver& resolver,
	IdAllocator& allocator,
	std::vector<RestoreFormElement>& outElements,
	std::vector<std::int32_t>& outChildren)
{
	RestoreFormElement element;
	element.id = allocator.Alloc(epl_system_id::kTypeFormControl);
	element.dataType = ResolveFormElementTypeId(node.name, resolver);
	element.name = GetXmlAttribute(node, "名称");
	element.comment = GetXmlAttribute(node, "备注");
	element.parent = parentId;
	element.left = GetXmlIntAttribute(node, "左边", 0);
	element.top = GetXmlIntAttribute(node, "顶边", 0);
	element.width = GetXmlIntAttribute(node, "宽度", 0);
	element.height = GetXmlIntAttribute(node, "高度", 0);
	element.tag = GetXmlAttribute(node, "标记");
	element.disable = GetXmlBoolAttribute(node, "禁止", false);
	element.visible = GetXmlBoolAttribute(node, "可视", true);
	element.cursor = DecodeBase64(GetXmlAttribute(node, "鼠标指针"));
	element.tabStop = GetXmlBoolAttribute(node, "可停留焦点", true);
	element.tabIndex = GetXmlIntAttribute(node, "停留顺序", 0);
	element.extensionData = DecodeBase64(GetXmlAttribute(node, "扩展属性数据"));
	element.events = ReadFormControlEventsFromXml(node, node.name + ".事件", preferredOwnerClassId, model);

	std::vector<std::int32_t> childIds;
	const bool isTabControl = resolver.IsTabControlType(element.dataType);
	if (isTabControl) {
		bool firstTab = true;
		for (const auto& child : node.children) {
			if (child.name != node.name + ".子夹") {
				continue;
			}
			if (!firstTab) {
				childIds.push_back(0);
			}
			firstTab = false;
			for (const auto& tabChild : child.children) {
				if (StartsWith(tabChild.name, node.name + ".")) {
					continue;
				}
				BuildFormControlTree(tabChild, element.id, preferredOwnerClassId, model, resolver, allocator, outElements, childIds);
			}
		}
	}
	else {
		for (const auto& child : node.children) {
			if (StartsWith(child.name, node.name + ".")) {
				continue;
			}
			BuildFormControlTree(child, element.id, preferredOwnerClassId, model, resolver, allocator, outElements, childIds);
		}
	}
	element.children = std::move(childIds);
	outChildren.push_back(element.id);
	outElements.push_back(std::move(element));
}

void BuildFormMenus(
	const XmlNode& node,
	const int level,
	const std::int32_t preferredOwnerClassId,
	const RestoreDocumentModel& model,
	IdAllocator& allocator,
	std::vector<RestoreFormElement>& outElements)
{
	for (const auto& child : node.children) {
		if (child.name != "菜单") {
			continue;
		}
		RestoreFormElement element;
		element.id = allocator.Alloc(epl_system_id::kTypeFormMenu);
		element.dataType = 65539;
		element.isMenu = true;
		element.name = GetXmlAttribute(child, "名称");
		element.text = GetXmlAttribute(child, "标题");
		element.visible = GetXmlBoolAttribute(child, "可视", true);
		element.disable = GetXmlBoolAttribute(child, "禁止", false);
		element.selected = GetXmlBoolAttribute(child, "选中", false);
		element.hotKey = GetXmlIntAttribute(child, "快捷键", 0);
		element.level = level;
		element.clickEvent = ReadFormMenuClickEventFromXml(child, preferredOwnerClassId, model);
		outElements.push_back(std::move(element));
		BuildFormMenus(child, level + 1, preferredOwnerClassId, model, allocator, outElements);
	}
}

bool BuildFormsFromXml(
	const std::vector<ParsedFormDef>& parsedForms,
	const std::unordered_map<std::string, std::int32_t>& formClassIds,
	const std::unordered_map<std::string, std::int32_t>& preferredFormIds,
	const RestoreDocumentModel& model,
	TypeResolver& resolver,
	IdAllocator& allocator,
	std::vector<RestoreForm>& outForms,
	std::string* outError)
{
	outForms.clear();
	for (const auto& formDef : parsedForms) {
		RestoreForm form;
		const std::string normalizedFormName = TypeResolver::NormalizeTypeName(formDef.name);
		if (const auto preferredIt = preferredFormIds.find(normalizedFormName);
			preferredIt != preferredFormIds.end() && preferredIt->second != 0) {
			form.id = preferredIt->second;
			allocator.Observe(form.id);
		}
		else {
			form.id = allocator.Alloc(epl_system_id::kTypeForm);
		}
		form.classId = 0;
		if (const auto it = formClassIds.find(normalizedFormName); it != formClassIds.end()) {
			form.classId = it->second;
		}
		form.name = formDef.name;
		form.comment = formDef.comment;

		RestoreFormElement selfElement;
		selfElement.id = allocator.Alloc(epl_system_id::kTypeFormSelf);
		selfElement.dataType = 65537;

		if (formDef.formXml != nullptr) {
			const std::string xmlText = [&]() {
				std::ostringstream stream;
				for (size_t i = 0; i < formDef.formXml->lines.size(); ++i) {
					if (i != 0) {
						stream << "\n";
					}
					stream << formDef.formXml->lines[i];
				}
				return stream.str();
			}();

			XmlNode root;
			SimpleXmlParser parser(xmlText);
			if (!parser.Parse(root, outError)) {
				if (outError != nullptr) {
					*outError = FormatFormXmlSyntaxError(
						*formDef.formXml,
						parser.CurrentLineIndex(),
						*outError);
				}
				return false;
			}
			form.name = GetXmlAttribute(root, "名称").empty() ? form.name : GetXmlAttribute(root, "名称");
			form.comment = GetXmlAttribute(root, "备注").empty() ? form.comment : GetXmlAttribute(root, "备注");
			selfElement.left = GetXmlIntAttribute(root, "左边", 0);
			selfElement.top = GetXmlIntAttribute(root, "顶边", 0);
			selfElement.width = GetXmlIntAttribute(root, "宽度", 0);
			selfElement.height = GetXmlIntAttribute(root, "高度", 0);
			selfElement.tag = GetXmlAttribute(root, "标记");
			selfElement.disable = GetXmlBoolAttribute(root, "禁止", false);
			selfElement.visible = GetXmlBoolAttribute(root, "可视", true);
			selfElement.cursor = DecodeBase64(GetXmlAttribute(root, "鼠标指针"));
			selfElement.tabStop = GetXmlBoolAttribute(root, "可停留焦点", true);
			selfElement.tabIndex = GetXmlIntAttribute(root, "停留顺序", 0);
			selfElement.extensionData = DecodeBase64(GetXmlAttribute(root, "扩展属性数据"));
			selfElement.events = ReadFormControlEventsFromXml(root, "窗口.事件", form.classId, model);

			form.elements.push_back(selfElement);
			for (const auto& child : root.children) {
				if (child.name == "窗口.菜单") {
					BuildFormMenus(child, 0, form.classId, model, allocator, form.elements);
				}
			}
			std::vector<std::int32_t> rootChildren;
			for (const auto& child : root.children) {
				if (child.name == "窗口.菜单" || StartsWith(child.name, root.name + ".")) {
					continue;
				}
				BuildFormControlTree(child, 0, form.classId, model, resolver, allocator, form.elements, rootChildren);
			}
		}
		else {
			form.elements.push_back(selfElement);
		}
		outForms.push_back(std::move(form));
	}
	return true;
}

std::unordered_map<std::string, size_t> BuildFormClassMatchTable(
	const std::vector<ParsedFormDef>& parsedForms,
	const std::vector<ParsedClassDef>& parsedClasses)
{
	std::unordered_map<std::string, size_t> matches;
	std::vector<bool> classAssigned(parsedClasses.size(), false);

	const auto tryMatch = [&](const size_t formIndex, const std::string& candidateClassName) -> bool {
		const std::string normalizedFormName = TypeResolver::NormalizeTypeName(parsedForms[formIndex].name);
		if (normalizedFormName.empty()) {
			return false;
		}

		const std::string normalizedCandidate = TypeResolver::NormalizeTypeName(candidateClassName);
		if (normalizedCandidate.empty()) {
			return false;
		}

		for (size_t classIndex = 0; classIndex < parsedClasses.size(); ++classIndex) {
			if (classAssigned[classIndex] || !parsedClasses[classIndex].isFormClass) {
				continue;
			}
			if (TypeResolver::NormalizeTypeName(parsedClasses[classIndex].name) != normalizedCandidate) {
				continue;
			}
			classAssigned[classIndex] = true;
			matches.insert_or_assign(normalizedFormName, classIndex);
			return true;
		}
		return false;
	};

	for (size_t formIndex = 0; formIndex < parsedForms.size(); ++formIndex) {
		tryMatch(formIndex, parsedForms[formIndex].name);
	}

	for (size_t formIndex = 0; formIndex < parsedForms.size(); ++formIndex) {
		const std::string normalizedFormName = TypeResolver::NormalizeTypeName(parsedForms[formIndex].name);
		if (matches.contains(normalizedFormName)) {
			continue;
		}
		tryMatch(formIndex, "窗口程序集_" + parsedForms[formIndex].name);
	}

	for (size_t formIndex = 0; formIndex < parsedForms.size(); ++formIndex) {
		const std::string normalizedFormName = TypeResolver::NormalizeTypeName(parsedForms[formIndex].name);
		if (matches.contains(normalizedFormName)) {
			continue;
		}

		if (StartsWith(normalizedFormName, "窗口")) {
			tryMatch(formIndex, "窗口程序集" + normalizedFormName.substr(std::string("窗口").size()));
		}
	}

	std::vector<size_t> remainingForms;
	std::vector<size_t> remainingClasses;
	for (size_t formIndex = 0; formIndex < parsedForms.size(); ++formIndex) {
		const std::string normalizedFormName = TypeResolver::NormalizeTypeName(parsedForms[formIndex].name);
		if (!matches.contains(normalizedFormName)) {
			remainingForms.push_back(formIndex);
		}
	}
	for (size_t classIndex = 0; classIndex < parsedClasses.size(); ++classIndex) {
		if (!classAssigned[classIndex] && parsedClasses[classIndex].isFormClass) {
			remainingClasses.push_back(classIndex);
		}
	}

	if (remainingForms.size() == remainingClasses.size()) {
		for (size_t i = 0; i < remainingForms.size(); ++i) {
			const size_t formIndex = remainingForms[i];
			const size_t classIndex = remainingClasses[i];
			matches.insert_or_assign(TypeResolver::NormalizeTypeName(parsedForms[formIndex].name), classIndex);
			classAssigned[classIndex] = true;
		}
	}

	return matches;
}

std::string BuildBundleItemKey(
	const std::string& prefix,
	const std::string& rawName,
	std::unordered_map<std::string, int>& counters)
{
	std::string logicalName = TypeResolver::NormalizeTypeName(rawName);
	if (logicalName.empty()) {
		logicalName = prefix;
	}

	const std::string baseKey = prefix + ":" + logicalName;
	int& counter = counters[baseKey];
	++counter;
	if (counter == 1) {
		return baseKey;
	}
	return baseKey + "#" + std::to_string(counter);
}

void AppendBundlePage(
	Document& document,
	const std::string& typeName,
	const std::string& pageName,
	const std::string& sourcePath,
	const std::string& text)
{
	if (TrimAsciiCopy(text).empty()) {
		return;
	}

	Page page;
	page.typeName = typeName;
	page.name = pageName;
	page.sourcePath = sourcePath;
	page.lines = SplitLines(RemoveUtf8Bom(text));
	document.pages.push_back(std::move(page));
}

Document BuildDocumentFromBundle(const ProjectBundle& bundle)
{
	Document document;
	document.sourcePath = bundle.sourcePath;
	document.projectName = bundle.projectName;
	document.versionText = bundle.versionText;
	document.dependencies = bundle.dependencies;

	for (const auto& file : bundle.sourceFiles) {
		Page page;
		page.typeName = "程序集";
		page.name = file.logicalName;
		page.sourcePath = file.relativePath;
		page.lines = SplitLines(RemoveUtf8Bom(file.content));
		document.pages.push_back(std::move(page));
	}

	AppendBundlePage(document, "全局变量", "全局变量", "src/.全局变量.txt", bundle.globalText);
	AppendBundlePage(document, "自定义数据类型", "自定义数据类型", "src/.数据类型.txt", bundle.dataTypeText);
	AppendBundlePage(document, "DLL命令", "Dll命令", "src/.DLL声明.txt", bundle.dllDeclareText);
	AppendBundlePage(document, "常量资源", "常量表...", "src/.常量.txt", bundle.constantText);

	if (!bundle.formFiles.empty()) {
		Page page;
		page.typeName = "窗口/表单";
		page.name = "窗口";
		page.lines.push_back(".版本 2");
		page.lines.push_back("");
		for (const auto& file : bundle.formFiles) {
			page.lines.push_back(".窗口 " + file.logicalName);
		}
		document.pages.push_back(std::move(page));
	}

	for (const auto& file : bundle.formFiles) {
		FormXml formXml;
		formXml.name = file.logicalName;
		formXml.sourcePath = file.relativePath;
		formXml.lines = SplitLines(RemoveUtf8Bom(file.xmlText));
		document.formXmls.push_back(std::move(formXml));
	}
	return document;
}

bool CanReuseNativeBytesForSemanticEquivalentSources(const ProjectBundle& bundle, const Document& document)
{
	if (bundle.nativeSourceBytes.empty() || bundle.nativeSourceSnapshots.empty()) {
		return false;
	}

	std::unordered_set<std::string> formNames;
	for (const auto& form : document.formXmls) {
		const std::string name = TypeResolver::NormalizeTypeName(form.name);
		if (!name.empty()) {
			formNames.insert(name);
		}
	}

	size_t classIndex = 0;
	for (const auto& page : document.pages) {
		if (page.typeName != "程序集") {
			continue;
		}
		if (classIndex >= bundle.nativeSourceSnapshots.size()) {
			return false;
		}

		ParsedClassDef parsedClass;
		std::string ignoredError;
		if (!ParseProgramPage(page, formNames, parsedClass, &ignoredError)) {
			return false;
		}

		const auto& snapshot = bundle.nativeSourceSnapshots[classIndex++];
		if (snapshot.classShapeDigest.empty() ||
			snapshot.classShapeDigest != ComputeParsedClassShapeDigest(parsedClass) ||
			snapshot.methods.size() != parsedClass.methods.size()) {
			return false;
		}
		for (size_t methodIndex = 0; methodIndex < parsedClass.methods.size(); ++methodIndex) {
			const auto& methodSnapshot = snapshot.methods[methodIndex];
			if (methodSnapshot.textDigest.empty() ||
				methodSnapshot.textDigest != ComputeParsedMethodDigest(parsedClass.methods[methodIndex])) {
				return false;
			}
		}
	}

	return classIndex == bundle.nativeSourceSnapshots.size();
}

void PushUniquePathCandidate(std::vector<std::filesystem::path>& outPaths, const std::filesystem::path& candidate)
{
	const auto normalized = candidate.lexically_normal();
	if (std::find(outPaths.begin(), outPaths.end(), normalized) == outPaths.end()) {
		outPaths.push_back(normalized);
	}
}

std::vector<std::filesystem::path> BuildDependencyModuleCandidatePaths(
	const std::string& sourcePath,
	const std::string& modulePathText)
{
	std::vector<std::filesystem::path> candidates;
	std::string normalizedText = TrimAsciiCopy(modulePathText);
	if (normalizedText.empty()) {
		return candidates;
	}

	if (normalizedText.size() >= 2 && normalizedText.front() == '"' && normalizedText.back() == '"') {
		normalizedText = normalizedText.substr(1, normalizedText.size() - 2);
	}
	if (!normalizedText.empty() && normalizedText.front() == '$') {
		normalizedText.erase(normalizedText.begin());
	}

	std::filesystem::path filePath(normalizedText);
	if (filePath.extension().empty()) {
		filePath += ".ec";
	}

	if (filePath.is_absolute()) {
		PushUniquePathCandidate(candidates, filePath);
		return candidates;
	}

	const auto addBaseCandidates = [&](const std::filesystem::path& baseDir) {
		if (baseDir.empty()) {
			return;
		}
		PushUniquePathCandidate(candidates, baseDir / filePath);
		PushUniquePathCandidate(candidates, baseDir / "ecom" / filePath);

		std::filesystem::path current = baseDir;
		while (!current.empty()) {
			PushUniquePathCandidate(candidates, current / "ecom" / filePath);
			if (current == current.root_path()) {
				break;
			}
			current = current.parent_path();
		}
	};

	std::error_code ec;
	if (!sourcePath.empty()) {
		addBaseCandidates(Utf8PathToPath(sourcePath).parent_path());
	}
	addBaseCandidates(std::filesystem::current_path(ec));
	addBaseCandidates(std::filesystem::path(GetBasePath()));
	for (const auto& registeredBaseDir : GetRegisteredEplOpenCommandBaseDirs()) {
		addBaseCandidates(registeredBaseDir);
	}

	return candidates;
}

bool ResolveDependencyModulePath(
	const std::string& sourcePath,
	const std::string& modulePathText,
	std::string& outResolvedPath)
{
	outResolvedPath.clear();
	for (const auto& candidate : BuildDependencyModuleCandidatePaths(sourcePath, modulePathText)) {
		std::error_code ec;
		if (!std::filesystem::exists(candidate, ec)) {
			continue;
		}
		outResolvedPath = candidate.string();
		return true;
	}
	return false;
}

bool BuildRestoreModel(
	const Document& document,
	const ProjectBundle* bundle,
	RestoreDocumentModel& outModel,
	std::string* outError,
	const ProjectBundle* originalBundle = nullptr)
{
	RestoreDocumentModel model;
	model.sourcePath = document.sourcePath;
	model.projectName = document.projectName.empty() ? "txt2e_project" : document.projectName;
	model.versionText = document.versionText.empty() ? "1.0" : document.versionText;
	for (const auto& dependency : document.dependencies) {
		RestoreDependencyInfo item;
		item.name = dependency.name;
		item.fileName = dependency.fileName;
		item.guid = dependency.guid;
		item.versionText = dependency.versionText;
		item.path = dependency.path;
		item.resolvedPath = dependency.resolvedPath;
		item.localWorkspace = dependency.localWorkspace;
		item.reExport = dependency.reExport;
		item.isSupportLibrary = dependency.kind == DependencyKind::ELib;
		item.definedIds.reserve(dependency.definedIds.size());
		for (const auto& range : dependency.definedIds) {
			if (range.count > 0) {
				item.definedIds.push_back(RestoreDependencyInfo::DefinedIdRange{
					range.start,
					range.count,
				});
			}
		}
		if (!item.isSupportLibrary && item.resolvedPath.empty()) {
			ResolveDependencyModulePath(document.sourcePath, item.path, item.resolvedPath);
		}
		model.dependencies.push_back(std::move(item));
	}
	if (bundle != nullptr) {
		ApplyNativeDependencyDefinedIds(*bundle, model.dependencies);
	}
	AssignDependencyChildIdSpans(model.dependencies);

	std::unordered_map<std::string, std::string> explicitWindowBindings;
	std::unordered_set<std::string> explicitFormClassNames;
	if (bundle != nullptr) {
		for (const auto& binding : bundle->windowBindings) {
			const std::string formName = TypeResolver::NormalizeTypeName(binding.formName);
			const std::string className = TypeResolver::NormalizeTypeName(binding.className);
			if (formName.empty() || className.empty()) {
				continue;
			}
			explicitWindowBindings.insert_or_assign(formName, className);
			explicitFormClassNames.insert(className);
		}
	}

	std::vector<ParsedClassDef> parsedClasses;
	std::vector<ParsedVariableDef> parsedGlobals;
	std::vector<ParsedStructDef> parsedStructs;
	std::vector<ParsedDllDef> parsedDlls;
	std::vector<ParsedConstantDef> parsedConstants;
	std::vector<ParsedFormDef> parsedForms;
	for (const auto& page : document.pages) {
		if (page.typeName == "窗口/表单") {
			ParseWindowPage(page, parsedForms);
		}
	}
	for (const auto& formXml : document.formXmls) {
		const std::string normalized = TypeResolver::NormalizeTypeName(formXml.name);
		auto it = std::find_if(
			parsedForms.begin(),
			parsedForms.end(),
			[&](const ParsedFormDef& item) { return TypeResolver::NormalizeTypeName(item.name) == normalized; });
		if (it == parsedForms.end()) {
			ParsedFormDef item;
			item.name = formXml.name;
			item.formXml = &formXml;
			parsedForms.push_back(item);
		}
		else {
			it->formXml = &formXml;
		}
	}

	std::unordered_set<std::string> formNames;
	for (const auto& form : parsedForms) {
		formNames.insert(TypeResolver::NormalizeTypeName(form.name));
	}

	for (const auto& page : document.pages) {
		if (page.typeName == "程序集") {
			ParsedClassDef parsedClass;
			if (!ParseProgramPage(page, formNames, parsedClass, outError)) {
				return false;
			}
			if (explicitFormClassNames.contains(TypeResolver::NormalizeTypeName(parsedClass.name))) {
				parsedClass.isFormClass = true;
			}
			parsedClasses.push_back(std::move(parsedClass));
		}
		else if (page.typeName == "全局变量") {
			ParseGlobalPage(page, parsedGlobals);
		}
		else if (page.typeName == "自定义数据类型") {
			ParseStructPage(page, parsedStructs);
		}
		else if (page.typeName == "DLL命令") {
			ParseDllPage(page, parsedDlls);
		}
		else if (page.typeName == "常量资源") {
			ParseConstantPage(page, parsedConstants);
		}
	}

	std::vector<ParsedClassDef> originalParsedClasses;
	if (originalBundle != nullptr && !originalBundle->sourceFiles.empty()) {
		originalParsedClasses.reserve(originalBundle->sourceFiles.size());
		for (const auto& file : originalBundle->sourceFiles) {
			Page originalPage;
			originalPage.typeName = "程序集";
			originalPage.name = file.logicalName;
			originalPage.sourcePath = file.relativePath;
			originalPage.lines = SplitLines(RemoveUtf8Bom(file.content));
			ParsedClassDef parsedClass;
			std::string ignoredError;
			if (!ParseProgramPage(originalPage, formNames, parsedClass, &ignoredError)) {
				originalParsedClasses.clear();
				break;
			}
			originalParsedClasses.push_back(std::move(parsedClass));
		}
	}

	auto formClassMatches = BuildFormClassMatchTable(parsedForms, parsedClasses);
	if (!explicitWindowBindings.empty()) {
		std::unordered_map<std::string, size_t> classIndexByName;
		for (size_t classIndex = 0; classIndex < parsedClasses.size(); ++classIndex) {
			classIndexByName.insert_or_assign(TypeResolver::NormalizeTypeName(parsedClasses[classIndex].name), classIndex);
		}
		for (const auto& [formName, className] : explicitWindowBindings) {
			if (const auto it = classIndexByName.find(className); it != classIndexByName.end()) {
				formClassMatches.insert_or_assign(formName, it->second);
			}
		}
	}

	IdAllocator allocator;
	TypeResolver resolver(document.sourcePath, model.dependencies);

	std::vector<const BundleNativeSourceFileSnapshot*> nativeSourceSnapshotsByIndex(parsedClasses.size(), nullptr);
	if (bundle != nullptr) {
		for (const auto& snapshot : bundle->nativeSourceSnapshots) {
			allocator.Observe(snapshot.classId);
			for (const auto id : snapshot.classVarIds) {
				allocator.Observe(id);
			}
			for (const auto& method : snapshot.methods) {
				allocator.Observe(method.id);
				for (const auto id : method.paramIds) {
					allocator.Observe(id);
				}
				for (const auto id : method.localIds) {
					allocator.Observe(id);
				}
			}
		}
		for (const auto& snapshot : bundle->nativeGlobalSnapshots) {
			allocator.Observe(snapshot.id);
		}
		for (const auto& snapshot : bundle->nativeStructSnapshots) {
			allocator.Observe(snapshot.id);
			for (const auto id : snapshot.memberIds) {
				allocator.Observe(id);
			}
		}
		for (const auto& snapshot : bundle->nativeDllSnapshots) {
			allocator.Observe(snapshot.id);
			for (const auto id : snapshot.paramIds) {
				allocator.Observe(id);
			}
		}
		for (const auto& snapshot : bundle->nativeConstantSnapshots) {
			allocator.Observe(snapshot.id);
		}

		const size_t limit = (std::min)(parsedClasses.size(), (std::min)(bundle->sourceFiles.size(), bundle->nativeSourceSnapshots.size()));
		for (size_t index = 0; index < limit; ++index) {
			nativeSourceSnapshotsByIndex[index] = &bundle->nativeSourceSnapshots[index];
		}
	}
	for (const auto& dependency : model.dependencies) {
		for (const auto& range : dependency.definedIds) {
			if (range.count <= 0) {
				continue;
			}
			const std::int32_t endNum = (range.start & epl_system_id::kMaskNum) + range.count - 1;
			allocator.Observe(epl_system_id::GetType(range.start) | endNum);
		}
	}

	auto ensureTypeId = [&](const std::string& rawTypeName) -> std::int32_t {
		const std::string typeName = TypeResolver::NormalizeTypeName(rawTypeName);
		if (typeName.empty()) {
			return 0;
		}
		if (const std::int32_t typeId = resolver.ResolveTypeId(typeName); typeId != 0) {
			return typeId;
		}

		RestoreStruct placeholder;
		placeholder.id = allocator.Alloc(epl_system_id::kTypeStruct);
		placeholder.name = typeName;
		placeholder.comment = "txt2e placeholder";
		placeholder.attr = 0;
		placeholder.isPlaceholder = true;
		model.structs.push_back(std::move(placeholder));
		resolver.RegisterPlaceholderType(typeName, model.structs.back().id);
		return model.structs.back().id;
	};

	auto convertVariableWithId = [&](
		const ParsedVariableDef& definition,
		const std::int32_t idType,
		const bool allowStatic,
		const bool allowPublic,
		const std::optional<std::int32_t> explicitId) {
		RestoreVariable variable;
		variable.id = explicitId.value_or(allocator.Alloc(idType));
		variable.name = definition.name;
		variable.comment = definition.comment;
		variable.dataType = ensureTypeId(definition.typeName);
		variable.attr = BuildVariableAttr(definition, allowStatic, allowPublic);
		variable.arrayBounds = ParseArrayBounds(definition.arrayText);
		return variable;
	};
	auto convertVariable = [&](const ParsedVariableDef& definition, const std::int32_t idType, const bool allowStatic, const bool allowPublic) {
		return convertVariableWithId(definition, idType, allowStatic, allowPublic, std::nullopt);
	};

	const auto peekReusableGlobalSnapshot = [&](const ParsedVariableDef& definition) -> const BundleNativeGlobalSnapshot* {
		if (bundle == nullptr) {
			return nullptr;
		}
		const std::string normalizedName = TypeResolver::NormalizeTypeName(definition.name);
		const std::string digest = ComputeParsedVariableDigest(definition);
		for (const auto& candidate : bundle->nativeGlobalSnapshots) {
			if (!candidate.name.empty() &&
				TypeResolver::NormalizeTypeName(candidate.name) != normalizedName) {
				continue;
			}
			if (candidate.textDigest != digest) {
				continue;
			}
			return &candidate;
		}
		return nullptr;
	};

	std::unordered_set<const BundleNativeGlobalSnapshot*> reusedGlobalSnapshots;
	const auto findReusableGlobalSnapshot = [&](const ParsedVariableDef& definition) -> const BundleNativeGlobalSnapshot* {
		if (bundle == nullptr) {
			return nullptr;
		}
		const std::string normalizedName = TypeResolver::NormalizeTypeName(definition.name);
		const std::string digest = ComputeParsedVariableDigest(definition);
		for (const auto& candidate : bundle->nativeGlobalSnapshots) {
			if (reusedGlobalSnapshots.contains(&candidate)) {
				continue;
			}
			if (!candidate.name.empty() &&
				TypeResolver::NormalizeTypeName(candidate.name) != normalizedName) {
				continue;
			}
			if (candidate.textDigest != digest) {
				continue;
			}
			reusedGlobalSnapshots.insert(&candidate);
			return &candidate;
		}
		return nullptr;
	};

	std::unordered_set<const BundleNativeStructSnapshot*> reusedStructSnapshots;
	const auto findReusableStructSnapshot = [&](const ParsedStructDef& definition) -> const BundleNativeStructSnapshot* {
		if (bundle == nullptr) {
			return nullptr;
		}
		const std::string normalizedName = TypeResolver::NormalizeTypeName(definition.name);
		const std::string digest = ComputeParsedStructDigest(definition);
		for (const auto& candidate : bundle->nativeStructSnapshots) {
			if (reusedStructSnapshots.contains(&candidate)) {
				continue;
			}
			if (!candidate.name.empty() &&
				TypeResolver::NormalizeTypeName(candidate.name) != normalizedName) {
				continue;
			}
			if (candidate.textDigest != digest) {
				continue;
			}
			reusedStructSnapshots.insert(&candidate);
			return &candidate;
		}
		return nullptr;
	};

	std::unordered_set<const BundleNativeDllSnapshot*> reusedDllSnapshots;
	const auto findReusableDllSnapshot = [&](const ParsedDllDef& definition) -> const BundleNativeDllSnapshot* {
		if (bundle == nullptr) {
			return nullptr;
		}
		const std::string normalizedName = TypeResolver::NormalizeTypeName(definition.name);
		const std::string digest = ComputeParsedDllDigest(definition);
		for (const auto& candidate : bundle->nativeDllSnapshots) {
			if (reusedDllSnapshots.contains(&candidate)) {
				continue;
			}
			if (!candidate.name.empty() &&
				TypeResolver::NormalizeTypeName(candidate.name) != normalizedName) {
				continue;
			}
			if (candidate.textDigest != digest) {
				continue;
			}
			reusedDllSnapshots.insert(&candidate);
			return &candidate;
		}
		return nullptr;
	};

	std::unordered_set<const BundleNativeConstantSnapshot*> reusedValueConstantSnapshots;
	const auto findReusableValueConstantSnapshot = [&](const ParsedConstantDef& definition) -> const BundleNativeConstantSnapshot* {
		if (bundle == nullptr) {
			return nullptr;
		}
		const std::string normalizedName = TypeResolver::NormalizeTypeName(definition.name);
		const std::string digest = ComputeParsedConstantDigest(definition);
		for (const auto& candidate : bundle->nativeConstantSnapshots) {
			if (reusedValueConstantSnapshots.contains(&candidate)) {
				continue;
			}
			if (candidate.pageType != kConstPageValue) {
				continue;
			}
			if (!candidate.name.empty() &&
				TypeResolver::NormalizeTypeName(candidate.name) != normalizedName) {
				continue;
			}
			if (candidate.textDigest != digest) {
				continue;
			}
			reusedValueConstantSnapshots.insert(&candidate);
			return &candidate;
		}
		return nullptr;
	};

	std::unordered_set<const BundleNativeConstantSnapshot*> reusedResourceSnapshots;
	const auto findReusableResourceSnapshot = [&](const BundleBinaryResource& resource) -> const BundleNativeConstantSnapshot* {
		if (bundle == nullptr) {
			return nullptr;
		}
		const std::int32_t pageType =
			resource.kind == BundleResourceKind::Image ? kConstPageImage : kConstPageSound;
		const std::string digest = ComputeBundleResourceDigest(resource);
		for (const auto& candidate : bundle->nativeConstantSnapshots) {
			if (reusedResourceSnapshots.contains(&candidate)) {
				continue;
			}
			if (candidate.pageType != pageType) {
				continue;
			}
			if (!candidate.key.empty() && candidate.key != resource.key) {
				continue;
			}
			if (candidate.textDigest != digest) {
				continue;
			}
			reusedResourceSnapshots.insert(&candidate);
			return &candidate;
		}
		return nullptr;
	};

	std::vector<size_t> localClassModelIndices;
	localClassModelIndices.reserve(parsedClasses.size());
	std::vector<size_t> localStructModelIndices;
	localStructModelIndices.reserve(parsedStructs.size());
	std::vector<const BundleNativeStructSnapshot*> nativeStructSnapshotsByIndex(parsedStructs.size(), nullptr);
	std::vector<size_t> localGlobalModelIndices;
	localGlobalModelIndices.reserve(parsedGlobals.size());
	std::vector<size_t> localDllModelIndices;
	localDllModelIndices.reserve(parsedDlls.size());
	std::vector<size_t> localConstantModelIndices;
	localConstantModelIndices.reserve(parsedConstants.size());
	std::vector<std::string> localConstantKeys;
	localConstantKeys.reserve(parsedConstants.size());
	std::unordered_map<std::string, int> localConstantKeyCounters;
	const auto appendDefinedIdRange = [](RestoreDependencyInfo& dependency, const std::int32_t start, const std::int32_t count) {
		if (count <= 0) {
			return;
		}
		dependency.definedIds.push_back(RestoreDependencyInfo::DefinedIdRange { start, count });
	};

	auto importDependencyBundle = [&](RestoreDependencyInfo& dependency) -> bool {
		if (dependency.isSupportLibrary) {
			return true;
		}
		ProjectBundle dependencyBundle;
		std::string dependencyError;
		bool dependencyLoaded = false;
		if (!dependency.localWorkspace.empty()) {
			std::error_code ec;
			if (std::filesystem::exists(Utf8PathToPath(dependency.localWorkspace), ec)) {
				BundleDirectoryCodec dependencyCodec;
				dependencyLoaded = dependencyCodec.ReadBundle(dependency.localWorkspace, dependencyBundle, &dependencyError);
			}
		}
		if (!dependencyLoaded && !dependency.resolvedPath.empty()) {
			std::error_code ec;
			if (std::filesystem::exists(Utf8PathToPath(dependency.resolvedPath), ec)) {
				Generator dependencyGenerator;
				dependencyLoaded = dependencyGenerator.GenerateBundle(dependency.resolvedPath, dependencyBundle, &dependencyError);
			}
		}
		if (!dependencyLoaded) {
			if (outError != nullptr) {
				*outError = "dependency_module_not_found: " + dependency.path;
				if (!dependency.resolvedPath.empty()) {
					*outError += " resolvedPath=" + dependency.resolvedPath;
				}
				if (!dependency.localWorkspace.empty()) {
					*outError += " localWorkspace=" + dependency.localWorkspace;
				}
				if (!dependencyError.empty()) {
					*outError += " => " + dependencyError;
				}
			}
			return false;
		}

		DependencyImportIdCursor dependencyIds(dependency);
		const bool preserveDefinedIds = dependencyIds.HasOriginalRanges();
		if (preserveDefinedIds) {
			dependencyIds.ObserveAll(allocator);
		}
		const auto convertDependencyVariable = [&](const ParsedVariableDef& definition, const std::int32_t idType, const bool allowStatic, const bool allowPublic) {
			const std::int32_t variableId = dependencyIds.AllocChild(allocator, idType);
			return convertVariableWithId(definition, idType, allowStatic, allowPublic, variableId);
		};

		Document dependencyDocument = BuildDocumentFromBundle(dependencyBundle);
		std::vector<ParsedClassDef> dependencyClasses;
		std::vector<ParsedVariableDef> dependencyGlobals;
		std::vector<ParsedStructDef> dependencyStructs;
		std::vector<ParsedDllDef> dependencyDlls;
		std::vector<ParsedConstantDef> dependencyConstants;
		const std::unordered_set<std::string> emptyFormNames;
		for (const auto& page : dependencyDocument.pages) {
			if (page.typeName == "程序集") {
				ParsedClassDef parsedClass;
				if (!ParseProgramPage(page, emptyFormNames, parsedClass, outError)) {
					return false;
				}
				dependencyClasses.push_back(std::move(parsedClass));
			}
			else if (page.typeName == "全局变量") {
				ParseGlobalPage(page, dependencyGlobals);
			}
			else if (page.typeName == "自定义数据类型") {
				ParseStructPage(page, dependencyStructs);
			}
			else if (page.typeName == "DLL命令") {
				ParseDllPage(page, dependencyDlls);
			}
			else if (page.typeName == "常量资源") {
				ParseConstantPage(page, dependencyConstants);
			}
		}

		std::unordered_map<std::string, const ParsedStructDef*> dependencyStructByName;
		for (const auto& parsedStruct : dependencyStructs) {
			const std::string normalizedName = TypeResolver::NormalizeTypeName(parsedStruct.name);
			if (normalizedName.empty()) {
				continue;
			}
			dependencyStructByName.insert_or_assign(normalizedName, &parsedStruct);
		}

		std::unordered_map<std::string, const ParsedClassDef*> dependencyClassByName;
		for (const auto& parsedClass : dependencyClasses) {
			dependencyClassByName.insert_or_assign(TypeResolver::NormalizeTypeName(parsedClass.name), &parsedClass);
		}

		struct DependencyNativeClassBinding {
			std::int32_t classId = 0;
			std::int32_t memoryAddress = 0;
			std::int32_t baseClass = 0;
			std::unordered_map<std::string, std::vector<NativeDependencyMethodSymbol>> methodsByName;
			std::unordered_map<std::string, size_t> methodOffsetsByName;

			const NativeDependencyMethodSymbol* TakeMethod(const std::string& name)
			{
				const std::string key = TypeResolver::NormalizeTypeName(name);
				const auto it = methodsByName.find(key);
				if (it == methodsByName.end()) {
					return nullptr;
				}
				size_t& offset = methodOffsetsByName[key];
				if (offset >= it->second.size()) {
					return nullptr;
				}
				return &it->second[offset++];
			}
		};

		std::unordered_map<std::string, DependencyNativeClassBinding> nativeClassBindings;
		for (const auto& classSymbol : dependency.nativeClasses) {
			const std::string className = TypeResolver::NormalizeTypeName(classSymbol.name);
			if (className.empty()) {
				continue;
			}
			nativeClassBindings[className].classId = classSymbol.id;
			nativeClassBindings[className].memoryAddress = classSymbol.memoryAddress;
			nativeClassBindings[className].baseClass = classSymbol.baseClass;
		}
		for (const auto& methodSymbol : dependency.nativeMethods) {
			const std::string methodName = TypeResolver::NormalizeTypeName(methodSymbol.name);
			if (methodName.empty()) {
				continue;
			}
			const std::string ownerName = TypeResolver::NormalizeTypeName(methodSymbol.ownerClassName);
			if (!ownerName.empty()) {
				nativeClassBindings[ownerName].methodsByName[methodName].push_back(methodSymbol);
			}
			nativeClassBindings[std::string()].methodsByName[methodName].push_back(methodSymbol);
		}

		const size_t nativeClassCount = (std::min)(
			dependencyBundle.sourceFiles.size(),
			dependencyBundle.nativeSourceSnapshots.size());
		for (size_t classIndex = 0; classIndex < nativeClassCount; ++classIndex) {
			const std::string className = TypeResolver::NormalizeTypeName(dependencyBundle.sourceFiles[classIndex].logicalName);
			if (className.empty()) {
				continue;
			}

			auto& binding = nativeClassBindings[className];
			const auto* snapshot = &dependencyBundle.nativeSourceSnapshots[classIndex];
			if (binding.classId == 0) {
				binding.classId = snapshot->classId;
			}
			if (binding.memoryAddress == 0) {
				binding.memoryAddress = snapshot->classMemoryAddress;
			}
			if (binding.baseClass == 0) {
				binding.baseClass = snapshot->baseClass;
			}
			const auto parsedClassIt = dependencyClassByName.find(className);
			const ParsedClassDef* parsedClass = parsedClassIt == dependencyClassByName.end() ? nullptr : parsedClassIt->second;
			for (size_t methodIndex = 0; methodIndex < snapshot->methods.size(); ++methodIndex) {
				const auto& methodSnapshot = snapshot->methods[methodIndex];
				std::string methodName = methodSnapshot.name;
				if (methodName.empty() && parsedClass != nullptr && methodIndex < parsedClass->methods.size()) {
					methodName = parsedClass->methods[methodIndex].name;
				}
				methodName = TypeResolver::NormalizeTypeName(methodName);
				const auto globalNativeIt = nativeClassBindings.find(std::string());
				const bool hasOriginalGlobalMethod =
					globalNativeIt != nativeClassBindings.end() &&
					globalNativeIt->second.methodsByName.contains(methodName);
				if (!methodName.empty() && !binding.methodsByName.contains(methodName) && !hasOriginalGlobalMethod) {
					NativeDependencyMethodSymbol methodSymbol;
					methodSymbol.id = methodSnapshot.id;
					methodSymbol.ownerClassId = snapshot->classId;
					methodSymbol.memoryAddress = methodSnapshot.memoryAddress;
					methodSymbol.ownerClassName = dependencyBundle.sourceFiles[classIndex].logicalName;
					methodSymbol.name = methodName;
					methodSymbol.paramIds = methodSnapshot.paramIds;
					binding.methodsByName[methodName].push_back(std::move(methodSymbol));
				}
			}
		}

		const auto findNativeClassBinding = [&](const ParsedClassDef& parsedClass) -> DependencyNativeClassBinding* {
			const auto it = nativeClassBindings.find(TypeResolver::NormalizeTypeName(parsedClass.name));
			return it == nativeClassBindings.end() ? nullptr : &it->second;
		};

		std::unordered_set<std::string> requiredDependencyStructNames;
		const auto markRequiredDependencyStructType = [&](auto&& self, const std::string& rawTypeName) -> void {
			const std::string normalizedName = TypeResolver::NormalizeTypeName(rawTypeName);
			if (normalizedName.empty()) {
				return;
			}
			const auto structIt = dependencyStructByName.find(normalizedName);
			if (structIt == dependencyStructByName.end() || structIt->second == nullptr) {
				return;
			}
			if (!requiredDependencyStructNames.insert(normalizedName).second) {
				return;
			}
			for (const auto& member : structIt->second->members) {
				self(self, member.typeName);
			}
		};

		for (const auto& parsedStruct : dependencyStructs) {
			if (parsedStruct.isPublic) {
				markRequiredDependencyStructType(markRequiredDependencyStructType, parsedStruct.name);
			}
		}
		for (const auto& variable : dependencyGlobals) {
			if (HasWordFlag(variable.flagsText, "公开")) {
				markRequiredDependencyStructType(markRequiredDependencyStructType, variable.typeName);
			}
		}
		for (const auto& parsedDll : dependencyDlls) {
			if (!parsedDll.isPublic) {
				continue;
			}
			markRequiredDependencyStructType(markRequiredDependencyStructType, parsedDll.returnTypeName);
			for (const auto& param : parsedDll.params) {
				markRequiredDependencyStructType(markRequiredDependencyStructType, param.typeName);
			}
		}
		for (const auto& parsedClass : dependencyClasses) {
			if (parsedClass.isFormClass || parsedClass.isUserClass) {
				continue;
			}
			for (const auto& parsedMethod : parsedClass.methods) {
				if (!parsedMethod.isPublic) {
					continue;
				}
				markRequiredDependencyStructType(markRequiredDependencyStructType, parsedMethod.returnTypeName);
				for (const auto& param : parsedMethod.params) {
					markRequiredDependencyStructType(markRequiredDependencyStructType, param.typeName);
				}
			}
		}
		for (const auto& parsedClass : dependencyClasses) {
			if (!parsedClass.isPublic || !parsedClass.isUserClass || parsedClass.isFormClass) {
				continue;
			}
			const ParsedClassDef* currentClass = &parsedClass;
			std::unordered_set<std::string> walkedClasses;
			while (currentClass != nullptr &&
				walkedClasses.insert(TypeResolver::NormalizeTypeName(currentClass->name)).second) {
				for (const auto& parsedMethod : currentClass->methods) {
					markRequiredDependencyStructType(markRequiredDependencyStructType, parsedMethod.returnTypeName);
					for (const auto& param : parsedMethod.params) {
						markRequiredDependencyStructType(markRequiredDependencyStructType, param.typeName);
					}
				}

				const std::string baseClassName = TypeResolver::NormalizeTypeName(currentClass->baseClassName);
				if (baseClassName.empty() || baseClassName == "对象") {
					break;
				}
				const auto baseIt = dependencyClassByName.find(baseClassName);
				currentClass = baseIt == dependencyClassByName.end() ? nullptr : baseIt->second;
			}
		}

		std::vector<size_t> importedStructModelIndices;
		importedStructModelIndices.reserve(requiredDependencyStructNames.size());
		std::int32_t rangeStart = 0;
		std::int32_t rangeCount = 0;
		for (const auto& parsedStruct : dependencyStructs) {
			const std::string normalizedName = TypeResolver::NormalizeTypeName(parsedStruct.name);
			if (normalizedName.empty() || !requiredDependencyStructNames.contains(normalizedName)) {
				continue;
			}
			RestoreStruct item;
			item.id = dependencyIds.AllocTopLevel(allocator, epl_system_id::kTypeStruct);
			item.name = parsedStruct.name;
			item.comment = parsedStruct.comment;
			item.attr = 0x2;
			importedStructModelIndices.push_back(model.structs.size());
			model.structs.push_back(std::move(item));
			resolver.RegisterUserType(parsedStruct.name, model.structs.back().id);
			if (rangeStart == 0) {
				rangeStart = model.structs.back().id;
			}
			++rangeCount;
		}
		if (!preserveDefinedIds) {
			appendDefinedIdRange(dependency, rangeStart, rangeCount);
		}

		std::unordered_map<std::string, size_t> importedClassModelIndices;
		std::vector<std::pair<std::string, size_t>> importedClassModelOrder;
		size_t hiddenTempClassIndex = (std::numeric_limits<size_t>::max)();
		rangeStart = 0;
		rangeCount = 0;
		{
			RestoreClass hiddenTemp;
			const auto hiddenNativeIt = nativeClassBindings.find(TypeResolver::NormalizeTypeName("__HIDDEN_TEMP_MOD__"));
			const DependencyNativeClassBinding* hiddenNative =
				hiddenNativeIt == nativeClassBindings.end() ? nullptr : &hiddenNativeIt->second;
			hiddenTemp.id = dependencyIds.AllocTopLevel(
				allocator,
				epl_system_id::kTypeStaticClass,
				hiddenNative != nullptr ? hiddenNative->classId : 0);
			hiddenTemp.memoryAddress = hiddenNative != nullptr ? hiddenNative->memoryAddress : 0;
			hiddenTemp.name = "__HIDDEN_TEMP_MOD__";
			hiddenTemp.comment = "dependency hidden module";
			if (hiddenNative != nullptr) {
				hiddenTemp.baseClass = hiddenNative->baseClass;
			}
			hiddenTemp.isHidden = true;
			hiddenTempClassIndex = model.classes.size();
			model.classes.push_back(std::move(hiddenTemp));
			rangeStart = model.classes.back().id;
			rangeCount = 1;
		}
		for (const auto& parsedClass : dependencyClasses) {
			if (!parsedClass.isPublic || !parsedClass.isUserClass || parsedClass.isFormClass) {
				continue;
			}
			DependencyNativeClassBinding* nativeClass = findNativeClassBinding(parsedClass);
			RestoreClass item;
			item.id = dependencyIds.AllocTopLevel(
				allocator,
				epl_system_id::kTypeClass,
				nativeClass != nullptr ? nativeClass->classId : 0);
			item.memoryAddress = nativeClass != nullptr ? nativeClass->memoryAddress : 0;
			item.name = parsedClass.name;
			item.comment = parsedClass.comment;
			item.baseClass = nativeClass != nullptr ? nativeClass->baseClass : -1;
			item.isHidden = true;
			const std::string normalizedClassName = TypeResolver::NormalizeTypeName(parsedClass.name);
			importedClassModelIndices.insert_or_assign(normalizedClassName, model.classes.size());
			importedClassModelOrder.emplace_back(normalizedClassName, model.classes.size());
			model.classes.push_back(std::move(item));
			resolver.RegisterUserType(parsedClass.name, model.classes.back().id);
			++rangeCount;
		}
		if (!preserveDefinedIds) {
			appendDefinedIdRange(dependency, rangeStart, rangeCount);
		}

		size_t importedStructIndex = 0;
		for (const auto& parsedStruct : dependencyStructs) {
			const std::string normalizedName = TypeResolver::NormalizeTypeName(parsedStruct.name);
			if (normalizedName.empty() || !requiredDependencyStructNames.contains(normalizedName)) {
				continue;
			}
			auto& targetStruct = model.structs[importedStructModelIndices[importedStructIndex++]];
			for (const auto& member : parsedStruct.members) {
				targetStruct.members.push_back(convertDependencyVariable(member, epl_system_id::kTypeStructMember, false, false));
			}
		}

		rangeStart = 0;
		rangeCount = 0;
		for (const auto& variable : dependencyGlobals) {
			if (!HasWordFlag(variable.flagsText, "公开")) {
				continue;
			}
			const std::int32_t importedId = dependencyIds.AllocTopLevel(allocator, epl_system_id::kTypeGlobal);
			RestoreVariable imported = convertVariableWithId(variable, epl_system_id::kTypeGlobal, false, false, importedId);
			imported.attr |= kGlobalAttrHidden;
			if (rangeStart == 0) {
				rangeStart = imported.id;
			}
			++rangeCount;
			model.globals.push_back(std::move(imported));
		}
		if (!preserveDefinedIds) {
			appendDefinedIdRange(dependency, rangeStart, rangeCount);
		}

		rangeStart = 0;
		rangeCount = 0;
		for (const auto& parsedConstant : dependencyConstants) {
			if (!parsedConstant.isPublic) {
				continue;
			}
			RestoreConstant constant;
			constant.id = dependencyIds.AllocTopLevel(allocator, epl_system_id::kTypeConstant);
			constant.attr = kConstAttrHidden;
			if (parsedConstant.isLongText) {
				constant.attr |= kConstAttrLongText;
			}
			constant.name = parsedConstant.name;
			constant.comment = parsedConstant.comment;
			constant.valueText = parsedConstant.valueText;
			if (rangeStart == 0) {
				rangeStart = constant.id;
			}
			++rangeCount;
			model.constants.push_back(std::move(constant));
		}
		for (const auto& resource : dependencyBundle.resources) {
			if (!resource.isPublic) {
				continue;
			}
			RestoreConstant constant;
			constant.id = dependencyIds.AllocTopLevel(
				allocator,
				resource.kind == BundleResourceKind::Image
					? epl_system_id::kTypeImageResource
					: epl_system_id::kTypeSoundResource);
			constant.attr = kConstAttrHidden;
			constant.pageType = resource.kind == BundleResourceKind::Image ? kConstPageImage : kConstPageSound;
			constant.name = resource.logicalName;
			constant.comment = resource.comment;
			constant.rawData = resource.data;
			if (rangeStart == 0) {
				rangeStart = constant.id;
			}
			++rangeCount;
			model.constants.push_back(std::move(constant));
		}
		if (!preserveDefinedIds) {
			appendDefinedIdRange(dependency, rangeStart, rangeCount);
		}

		rangeStart = 0;
		rangeCount = 0;
		for (const auto& parsedDll : dependencyDlls) {
			if (!parsedDll.isPublic) {
				continue;
			}
			RestoreDll dll;
			dll.id = dependencyIds.AllocTopLevel(allocator, epl_system_id::kTypeDll);
			dll.attr = 0x4;
			dll.returnType = ensureTypeId(parsedDll.returnTypeName);
			dll.name = parsedDll.name;
			dll.comment = parsedDll.comment;
			dll.fileName = parsedDll.fileName;
			dll.commandName = parsedDll.commandName;
			for (const auto& param : parsedDll.params) {
				dll.params.push_back(convertDependencyVariable(param, epl_system_id::kTypeDllParameter, false, false));
			}
			if (rangeStart == 0) {
				rangeStart = dll.id;
			}
			++rangeCount;
			model.dlls.push_back(std::move(dll));
		}
		if (!preserveDefinedIds) {
			appendDefinedIdRange(dependency, rangeStart, rangeCount);
		}

		rangeStart = 0;
		rangeCount = 0;
		const auto appendImportedMethod = [&](
			const ParsedMethodDef& parsedMethod,
			RestoreClass& ownerClass,
			const bool preservePublic,
			DependencyNativeClassBinding* nativeClass) {
			const NativeDependencyMethodSymbol* nativeMethod =
				nativeClass == nullptr ? nullptr : nativeClass->TakeMethod(parsedMethod.name);
			if (nativeMethod == nullptr) {
				if (auto globalNativeIt = nativeClassBindings.find(std::string()); globalNativeIt != nativeClassBindings.end()) {
					nativeMethod = globalNativeIt->second.TakeMethod(parsedMethod.name);
				}
			}
			RestoreMethod method;
			method.id = dependencyIds.AllocTopLevel(
				allocator,
				epl_system_id::kTypeMethod,
				nativeMethod != nullptr ? nativeMethod->id : 0);
			method.memoryAddress = nativeMethod != nullptr ? nativeMethod->memoryAddress : 0;
			method.ownerClass = ownerClass.id;
			method.attr =
				nativeMethod != nullptr && nativeMethod->attr != 0
					? nativeMethod->attr
					: 0x80;
			method.attr |= 0x80;
			if (preservePublic && parsedMethod.isPublic) {
				method.attr |= 0x8;
			}
			method.returnType =
				nativeMethod != nullptr && nativeMethod->returnType != 0
					? nativeMethod->returnType
					: ensureTypeId(parsedMethod.returnTypeName);
			method.name = parsedMethod.name;
			method.comment = parsedMethod.comment;
			const size_t paramCount =
				nativeMethod != nullptr
					? (std::max)(parsedMethod.params.size(), nativeMethod->params.size())
					: parsedMethod.params.size();
			for (size_t paramIndex = 0; paramIndex < paramCount; ++paramIndex) {
				const ParsedVariableDef* parsedParam =
					paramIndex < parsedMethod.params.size() ? &parsedMethod.params[paramIndex] : nullptr;
				const NativeDependencyMethodParamSymbol* nativeParam =
					nativeMethod != nullptr && paramIndex < nativeMethod->params.size()
						? &nativeMethod->params[paramIndex]
						: nullptr;
				const std::int32_t nativeParamId =
					nativeParam != nullptr && nativeParam->id != 0
						? nativeParam->id
						: (nativeMethod != nullptr && paramIndex < nativeMethod->paramIds.size()
							? nativeMethod->paramIds[paramIndex]
							: 0);
				const std::int32_t paramId = dependencyIds.AllocChild(allocator, epl_system_id::kTypeLocal, nativeParamId);
				if (nativeParam != nullptr) {
					RestoreVariable param;
					param.id = paramId;
					param.dataType =
						nativeParam->dataType != 0
							? nativeParam->dataType
							: (parsedParam != nullptr ? ensureTypeId(parsedParam->typeName) : 0);
					param.attr =
						nativeParam->attr != 0 || parsedParam == nullptr
							? nativeParam->attr
							: BuildVariableAttr(*parsedParam, false, false);
					param.arrayBounds =
						!nativeParam->arrayBounds.empty() || parsedParam == nullptr
							? nativeParam->arrayBounds
							: ParseArrayBounds(parsedParam->arrayText);
					if (parsedParam != nullptr) {
						param.name = parsedParam->name;
						param.comment = parsedParam->comment;
					}
					method.params.push_back(std::move(param));
				}
				else if (parsedParam != nullptr) {
					method.params.push_back(convertVariableWithId(
						*parsedParam,
						epl_system_id::kTypeLocal,
						false,
						false,
						paramId));
				}
			}
			ownerClass.functionIds.push_back(method.id);
			if (rangeStart == 0) {
				rangeStart = method.id;
			}
			++rangeCount;
			model.methods.push_back(std::move(method));
		};

		for (const auto& parsedClass : dependencyClasses) {
			if (parsedClass.isFormClass || parsedClass.isUserClass) {
				continue;
			}
			DependencyNativeClassBinding* nativeClass = findNativeClassBinding(parsedClass);
			for (const auto& parsedMethod : parsedClass.methods) {
				if (!parsedMethod.isPublic) {
					continue;
				}
				appendImportedMethod(parsedMethod, model.classes[hiddenTempClassIndex], true, nativeClass);
			}
		}

		for (const auto& [normalizedName, modelIndex] : importedClassModelOrder) {
			auto depClassIt = dependencyClassByName.find(normalizedName);
			if (depClassIt == dependencyClassByName.end() || depClassIt->second == nullptr) {
				continue;
			}

			std::unordered_set<std::string> addedMethodNames;
			const ParsedClassDef* currentClass = depClassIt->second;
			while (currentClass != nullptr) {
				DependencyNativeClassBinding* nativeClass = findNativeClassBinding(*currentClass);
				for (const auto& parsedMethod : currentClass->methods) {
					const std::string methodName = TypeResolver::NormalizeTypeName(parsedMethod.name);
					if (!parsedMethod.isPublic ||
						methodName == "_初始化" ||
						methodName == "_销毁" ||
						!addedMethodNames.insert(methodName).second) {
						continue;
					}
					appendImportedMethod(parsedMethod, model.classes[modelIndex], true, nativeClass);
				}

				const std::string baseClassName = TypeResolver::NormalizeTypeName(currentClass->baseClassName);
				if (baseClassName.empty() || baseClassName == "对象") {
					break;
				}
				const auto baseIt = dependencyClassByName.find(baseClassName);
				currentClass = baseIt == dependencyClassByName.end() ? nullptr : baseIt->second;
			}
		}
		if (!preserveDefinedIds) {
			appendDefinedIdRange(dependency, rangeStart, rangeCount);
		}
		return true;
	};

	for (auto& dependency : model.dependencies) {
		if (!dependency.isSupportLibrary && !importDependencyBundle(dependency)) {
			return false;
		}
	}

	for (size_t classIndex = 0; classIndex < parsedClasses.size(); ++classIndex) {
		const auto& parsedClass = parsedClasses[classIndex];
		const BundleNativeSourceFileSnapshot* nativeSourceSnapshot =
			classIndex < nativeSourceSnapshotsByIndex.size() ? nativeSourceSnapshotsByIndex[classIndex] : nullptr;
		RestoreClass item;
		if (nativeSourceSnapshot != nullptr && nativeSourceSnapshot->classId != 0) {
			item.id = nativeSourceSnapshot->classId;
			item.memoryAddress = nativeSourceSnapshot->classMemoryAddress;
			item.formId = nativeSourceSnapshot->formId;
		}
		else {
			item.id = allocator.Alloc(
				parsedClass.isFormClass
					? epl_system_id::kTypeFormClass
					: (parsedClass.isUserClass ? epl_system_id::kTypeClass : epl_system_id::kTypeStaticClass));
		}
		item.name = parsedClass.name;
		item.comment = parsedClass.comment;
		item.isPublic = parsedClass.isPublic;
		item.isFormClass = parsedClass.isFormClass;
		localClassModelIndices.push_back(model.classes.size());
		model.classes.push_back(std::move(item));
		resolver.RegisterUserType(parsedClass.name, model.classes.back().id);
	}

	for (size_t structIndex = 0; structIndex < parsedStructs.size(); ++structIndex) {
		const auto& parsedStruct = parsedStructs[structIndex];
		const BundleNativeStructSnapshot* reusableStructSnapshot = findReusableStructSnapshot(parsedStruct);
		nativeStructSnapshotsByIndex[structIndex] = reusableStructSnapshot;
		RestoreStruct item;
		item.id =
			reusableStructSnapshot != nullptr && reusableStructSnapshot->id != 0
			? reusableStructSnapshot->id
			: allocator.Alloc(epl_system_id::kTypeStruct);
		item.memoryAddress = reusableStructSnapshot != nullptr ? reusableStructSnapshot->memoryAddress : 0;
		item.name = parsedStruct.name;
		item.comment = parsedStruct.comment;
		item.attr = parsedStruct.isPublic ? 0x1 : 0;
		localStructModelIndices.push_back(model.structs.size());
		model.structs.push_back(std::move(item));
		resolver.RegisterUserType(parsedStruct.name, model.structs.back().id);
	}

	for (size_t classIndex = 0; classIndex < parsedClasses.size(); ++classIndex) {
		const auto& parsedClass = parsedClasses[classIndex];
		const BundleNativeSourceFileSnapshot* nativeSourceSnapshot =
			classIndex < nativeSourceSnapshotsByIndex.size() ? nativeSourceSnapshotsByIndex[classIndex] : nullptr;
		auto& targetClass = model.classes[localClassModelIndices[classIndex]];
		if (nativeSourceSnapshot != nullptr) {
			targetClass.baseClass = nativeSourceSnapshot->baseClass;
		}
		else {
			targetClass.baseClass = parsedClass.isFormClass && TypeResolver::NormalizeTypeName(parsedClass.baseClassName).empty()
				? 65537
				: (TypeResolver::NormalizeTypeName(parsedClass.baseClassName) == "对象" ? -1 : ensureTypeId(parsedClass.baseClassName));
		}
		for (size_t variableIndex = 0; variableIndex < parsedClass.vars.size(); ++variableIndex) {
			RestoreVariable variable =
				convertVariable(parsedClass.vars[variableIndex], epl_system_id::kTypeClassMember, false, false);
			if (nativeSourceSnapshot != nullptr && variableIndex < nativeSourceSnapshot->classVarIds.size() &&
				nativeSourceSnapshot->classVarIds[variableIndex] != 0) {
				variable.id = nativeSourceSnapshot->classVarIds[variableIndex];
			}
			targetClass.vars.push_back(std::move(variable));
		}

		struct NativeMethodSnapshotMatch {
			const BundleNativeMethodSnapshot* snapshot = nullptr;
			const ParsedMethodDef* originalParsedMethod = nullptr;
		};
		std::unordered_set<const BundleNativeMethodSnapshot*> usedNativeMethodSnapshots;
		const auto originalMethodForSnapshot = [&](const size_t snapshotIndex) -> const ParsedMethodDef* {
			if (classIndex >= originalParsedClasses.size() ||
				snapshotIndex >= originalParsedClasses[classIndex].methods.size()) {
				return nullptr;
			}
			return &originalParsedClasses[classIndex].methods[snapshotIndex];
		};
		const auto snapshotMethodName = [&](const size_t snapshotIndex, const BundleNativeMethodSnapshot& snapshot) {
			std::string methodName = snapshot.name;
			if (methodName.empty()) {
				if (const ParsedMethodDef* originalMethod = originalMethodForSnapshot(snapshotIndex);
					originalMethod != nullptr) {
					methodName = originalMethod->name;
				}
			}
			return TypeResolver::NormalizeTypeName(methodName);
		};
		const auto findNativeMethodSnapshot = [&](const ParsedMethodDef& parsedMethod, const bool requireDigest) -> NativeMethodSnapshotMatch {
			if (nativeSourceSnapshot == nullptr) {
				return {};
			}
			const std::string normalizedName = TypeResolver::NormalizeTypeName(parsedMethod.name);
			const std::string methodDigest = ComputeParsedMethodDigest(parsedMethod);
			for (size_t snapshotIndex = 0; snapshotIndex < nativeSourceSnapshot->methods.size(); ++snapshotIndex) {
				const auto& candidate = nativeSourceSnapshot->methods[snapshotIndex];
				if (usedNativeMethodSnapshots.contains(&candidate)) {
					continue;
				}
				if (snapshotMethodName(snapshotIndex, candidate) != normalizedName) {
					continue;
				}
				if (requireDigest &&
					(candidate.textDigest.empty() || candidate.textDigest != methodDigest)) {
					continue;
				}
				usedNativeMethodSnapshots.insert(&candidate);
				return NativeMethodSnapshotMatch{ &candidate, originalMethodForSnapshot(snapshotIndex) };
			}
			return {};
		};

		for (size_t methodIndex = 0; methodIndex < parsedClass.methods.size(); ++methodIndex) {
			const auto& parsedMethod = parsedClass.methods[methodIndex];
			const NativeMethodSnapshotMatch reusableNativeMethodMatch =
				findNativeMethodSnapshot(parsedMethod, true);
			const NativeMethodSnapshotMatch identityNativeMethodMatch =
				reusableNativeMethodMatch.snapshot != nullptr
					? reusableNativeMethodMatch
					: findNativeMethodSnapshot(parsedMethod, false);
			const BundleNativeMethodSnapshot* reusableNativeMethodSnapshot =
				reusableNativeMethodMatch.snapshot;
			const BundleNativeMethodSnapshot* identityNativeMethodSnapshot =
				identityNativeMethodMatch.snapshot;
			RestoreMethod method;
			if (identityNativeMethodSnapshot != nullptr && identityNativeMethodSnapshot->id != 0) {
				method.id = identityNativeMethodSnapshot->id;
				method.memoryAddress = identityNativeMethodSnapshot->memoryAddress;
			}
			else {
				method.id = allocator.Alloc(epl_system_id::kTypeMethod);
			}
			method.ownerClass = targetClass.id;
			const std::int32_t defaultMethodAttr = ComputeDefaultMethodAttr(parsedMethod);
			method.attr = defaultMethodAttr;
			if (identityNativeMethodSnapshot != nullptr &&
				(identityNativeMethodSnapshot->attr != 0 || defaultMethodAttr == 0)) {
				method.attr = identityNativeMethodSnapshot->attr;
			}
			method.returnType = ensureTypeId(parsedMethod.returnTypeName);
			method.name = parsedMethod.name;
			method.comment = parsedMethod.comment;
			for (size_t paramIndex = 0; paramIndex < parsedMethod.params.size(); ++paramIndex) {
				RestoreVariable param =
					convertVariable(parsedMethod.params[paramIndex], epl_system_id::kTypeLocal, false, false);
				if (identityNativeMethodSnapshot != nullptr &&
					paramIndex < identityNativeMethodSnapshot->paramIds.size() &&
					identityNativeMethodSnapshot->paramIds[paramIndex] != 0) {
					param.id = identityNativeMethodSnapshot->paramIds[paramIndex];
				}
				method.params.push_back(std::move(param));
			}
			for (size_t localIndex = 0; localIndex < parsedMethod.locals.size(); ++localIndex) {
				RestoreVariable local =
					convertVariable(parsedMethod.locals[localIndex], epl_system_id::kTypeLocal, true, false);
				if (identityNativeMethodSnapshot != nullptr &&
					localIndex < identityNativeMethodSnapshot->localIds.size() &&
					identityNativeMethodSnapshot->localIds[localIndex] != 0) {
					local.id = identityNativeMethodSnapshot->localIds[localIndex];
				}
				method.locals.push_back(std::move(local));
			}
			NativeObjectMethodEncodeContext nativeObjectEncodeContext;
			nativeObjectEncodeContext.typeResolver = &resolver;
			const auto addNativeObjectVariable = [&nativeObjectEncodeContext](const std::string& name, const std::int32_t id, const std::int32_t typeId) {
				const std::string key = TypeResolver::NormalizeTypeName(name);
				if (key.empty() || id == 0) {
					return;
				}
				nativeObjectEncodeContext.variablesByName.insert_or_assign(
					key,
					NativeObjectVariableSymbol{ id, typeId });
			};
			for (size_t paramIndex = 0; paramIndex < parsedMethod.params.size() && paramIndex < method.params.size(); ++paramIndex) {
				addNativeObjectVariable(parsedMethod.params[paramIndex].name, method.params[paramIndex].id, method.params[paramIndex].dataType);
			}
			for (size_t localIndex = 0; localIndex < parsedMethod.locals.size() && localIndex < method.locals.size(); ++localIndex) {
				addNativeObjectVariable(parsedMethod.locals[localIndex].name, method.locals[localIndex].id, method.locals[localIndex].dataType);
			}
			for (const auto& classVariable : targetClass.vars) {
				addNativeObjectVariable(classVariable.name, classVariable.id, classVariable.dataType);
			}
			for (const auto& globalDefinition : parsedGlobals) {
				const BundleNativeGlobalSnapshot* snapshot = peekReusableGlobalSnapshot(globalDefinition);
				if (snapshot == nullptr || snapshot->id == 0) {
					continue;
				}
				addNativeObjectVariable(globalDefinition.name, snapshot->id, ensureTypeId(globalDefinition.typeName));
			}
			for (const auto& [formName, matchedClassIndex] : formClassMatches) {
				if (matchedClassIndex != classIndex ||
					nativeSourceSnapshot == nullptr ||
					nativeSourceSnapshot->formId == 0) {
					continue;
				}
				addNativeObjectVariable(formName, nativeSourceSnapshot->formId, 65537);
			}
			for (size_t sourceClassIndex = 0; sourceClassIndex < parsedClasses.size(); ++sourceClassIndex) {
				const BundleNativeSourceFileSnapshot* sourceSnapshot =
					sourceClassIndex < nativeSourceSnapshotsByIndex.size()
						? nativeSourceSnapshotsByIndex[sourceClassIndex]
						: nullptr;
				if (sourceSnapshot == nullptr || sourceSnapshot->classId == 0) {
					continue;
				}
				for (size_t sourceMethodIndex = 0; sourceMethodIndex < sourceSnapshot->methods.size(); ++sourceMethodIndex) {
					const auto& sourceMethodSnapshot = sourceSnapshot->methods[sourceMethodIndex];
					if (sourceMethodSnapshot.id == 0) {
						continue;
					}
					std::string sourceMethodName = sourceMethodSnapshot.name;
					if (sourceMethodName.empty() &&
						sourceClassIndex < parsedClasses.size() &&
						sourceMethodIndex < parsedClasses[sourceClassIndex].methods.size()) {
						sourceMethodName = parsedClasses[sourceClassIndex].methods[sourceMethodIndex].name;
					}
					const std::string sourceMethodKey = TypeResolver::NormalizeTypeName(sourceMethodName);
					if (sourceMethodKey.empty()) {
						continue;
					}
					const NativeFunctionSymbol sourceMethodSymbol{ -2, sourceMethodSnapshot.id };
					nativeObjectEncodeContext.functionsByName.insert_or_assign(sourceMethodKey, sourceMethodSymbol);
					nativeObjectEncodeContext
						.methodsByOwnerType[sourceSnapshot->classId]
						.insert_or_assign(sourceMethodKey, sourceMethodSymbol);
				}
			}
			for (const auto& existingMethod : model.methods) {
				if (existingMethod.id == 0 || existingMethod.name.empty()) {
					continue;
				}
				nativeObjectEncodeContext
					.functionsByName
					.insert_or_assign(
						TypeResolver::NormalizeTypeName(existingMethod.name),
						NativeFunctionSymbol{ -2, existingMethod.id });
				if (existingMethod.ownerClass == 0) {
					continue;
				}
				nativeObjectEncodeContext
					.methodsByOwnerType[existingMethod.ownerClass]
					.insert_or_assign(
						TypeResolver::NormalizeTypeName(existingMethod.name),
						NativeFunctionSymbol{ -2, existingMethod.id });
			}
			if (reusableNativeMethodSnapshot != nullptr) {
				method.lineOffset = reusableNativeMethodSnapshot->lineOffset;
				method.blockOffset = reusableNativeMethodSnapshot->blockOffset;
				method.methodReference = reusableNativeMethodSnapshot->methodReference;
				method.variableReference = reusableNativeMethodSnapshot->variableReference;
				method.constantReference = reusableNativeMethodSnapshot->constantReference;
				method.expressionData = reusableNativeMethodSnapshot->expressionData;
			}
			else if (identityNativeMethodSnapshot != nullptr) {
				std::string semanticError;
				if (!BuildMethodCodeDataWithSemanticNativeObjectCalls(
						parsedMethod.bodyLines,
						method,
						nativeObjectEncodeContext,
						&semanticError)) {
					if (outError != nullptr) {
						*outError =
							"semantic_method_rebuild_failed: " +
							parsedClass.name + "." + parsedMethod.name +
							" (" + parsedClass.sourcePath + ":" +
							std::to_string(parsedMethod.bodyStartLineIndex + 1) + ")";
						if (!semanticError.empty()) {
							*outError += " => " + semanticError;
						}
					}
					return false;
				}
			}
			else if (!BuildMethodCodeData(parsedMethod.bodyLines, method, outError, nullptr)) {
				return false;
			}
			targetClass.functionIds.push_back(method.id);
			model.methods.push_back(std::move(method));
		}
	}

	for (const auto& variable : parsedGlobals) {
		localGlobalModelIndices.push_back(model.globals.size());
		RestoreVariable converted = convertVariable(variable, epl_system_id::kTypeGlobal, false, true);
		if (const auto* snapshot = findReusableGlobalSnapshot(variable);
			snapshot != nullptr && snapshot->id != 0) {
			converted.id = snapshot->id;
		}
		model.globals.push_back(std::move(converted));
	}

	for (size_t structIndex = 0; structIndex < parsedStructs.size(); ++structIndex) {
		const auto& parsedStruct = parsedStructs[structIndex];
		const BundleNativeStructSnapshot* reusableStructSnapshot =
			structIndex < nativeStructSnapshotsByIndex.size() ? nativeStructSnapshotsByIndex[structIndex] : nullptr;
		for (size_t memberIndex = 0; memberIndex < parsedStruct.members.size(); ++memberIndex) {
			RestoreVariable convertedMember =
				convertVariable(parsedStruct.members[memberIndex], epl_system_id::kTypeStructMember, false, false);
			if (reusableStructSnapshot != nullptr &&
				memberIndex < reusableStructSnapshot->memberIds.size() &&
				reusableStructSnapshot->memberIds[memberIndex] != 0) {
				convertedMember.id = reusableStructSnapshot->memberIds[memberIndex];
			}
			model.structs[localStructModelIndices[structIndex]].members.push_back(std::move(convertedMember));
		}
	}

	for (const auto& parsedDll : parsedDlls) {
		const BundleNativeDllSnapshot* reusableDllSnapshot = findReusableDllSnapshot(parsedDll);
		RestoreDll dll;
		dll.id =
			reusableDllSnapshot != nullptr && reusableDllSnapshot->id != 0
			? reusableDllSnapshot->id
			: allocator.Alloc(epl_system_id::kTypeDll);
		dll.memoryAddress = reusableDllSnapshot != nullptr ? reusableDllSnapshot->memoryAddress : 0;
		dll.attr = parsedDll.isPublic ? 0x2 : 0;
		dll.returnType = ensureTypeId(parsedDll.returnTypeName);
		dll.name = parsedDll.name;
		dll.comment = parsedDll.comment;
		dll.fileName = parsedDll.fileName;
		dll.commandName = parsedDll.commandName;
		for (size_t paramIndex = 0; paramIndex < parsedDll.params.size(); ++paramIndex) {
			RestoreVariable converted =
				convertVariable(parsedDll.params[paramIndex], epl_system_id::kTypeDllParameter, false, false);
			if (reusableDllSnapshot != nullptr &&
				paramIndex < reusableDllSnapshot->paramIds.size() &&
				reusableDllSnapshot->paramIds[paramIndex] != 0) {
				converted.id = reusableDllSnapshot->paramIds[paramIndex];
			}
			dll.params.push_back(std::move(converted));
		}
		localDllModelIndices.push_back(model.dlls.size());
		model.dlls.push_back(std::move(dll));
	}

	for (const auto& parsedConstant : parsedConstants) {
		const BundleNativeConstantSnapshot* reusableConstantSnapshot =
			findReusableValueConstantSnapshot(parsedConstant);
		RestoreConstant constant;
		constant.id =
			reusableConstantSnapshot != nullptr && reusableConstantSnapshot->id != 0
			? reusableConstantSnapshot->id
			: allocator.Alloc(epl_system_id::kTypeConstant);
		constant.attr = parsedConstant.isPublic ? kConstAttrPublic : 0;
		if (parsedConstant.isLongText) {
			constant.attr |= kConstAttrLongText;
		}
		constant.name = parsedConstant.name;
		constant.comment = parsedConstant.comment;
		constant.valueText = parsedConstant.valueText;
		localConstantKeys.push_back(BuildBundleItemKey("constant", parsedConstant.name, localConstantKeyCounters));
		localConstantModelIndices.push_back(model.constants.size());
		model.constants.push_back(std::move(constant));
	}

	std::unordered_map<std::string, std::int32_t> formClassIds;
	std::unordered_map<std::string, std::int32_t> preferredFormIds;
	for (const auto& [formName, classIndex] : formClassMatches) {
		if (classIndex < localClassModelIndices.size()) {
			const auto& matchedClass = model.classes[localClassModelIndices[classIndex]];
			formClassIds.insert_or_assign(formName, matchedClass.id);
			if (matchedClass.formId != 0) {
				preferredFormIds.insert_or_assign(formName, matchedClass.formId);
			}
		}
	}
	if (!BuildFormsFromXml(parsedForms, formClassIds, preferredFormIds, model, resolver, allocator, model.forms, outError)) {
		return false;
	}

	for (auto& item : model.classes) {
		if (!item.isFormClass) {
			continue;
		}
		for (const auto& form : model.forms) {
			if (form.classId == item.id) {
				item.formId = form.id;
				break;
			}
		}
	}

	if (bundle != nullptr) {
		const size_t baseConstantCount = model.constants.size();
		std::vector<size_t> resourceModelIndices;
		resourceModelIndices.reserve(bundle->resources.size());
		for (const auto& resource : bundle->resources) {
			RestoreConstant constant;
			const BundleNativeConstantSnapshot* reusableResourceSnapshot = findReusableResourceSnapshot(resource);
			constant.id =
				reusableResourceSnapshot != nullptr && reusableResourceSnapshot->id != 0
				? reusableResourceSnapshot->id
				: allocator.Alloc(resource.kind == BundleResourceKind::Image
					? epl_system_id::kTypeImageResource
					: epl_system_id::kTypeSoundResource);
			constant.attr = resource.isPublic ? kConstAttrPublic : 0;
			constant.pageType = resource.kind == BundleResourceKind::Image ? kConstPageImage : kConstPageSound;
			constant.name = resource.logicalName;
			constant.comment = resource.comment;
			constant.rawData = resource.data;
			resourceModelIndices.push_back(model.constants.size());
			model.constants.push_back(std::move(constant));
		}

		std::unordered_map<std::string, std::int32_t> keyToId;
		for (size_t index = 0; index < bundle->sourceFiles.size() && index < localClassModelIndices.size(); ++index) {
			keyToId.insert_or_assign(bundle->sourceFiles[index].key, model.classes[localClassModelIndices[index]].id);
		}
		for (size_t index = 0; index < bundle->formFiles.size() && index < model.forms.size(); ++index) {
			keyToId.insert_or_assign(bundle->formFiles[index].key, model.forms[index].id);
		}

		std::unordered_map<std::string, int> fixedKeyCounters;
		for (size_t index = 0; index < parsedGlobals.size() && index < localGlobalModelIndices.size(); ++index) {
			keyToId.insert_or_assign(BuildBundleItemKey("global", parsedGlobals[index].name, fixedKeyCounters), model.globals[localGlobalModelIndices[index]].id);
		}
		for (size_t index = 0; index < parsedStructs.size() && index < localStructModelIndices.size(); ++index) {
			keyToId.insert_or_assign(BuildBundleItemKey("struct", parsedStructs[index].name, fixedKeyCounters), model.structs[localStructModelIndices[index]].id);
		}
		for (size_t index = 0; index < parsedDlls.size() && index < localDllModelIndices.size(); ++index) {
			keyToId.insert_or_assign(BuildBundleItemKey("dll", parsedDlls[index].name, fixedKeyCounters), model.dlls[localDllModelIndices[index]].id);
		}
		for (size_t index = 0; index < parsedConstants.size() && index < localConstantModelIndices.size(); ++index) {
			keyToId.insert_or_assign(BuildBundleItemKey("constant", parsedConstants[index].name, fixedKeyCounters), model.constants[localConstantModelIndices[index]].id);
		}
		for (size_t index = 0; index < bundle->resources.size(); ++index) {
			keyToId.insert_or_assign(bundle->resources[index].key, model.constants[baseConstantCount + index].id);
		}

		for (const auto& folder : bundle->folders) {
			RestoreFolder modelFolder;
			modelFolder.key = folder.key;
			modelFolder.parentKey = folder.parentKey;
			modelFolder.expand = folder.expand;
			modelFolder.name = folder.name;
			for (const auto& childKey : folder.childKeys) {
				if (StartsWith(childKey, "folder:")) {
					std::int32_t value = 0;
					if (TryParseInt32(childKey.substr(std::string("folder:").size()), value)) {
						modelFolder.children.push_back(value);
					}
					continue;
				}
				if (const auto it = keyToId.find(childKey); it != keyToId.end()) {
					modelFolder.children.push_back(it->second);
				}
			}
			model.folders.push_back(std::move(modelFolder));
		}
		model.folderAllocatedKey = bundle->folderAllocatedKey;
		if (model.folderAllocatedKey == 0) {
			for (const auto& folder : model.folders) {
				model.folderAllocatedKey = (std::max)(model.folderAllocatedKey, folder.key);
				model.folderAllocatedKey = (std::max)(model.folderAllocatedKey, folder.parentKey);
			}
		}

		std::unordered_map<std::string, size_t> constantIndexByKey;
		for (size_t index = 0; index < localConstantKeys.size() && index < localConstantModelIndices.size(); ++index) {
			constantIndexByKey.insert_or_assign(localConstantKeys[index], localConstantModelIndices[index]);
		}
		for (size_t index = 0; index < bundle->resources.size() && index < resourceModelIndices.size(); ++index) {
			constantIndexByKey.insert_or_assign(bundle->resources[index].key, resourceModelIndices[index]);
		}

		std::vector<size_t> orderedConstantIndices;
		orderedConstantIndices.reserve(model.constants.size());
		std::unordered_set<size_t> assignedConstantIndices;
		for (const auto& childKey : bundle->rootChildKeys) {
			const auto it = constantIndexByKey.find(childKey);
			if (it == constantIndexByKey.end()) {
				continue;
			}
			if (assignedConstantIndices.insert(it->second).second) {
				orderedConstantIndices.push_back(it->second);
			}
		}

		std::vector<RestoreConstant> reorderedConstants;
		reorderedConstants.reserve(model.constants.size());
		for (const auto index : orderedConstantIndices) {
			reorderedConstants.push_back(std::move(model.constants[index]));
		}
		for (size_t index = 0; index < model.constants.size(); ++index) {
			if (!assignedConstantIndices.contains(index)) {
				reorderedConstants.push_back(std::move(model.constants[index]));
			}
		}
		model.constants = std::move(reorderedConstants);
	}

	outModel = std::move(model);
	return true;
}

bool CanReuseNativeBundleSnapshot(const ProjectBundle& bundle)
{
	if (bundle.nativeSourceBytes.empty() || bundle.nativeBundleDigest.empty()) {
		return false;
	}
	return ComputeBundleDigest(bundle) == bundle.nativeBundleDigest;
}

struct SectionEmitInfo {
	std::uint32_t key = 0;
	std::string name;
	std::int32_t flags = 0;
	std::vector<std::uint8_t> data;
};

const NativeSectionSnapshot* FindNativeSectionSnapshot(
	const std::vector<NativeSectionSnapshot>& snapshots,
	const std::uint32_t key)
{
	for (const auto& snapshot : snapshots) {
		if (snapshot.key == key) {
			return &snapshot;
		}
	}
	return nullptr;
}

bool AreWindowBindingsEquivalent(const ProjectBundle& left, const ProjectBundle& right)
{
	if (left.windowBindings.size() != right.windowBindings.size()) {
		return false;
	}
	for (size_t index = 0; index < left.windowBindings.size(); ++index) {
		const auto& lhs = left.windowBindings[index];
		const auto& rhs = right.windowBindings[index];
		if (lhs.formName != rhs.formName || lhs.className != rhs.className) {
			return false;
		}
	}
	return true;
}

bool AreFormFilesEquivalent(const ProjectBundle& left, const ProjectBundle& right)
{
	if (left.formFiles.size() != right.formFiles.size()) {
		return false;
	}
	for (size_t index = 0; index < left.formFiles.size(); ++index) {
		const auto& lhs = left.formFiles[index];
		const auto& rhs = right.formFiles[index];
		if (lhs.key != rhs.key ||
			lhs.logicalName != rhs.logicalName ||
			lhs.xmlText != rhs.xmlText) {
			return false;
		}
	}
	return true;
}

bool AreResourcesEquivalent(const ProjectBundle& left, const ProjectBundle& right)
{
	if (left.resources.size() != right.resources.size()) {
		return false;
	}
	for (size_t index = 0; index < left.resources.size(); ++index) {
		const auto& lhs = left.resources[index];
		const auto& rhs = right.resources[index];
		if (lhs.kind != rhs.kind ||
			lhs.key != rhs.key ||
			lhs.logicalName != rhs.logicalName ||
			lhs.comment != rhs.comment ||
			lhs.isPublic != rhs.isPublic ||
			lhs.data != rhs.data) {
			return false;
		}
	}
	return true;
}

bool AreFolderEntriesEquivalent(const BundleFolder& left, const BundleFolder& right)
{
	return left.key == right.key &&
		left.parentKey == right.parentKey &&
		left.expand == right.expand &&
		left.name == right.name &&
		left.childKeys == right.childKeys;
}

bool AreFolderSectionsEquivalent(const ProjectBundle& left, const ProjectBundle& right)
{
	if (left.folderAllocatedKey != right.folderAllocatedKey ||
		left.rootChildKeys != right.rootChildKeys ||
		left.folders.size() != right.folders.size()) {
		return false;
	}
	for (size_t index = 0; index < left.folders.size(); ++index) {
		if (!AreFolderEntriesEquivalent(left.folders[index], right.folders[index])) {
			return false;
		}
	}
	return true;
}

std::vector<const Dependency*> CollectEComDependencies(const ProjectBundle& bundle)
{
	std::vector<const Dependency*> out;
	for (const auto& dependency : bundle.dependencies) {
		if (dependency.kind == DependencyKind::ECom) {
			out.push_back(&dependency);
		}
	}
	return out;
}

bool AreEComDependenciesEquivalent(const ProjectBundle& left, const ProjectBundle& right)
{
	const auto lhs = CollectEComDependencies(left);
	const auto rhs = CollectEComDependencies(right);
	if (lhs.size() != rhs.size()) {
		return false;
	}
	for (size_t index = 0; index < lhs.size(); ++index) {
		if (lhs[index]->name != rhs[index]->name ||
			lhs[index]->path != rhs[index]->path ||
			lhs[index]->reExport != rhs[index]->reExport) {
			return false;
		}
	}
	return true;
}

bool AreProjectConfigEquivalent(const ProjectBundle& left, const ProjectBundle& right)
{
	if (!left.projectNameStored) {
		return false;
	}
	return left.projectName == right.projectName && left.versionText == right.versionText;
}

bool AreResourceSectionsEquivalent(const ProjectBundle& left, const ProjectBundle& right)
{
	return left.constantText == right.constantText &&
		AreFormFilesEquivalent(left, right) &&
		AreResourcesEquivalent(left, right) &&
		AreWindowBindingsEquivalent(left, right);
}

bool IsStandardSerializedSectionKey(const std::uint32_t key)
{
	return key == kSectionSystemInfo ||
		key == kSectionProjectConfig ||
		key == kSectionResource ||
		key == kSectionCode ||
		key == kSectionLosable ||
		key == kSectionEventIndices ||
		key == kSectionEPackageInfo ||
		key == kSectionClassPublicity ||
		key == kSectionEcDependencies ||
		key == kSectionFolder;
}

bool ParseLengthPrefixedTextArray(
	const std::vector<std::uint8_t>& data,
	std::vector<std::string>& outValues)
{
	outValues.clear();
	size_t offset = 0;
	while (offset < data.size()) {
		if (offset + sizeof(std::int32_t) > data.size()) {
			return false;
		}

		std::int32_t length = 0;
		std::memcpy(&length, data.data() + offset, sizeof(length));
		offset += sizeof(length);
		if (length < 0 || offset + static_cast<size_t>(length) > data.size()) {
			return false;
		}

		outValues.emplace_back(
			reinterpret_cast<const char*>(data.data() + offset),
			static_cast<size_t>(length));
		offset += static_cast<size_t>(length);
	}
	return true;
}

std::vector<std::int32_t> CollectNativeSourceMethodIds(const ProjectBundle& bundle)
{
	std::vector<std::int32_t> methodIds;
	for (const auto& snapshot : bundle.nativeSourceSnapshots) {
		for (const auto& method : snapshot.methods) {
			methodIds.push_back(method.id);
		}
	}
	return methodIds;
}

bool TryCollectEPackageMethodIds(
	const ProjectBundle& bundle,
	const size_t expectedCount,
	std::vector<std::int32_t>& outMethodIds)
{
	outMethodIds.clear();

	const Document document = BuildDocumentFromBundle(bundle);
	RestoreDocumentModel model;
	std::string error;
	if (BuildRestoreModel(document, &bundle, model, &error) &&
		model.methods.size() == expectedCount) {
		outMethodIds.reserve(model.methods.size());
		for (const auto& method : model.methods) {
			outMethodIds.push_back(method.id);
		}
		return true;
	}

	outMethodIds = CollectNativeSourceMethodIds(bundle);
	if (outMethodIds.size() == expectedCount) {
		return true;
	}

	outMethodIds.clear();
	return false;
}

std::vector<std::uint8_t> BuildEPackageInfoSection(
	const RestoreDocumentModel& model,
	const ProjectBundle* originalBundle,
	const NativeSectionSnapshot* originalSection)
{
	std::vector<std::string> currentEntries(model.methods.size());
	if (originalBundle != nullptr && originalSection != nullptr) {
		std::vector<std::string> originalEntries;
		std::vector<std::int32_t> originalMethodIds;
		if (ParseLengthPrefixedTextArray(originalSection->data, originalEntries) &&
			TryCollectEPackageMethodIds(*originalBundle, originalEntries.size(), originalMethodIds) &&
			originalMethodIds.size() == originalEntries.size()) {
			std::unordered_map<std::int32_t, std::string> entryByMethodId;
			entryByMethodId.reserve(originalEntries.size());
			for (size_t index = 0; index < originalEntries.size(); ++index) {
				entryByMethodId.insert_or_assign(originalMethodIds[index], originalEntries[index]);
			}
			for (size_t index = 0; index < model.methods.size(); ++index) {
				if (const auto it = entryByMethodId.find(model.methods[index].id);
					it != entryByMethodId.end()) {
					currentEntries[index] = it->second;
				}
			}
		}
	}

	ByteWriter writer;
	for (const auto& entry : currentEntries) {
		writer.WriteDynamicText(entry);
	}
	return writer.TakeBytes();
}

std::vector<std::uint8_t> BuildEventIndicesSection(
	const RestoreDocumentModel& model,
	const ProjectBundle* currentBundle,
	const NativeSectionSnapshot* originalSection)
{
	ByteWriter writer;
	size_t eventCount = 0;
	for (const auto& form : model.forms) {
		for (const auto& element : form.elements) {
			if (element.isMenu) {
				if (element.clickEvent != 0) {
					writer.WriteI32(form.id);
					writer.WriteI32(element.id);
					writer.WriteI32(0);
					writer.WriteI32(element.clickEvent);
					++eventCount;
				}
				continue;
			}

			for (const auto& [eventKey, handlerId] : element.events) {
				writer.WriteI32(form.id);
				writer.WriteI32(element.id);
				writer.WriteI32(eventKey);
				writer.WriteI32(handlerId);
				++eventCount;
			}
		}
	}

	if (eventCount == 0 &&
		currentBundle != nullptr &&
		currentBundle->bundleFormatVersion < 2 &&
		originalSection != nullptr) {
		return originalSection->data;
	}

	return writer.TakeBytes();
}

void WriteInt32ArrayPayload(ByteWriter& writer, const std::vector<std::int32_t>& values)
{
	for (const auto value : values) {
		writer.WriteI32(value);
	}
}

void WriteInt16ArrayPayload(ByteWriter& writer, const std::vector<std::int16_t>& values)
{
	for (const auto value : values) {
		writer.WriteI16(value);
	}
}

void WriteInt32ArrayWithByteSizePrefix(ByteWriter& writer, const std::vector<std::int32_t>& values)
{
	ByteWriter payload;
	WriteInt32ArrayPayload(payload, values);
	writer.WriteDynamicBytes(payload.bytes());
}

void WriteInt16ArrayWithByteSizePrefix(ByteWriter& writer, const std::vector<std::int16_t>& values)
{
	ByteWriter payload;
	WriteInt16ArrayPayload(payload, values);
	writer.WriteDynamicBytes(payload.bytes());
}

void WriteVariableBlockPayload(ByteWriter& writer, const std::vector<RestoreVariable>& variables)
{
	if (variables.empty()) {
		writer.WriteI32(0);
		writer.WriteDynamicBytes({});
		return;
	}

	ByteWriter payload;
	for (const auto& variable : variables) {
		payload.WriteI32(variable.id);
	}

	std::vector<std::int32_t> offsets;
	offsets.reserve(variables.size());
	ByteWriter body;
	for (const auto& variable : variables) {
		offsets.push_back(static_cast<std::int32_t>(body.position()));
		ByteWriter item;
		item.WriteI32(0);
		item.WriteI32(variable.dataType);
		item.WriteI16(variable.attr);
		item.WriteU8(static_cast<std::uint8_t>(variable.arrayBounds.size()));
		for (const auto bound : variable.arrayBounds) {
			item.WriteI32(bound);
		}
		item.WriteStandardText(variable.name);
		item.WriteStandardText(variable.comment);
		const std::int32_t itemLength = static_cast<std::int32_t>(item.position() - sizeof(std::int32_t));
		item.PatchI32(0, itemLength);
		body.WriteBytes(item.bytes());
	}
	for (const auto offset : offsets) {
		payload.WriteI32(offset);
	}
	payload.WriteBytes(body.bytes());

	writer.WriteI32(static_cast<std::int32_t>(variables.size()));
	writer.WriteDynamicBytes(payload.bytes());
}

std::int32_t ComputeDefaultMethodAttr(const ParsedMethodDef& method)
{
	if (method.name == "_启动子程序" ||
		method.name == "_临时子程序" ||
		method.name == "template_DownProgFunc") {
		return 0;
	}
	return 0x30 | (method.isPublic ? 0x8 : 0);
}

template <typename TItem, typename TWriter>
void WriteBlocksWithIdAndMemoryAddress(
	ByteWriter& writer,
	const std::vector<TItem>& items,
	TWriter&& itemWriter)
{
	writer.WriteI32(static_cast<std::int32_t>(items.size() * 8));
	for (const auto& item : items) {
		writer.WriteI32(item.id);
	}
	for (const auto& item : items) {
		writer.WriteI32(item.memoryAddress);
	}
	for (const auto& item : items) {
		itemWriter(writer, item);
	}
}

template <typename TItem, typename TWriter>
void WriteBlocksWithIdAndOffset(
	ByteWriter& writer,
	const std::vector<TItem>& items,
	TWriter&& itemWriter)
{
	writer.WriteI32(static_cast<std::int32_t>(items.size()));
	if (items.empty()) {
		writer.WriteI32(0);
		return;
	}

	std::vector<std::vector<std::uint8_t>> encodedItems;
	encodedItems.reserve(items.size());
	for (const auto& item : items) {
		ByteWriter itemData;
		itemWriter(itemData, item);
		ByteWriter withLength;
		withLength.WriteI32(static_cast<std::int32_t>(itemData.position()));
		withLength.WriteBytes(itemData.bytes());
		encodedItems.push_back(withLength.TakeBytes());
	}

	std::int32_t totalSize = static_cast<std::int32_t>(items.size() * 8);
	std::vector<std::int32_t> offsets;
	offsets.reserve(items.size());
	std::int32_t offset = 0;
	for (const auto& encoded : encodedItems) {
		offsets.push_back(offset);
		offset += static_cast<std::int32_t>(encoded.size());
		totalSize += static_cast<std::int32_t>(encoded.size());
	}

	writer.WriteI32(totalSize);
	for (const auto& item : items) {
		writer.WriteI32(item.id);
	}
	for (const auto itemOffset : offsets) {
		writer.WriteI32(itemOffset);
	}
	for (const auto& encoded : encodedItems) {
		writer.WriteBytes(encoded);
	}
}

void WriteFormElements(ByteWriter& writer, const std::vector<RestoreFormElement>& elements)
{
	WriteBlocksWithIdAndOffset(writer, elements, [](ByteWriter& itemWriter, const RestoreFormElement& element) {
		itemWriter.WriteI32(element.dataType);
		itemWriter.WriteBytes(std::vector<std::uint8_t>(20, 0));
		itemWriter.WriteStandardText(element.name);
		if (element.isMenu) {
			itemWriter.WriteStandardText(std::string());
			itemWriter.WriteI32(element.hotKey);
			itemWriter.WriteI32(element.level);
			std::int32_t showStatus = (element.visible ? 0 : 0x1) | (element.disable ? 0x2 : 0) | (element.selected ? 0x4 : 0);
			itemWriter.WriteI32(showStatus);
			itemWriter.WriteStandardText(element.text);
			itemWriter.WriteI32(element.clickEvent);
			itemWriter.WriteBytes(std::vector<std::uint8_t>(16, 0));
			return;
		}

		itemWriter.WriteStandardText(element.comment);
		itemWriter.WriteI32(element.cWndAddress);
		itemWriter.WriteI32(element.left);
		itemWriter.WriteI32(element.top);
		itemWriter.WriteI32(element.width);
		itemWriter.WriteI32(element.height);
		itemWriter.WriteI32(element.unknownBeforeParent);
		itemWriter.WriteI32(element.parent);
		itemWriter.WriteI32(static_cast<std::int32_t>(element.children.size()));
		for (const auto child : element.children) {
			itemWriter.WriteI32(child);
		}
		itemWriter.WriteDynamicBytes(element.cursor);
		itemWriter.WriteStandardText(element.tag);
		itemWriter.WriteI32(element.unknownBeforeVisible);
		std::int32_t showStatus = (element.visible ? 0x1 : 0) | (element.disable ? 0x2 : 0) |
			(element.tabStop ? 0x4 : 0) | (element.locked ? 0x10 : 0);
		itemWriter.WriteI32(showStatus);
		itemWriter.WriteI32(element.tabIndex);
		itemWriter.WriteI32(static_cast<std::int32_t>(element.events.size()));
		for (const auto& [key, value] : element.events) {
			itemWriter.WriteI32(key);
			itemWriter.WriteI32(value);
		}
		itemWriter.WriteBytes(std::vector<std::uint8_t>(20, 0));
		itemWriter.WriteBytes(element.extensionData);
	});
}

void WriteForms(ByteWriter& writer, const std::vector<RestoreForm>& forms)
{
	WriteBlocksWithIdAndMemoryAddress(writer, forms, [](ByteWriter& out, const RestoreForm& form) {
		out.WriteI32(form.unknown1);
		out.WriteI32(form.classId);
		out.WriteDynamicText(form.name);
		out.WriteDynamicText(form.comment);
		WriteFormElements(out, form.elements);
	});
}

void WriteConstants(ByteWriter& writer, const std::vector<RestoreConstant>& constants, std::string* outError)
{
	WriteBlocksWithIdAndOffset(writer, constants, [&](ByteWriter& out, const RestoreConstant& constant) {
		out.WriteI16(constant.attr);
		out.WriteStandardText(constant.name);
		out.WriteStandardText(constant.comment);
		if (constant.pageType == kConstPageImage || constant.pageType == kConstPageSound) {
			out.WriteDynamicBytes(constant.rawData);
			return;
		}

		const std::string valueText = constant.valueText;
		std::string decodedText;
		bool decodedLongText = false;
		if (TryDecodeDumpTextLiteral(valueText, decodedText, decodedLongText)) {
			out.WriteU8(kConstTypeText);
			out.WriteBStr(std::make_optional(decodedText));
			return;
		}
		(void)decodedLongText;
		if (valueText.empty()) {
			out.WriteU8(kConstTypeEmpty);
			return;
		}
		if (StartsWith(valueText, "[") && EndsWith(valueText, "]")) {
			// v1 仅保留格式，可继续扩展成完整日期解析。
			out.WriteU8(kConstTypeText);
			out.WriteBStr(std::make_optional(valueText));
			return;
		}
		if (StartsWith(valueText, "“") && EndsWith(valueText, "”")) {
			out.WriteU8(kConstTypeText);
			out.WriteBStr(std::make_optional(StripWrappedText(valueText, "“", "”")));
			return;
		}

		if (StartsWith(valueText, "<文本长度:") && EndsWith(valueText, ">")) {
			const size_t colonPos = valueText.find(':');
			const size_t endPos = valueText.rfind('>');
			std::int32_t length = 0;
			if (colonPos != std::string::npos && endPos != std::string::npos && colonPos + 1 < endPos) {
				TryParseInt32(valueText.substr(colonPos + 1, endPos - colonPos - 1), length);
			}
			out.WriteU8(kConstTypeText);
			out.WriteBStr(std::make_optional(std::string((std::max)(length, 0), ' ')));
			return;
		}

		double numberValue = 0.0;
		if (TryParseDouble(valueText, numberValue)) {
			out.WriteU8(kConstTypeNumber);
			out.WriteDouble(numberValue);
			return;
		}
		if (const auto boolValue = ParseBoolLiteral(valueText); boolValue.has_value()) {
			out.WriteU8(kConstTypeBool);
			out.WriteBool32(*boolValue);
			return;
		}

		out.WriteU8(kConstTypeText);
		out.WriteBStr(std::make_optional(valueText));
	});
	(void)outError;
}

std::uint32_t ComputeChecksum(const std::vector<std::uint8_t>& data)
{
	std::array<std::uint8_t, 4> checksum = {};
	for (size_t i = 0; i < data.size(); ++i) {
		checksum[i & 0x3] ^= data[i];
	}
	return
		(static_cast<std::uint32_t>(checksum[3]) << 24) |
		(static_cast<std::uint32_t>(checksum[2]) << 16) |
		(static_cast<std::uint32_t>(checksum[1]) << 8) |
		static_cast<std::uint32_t>(checksum[0]);
}

std::array<std::uint8_t, 30> EncodeSectionName(const std::uint32_t key, const std::string& name)
{
	std::array<std::uint8_t, 30> encoded = {};
	if (!name.empty()) {
		std::memcpy(encoded.data(), name.data(), (std::min)(encoded.size(), name.size()));
	}
	if (key != kSectionEndOfFile) {
		const auto* keyBytes = reinterpret_cast<const std::uint8_t*>(&key);
		for (size_t i = 0; i < encoded.size(); ++i) {
			encoded[i] ^= keyBytes[(i + 1) % 4];
		}
	}
	return encoded;
}

void WriteSection(
	ByteWriter& writer,
	const std::uint32_t key,
	const std::string& name,
	const std::int32_t flags,
	const std::int32_t index,
	const std::vector<std::uint8_t>& data)
{
	ByteWriter headerInfo;
	headerInfo.WriteU32(key);
	const auto encodedName = EncodeSectionName(key, name);
	headerInfo.WriteRaw(encodedName.data(), encodedName.size());
	headerInfo.WriteI16(0);
	headerInfo.WriteI32(index);
	headerInfo.WriteI32(flags);
	headerInfo.WriteI32(static_cast<std::int32_t>(ComputeChecksum(data)));
	headerInfo.WriteI32(static_cast<std::int32_t>(data.size()));
	for (int i = 0; i < 10; ++i) {
		headerInfo.WriteI32(0);
	}

	writer.WriteU32(kMagicSection);
	writer.WriteU32(ComputeChecksum(headerInfo.bytes()));
	writer.WriteBytes(headerInfo.bytes());
	writer.WriteBytes(data);
}

std::pair<std::int32_t, std::int32_t> ParseVersionPair(const std::string& versionText)
{
	std::int32_t major = 1;
	std::int32_t minor = 0;
	const size_t dotPos = versionText.find('.');
	if (dotPos == std::string::npos) {
		TryParseInt32(versionText, major);
		return { major, minor };
	}
	TryParseInt32(versionText.substr(0, dotPos), major);
	TryParseInt32(versionText.substr(dotPos + 1), minor);
	return { major, minor };
}

std::vector<std::string> BuildSupportLibraryInfoText(const std::vector<RestoreDependencyInfo>& dependencies)
{
	std::vector<std::string> values;
	for (const auto& dependency : dependencies) {
		if (!dependency.isSupportLibrary) {
			continue;
		}
		auto [major, minor] = ParseVersionPair(dependency.versionText);
		values.push_back(
			dependency.fileName + "\r" +
			dependency.guid + "\r" +
			std::to_string(major) + "\r" +
			std::to_string(minor) + "\r" +
			dependency.name);
	}
	return values;
}

std::int32_t ComputeAllocatedIdNum(const RestoreDocumentModel& model)
{
	std::int32_t maxId = 0xFFFF;
	const auto update = [&](const std::int32_t id) {
		if ((id & epl_system_id::kMaskType) != 0) {
			maxId = (std::max)(maxId, id & epl_system_id::kMaskNum);
		}
	};

	for (const auto& item : model.classes) {
		update(item.id);
		for (const auto& variable : item.vars) {
			update(variable.id);
		}
	}
	for (const auto& item : model.methods) {
		update(item.id);
		for (const auto& variable : item.params) {
			update(variable.id);
		}
		for (const auto& variable : item.locals) {
			update(variable.id);
		}
	}
	for (const auto& item : model.globals) {
		update(item.id);
	}
	for (const auto& item : model.structs) {
		update(item.id);
		for (const auto& member : item.members) {
			update(member.id);
		}
	}
	for (const auto& item : model.dlls) {
		update(item.id);
		for (const auto& param : item.params) {
			update(param.id);
		}
	}
	for (const auto& item : model.constants) {
		update(item.id);
	}
	for (const auto& item : model.forms) {
		update(item.id);
		for (const auto& element : item.elements) {
			update(element.id);
		}
	}
	return maxId;
}

std::vector<std::uint8_t> BuildSystemInfoSection(const RestoreDocumentModel&)
{
	ByteWriter writer;
	writer.WriteI16(5);
	writer.WriteI16(6);
	writer.WriteI32(1);
	writer.WriteI32(1);
	writer.WriteI16(1);
	writer.WriteI16(7);
	writer.WriteI32(1);
	writer.WriteI32(0);
	writer.WriteI32(0);
	for (int i = 0; i < 8; ++i) {
		writer.WriteI32(0);
	}
	return writer.TakeBytes();
}

std::vector<std::uint8_t> BuildProjectConfigSection(const RestoreDocumentModel& model)
{
	const auto [major, minor] = ParseVersionPair(model.versionText);
	ByteWriter writer;
	writer.WriteDynamicText(model.projectName);
	writer.WriteDynamicText(std::string());
	writer.WriteDynamicText(std::string());
	writer.WriteDynamicText(std::string());
	writer.WriteDynamicText(std::string());
	writer.WriteDynamicText(std::string());
	writer.WriteDynamicText(std::string());
	writer.WriteDynamicText(std::string());
	writer.WriteDynamicText(std::string());
	writer.WriteDynamicText(std::string());
	writer.WriteI32(major);
	writer.WriteI32(minor);
	writer.WriteI32(0);
	writer.WriteI32(0);
	writer.WriteI32(0);
	writer.WriteRaw(std::vector<std::uint8_t>(20, 0).data(), 20);
	writer.WriteI32(0);
	writer.WriteI32(0);
	return writer.TakeBytes();
}

std::vector<std::uint8_t> BuildCodeSection(
	const RestoreDocumentModel& model,
	const BundleNativeProgramHeaderSnapshot* nativeProgramHeader)
{
	ByteWriter writer;
	const std::vector<std::string> supportLibraryInfo = BuildSupportLibraryInfoText(model.dependencies);
	const bool canReuseNativeProgramHeader =
		nativeProgramHeader != nullptr &&
		nativeProgramHeader->supportLibraryInfo == supportLibraryInfo;
	const std::int32_t allocatedIdNum = ComputeAllocatedIdNum(model);

	writer.WriteI32(canReuseNativeProgramHeader
		? (std::max)(allocatedIdNum, nativeProgramHeader->versionFlag1)
		: allocatedIdNum);
	writer.WriteI32(canReuseNativeProgramHeader
		? nativeProgramHeader->unk1
		: kProgramHeaderUnk1);

	if (canReuseNativeProgramHeader) {
		writer.WriteDynamicBytes(nativeProgramHeader->unk2_1);
		writer.WriteDynamicBytes(nativeProgramHeader->unk2_2);
		writer.WriteDynamicBytes(nativeProgramHeader->unk2_3);
	}
	else {
		const size_t supportCount = std::count_if(
			model.dependencies.begin(),
			model.dependencies.end(),
			[](const RestoreDependencyInfo& item) { return item.isSupportLibrary; });
		std::vector<std::int32_t> minCmd(static_cast<size_t>(supportCount), 0);
		std::vector<std::int16_t> minType(static_cast<size_t>(supportCount), 0);
		std::vector<std::int16_t> minConst(static_cast<size_t>(supportCount), 0);
		WriteInt32ArrayWithByteSizePrefix(writer, minCmd);
		WriteInt16ArrayWithByteSizePrefix(writer, minType);
		WriteInt16ArrayWithByteSizePrefix(writer, minConst);
	}
	writer.WriteTextArray(supportLibraryInfo);
	writer.WriteI32(canReuseNativeProgramHeader ? nativeProgramHeader->flag1 : 0);
	writer.WriteI32(canReuseNativeProgramHeader ? nativeProgramHeader->flag2 : 0);
	if (canReuseNativeProgramHeader && (nativeProgramHeader->flag1 & 0x1) != 0) {
		writer.WriteBytes(nativeProgramHeader->unk3Op);
	}
	writer.WriteDynamicBytes(canReuseNativeProgramHeader ? nativeProgramHeader->icon : std::vector<std::uint8_t>{});
	writer.WriteDynamicText(canReuseNativeProgramHeader ? nativeProgramHeader->debugCommandLine : std::string());

	WriteBlocksWithIdAndMemoryAddress(writer, model.classes, [](ByteWriter& out, const RestoreClass& item) {
		out.WriteI32(item.formId);
		out.WriteI32(item.baseClass);
		out.WriteDynamicText(item.name);
		out.WriteDynamicText(item.comment);
		out.WriteI32(static_cast<std::int32_t>(item.functionIds.size() * 4));
		for (const auto functionId : item.functionIds) {
			out.WriteI32(functionId);
		}
		WriteVariableBlockPayload(out, item.vars);
	});

	WriteBlocksWithIdAndMemoryAddress(writer, model.methods, [](ByteWriter& out, const RestoreMethod& item) {
		out.WriteI32(item.ownerClass);
		out.WriteI32(item.attr);
		out.WriteI32(item.returnType);
		out.WriteDynamicText(item.name);
		out.WriteDynamicText(item.comment);
		WriteVariableBlockPayload(out, item.locals);
		WriteVariableBlockPayload(out, item.params);
		out.WriteDynamicBytes(item.lineOffset);
		out.WriteDynamicBytes(item.blockOffset);
		out.WriteDynamicBytes(item.methodReference);
		out.WriteDynamicBytes(item.variableReference);
		out.WriteDynamicBytes(item.constantReference);
		out.WriteDynamicBytes(item.expressionData);
	});

	WriteVariableBlockPayload(writer, model.globals);

	WriteBlocksWithIdAndMemoryAddress(writer, model.structs, [](ByteWriter& out, const RestoreStruct& item) {
		out.WriteI32(item.attr);
		out.WriteDynamicText(item.name);
		out.WriteDynamicText(item.comment);
		WriteVariableBlockPayload(out, item.members);
	});

	WriteBlocksWithIdAndMemoryAddress(writer, model.dlls, [](ByteWriter& out, const RestoreDll& item) {
		out.WriteI32(item.attr);
		out.WriteI32(item.returnType);
		out.WriteDynamicText(item.name);
		out.WriteDynamicText(item.comment);
		out.WriteDynamicText(item.fileName);
		out.WriteDynamicText(item.commandName);
		WriteVariableBlockPayload(out, item.params);
	});

	writer.WriteBytes(std::vector<std::uint8_t>(40, 0));
	return writer.TakeBytes();
}

std::vector<std::uint8_t> BuildResourceSection(const RestoreDocumentModel& model, std::string* outError)
{
	ByteWriter writer;
	WriteForms(writer, model.forms);
	WriteConstants(writer, model.constants, outError);
	writer.WriteI32(0);
	return writer.TakeBytes();
}

std::vector<std::uint8_t> BuildClassPublicitySection(const RestoreDocumentModel& model)
{
	ByteWriter writer;
	for (const auto& item : model.classes) {
		std::int32_t flags = 0;
		if (item.isPublic) {
			flags |= 0x1;
		}
		if (item.isHidden) {
			flags |= 0x2;
		}
		if (flags == 0) {
			continue;
		}
		writer.WriteI32(item.id);
		writer.WriteI32(flags);
	}
	return writer.TakeBytes();
}

std::vector<std::uint8_t> BuildFolderSection(const RestoreDocumentModel& model)
{
	ByteWriter writer;
	writer.WriteI32(model.folderAllocatedKey);
	for (const auto& folder : model.folders) {
		writer.WriteI32(folder.expand ? 1 : 0);
		writer.WriteI32(folder.key);
		writer.WriteI32(folder.parentKey);
		writer.WriteDynamicText(folder.name);
		writer.WriteI32(static_cast<std::int32_t>(folder.children.size() * sizeof(std::int32_t)));
		for (const auto child : folder.children) {
			writer.WriteI32(child);
		}
	}
	return writer.TakeBytes();
}

std::vector<std::uint8_t> BuildLosableSection()
{
	ByteWriter writer;
	writer.WriteDynamicText(std::string());
	writer.WriteI32(0);
	writer.WriteI32(0);
	writer.WriteBytes(std::vector<std::uint8_t>(16, 0));
	writer.WriteI32(-1);
	return writer.TakeBytes();
}

std::vector<std::uint8_t> BuildEcDependenciesSection(const RestoreDocumentModel& model)
{
	ByteWriter writer;
	std::vector<const RestoreDependencyInfo*> ecomDependencies;
	for (const auto& dependency : model.dependencies) {
		if (!dependency.isSupportLibrary) {
			ecomDependencies.push_back(&dependency);
		}
	}
	if (ecomDependencies.empty()) {
		return {};
	}

	writer.WriteI32(static_cast<std::int32_t>(ecomDependencies.size()));
	for (const auto* dependency : ecomDependencies) {
		writer.WriteI32(2);

		std::int64_t fileTime = 0;
		std::int32_t fileSize = 0;
		const std::string statsPath = !dependency->resolvedPath.empty() ? dependency->resolvedPath : dependency->path;
		if (!statsPath.empty()) {
			std::error_code ec;
			const std::filesystem::path path =
				!dependency->resolvedPath.empty()
					? Utf8PathToPath(statsPath)
					: std::filesystem::path(statsPath);
			if (std::filesystem::exists(path, ec)) {
				fileSize = static_cast<std::int32_t>(std::filesystem::file_size(path, ec));
				if (!ec) {
					const auto writeTime = std::filesystem::last_write_time(path, ec);
					if (!ec) {
						const auto systemNow = std::chrono::system_clock::now();
						const auto fileNow = decltype(writeTime)::clock::now();
						const auto systemTime = systemNow + (writeTime - fileNow);
						fileTime = std::chrono::duration_cast<std::chrono::nanoseconds>(systemTime.time_since_epoch()).count() / 100 + 116444736000000000LL;
					}
				}
			}
		}
		writer.WriteI32(fileSize);
		writer.WriteI64(fileTime);
		writer.WriteI32(dependency->reExport ? 1 : 0);
		writer.WriteDynamicText(dependency->name);
		writer.WriteDynamicText(dependency->path);
		std::vector<std::int32_t> starts;
		std::vector<std::int32_t> counts;
		starts.reserve(dependency->definedIds.size());
		counts.reserve(dependency->definedIds.size());
		for (const auto& range : dependency->definedIds) {
			if (range.count <= 0) {
				continue;
			}
			starts.push_back(range.start);
			counts.push_back(range.count);
		}
		WriteInt32ArrayWithByteSizePrefix(writer, starts);
		WriteInt32ArrayWithByteSizePrefix(writer, counts);
	}
	return writer.TakeBytes();
}

bool SerializeToModuleBytes(
	const RestoreDocumentModel& model,
	std::vector<std::uint8_t>& outBytes,
	std::string* outError,
	const ProjectBundle* currentBundle = nullptr,
	const ProjectBundle* originalBundle = nullptr,
	const std::vector<NativeSectionSnapshot>* originalSections = nullptr)
{
	if (outError != nullptr) {
		outError->clear();
	}

	RestoreDocumentModel emitModel = model;
	std::sort(
		emitModel.classes.begin(),
		emitModel.classes.end(),
		[](const RestoreClass& left, const RestoreClass& right) {
			return left.id < right.id;
		});
	std::sort(
		emitModel.methods.begin(),
		emitModel.methods.end(),
		[](const RestoreMethod& left, const RestoreMethod& right) {
			return left.id < right.id;
		});
	std::sort(
		emitModel.globals.begin(),
		emitModel.globals.end(),
		[](const RestoreVariable& left, const RestoreVariable& right) {
			return left.id < right.id;
		});
	std::sort(
		emitModel.structs.begin(),
		emitModel.structs.end(),
		[](const RestoreStruct& left, const RestoreStruct& right) {
			return left.id < right.id;
		});
	std::sort(
		emitModel.dlls.begin(),
		emitModel.dlls.end(),
		[](const RestoreDll& left, const RestoreDll& right) {
			return left.id < right.id;
		});
	std::sort(
		emitModel.constants.begin(),
		emitModel.constants.end(),
		[](const RestoreConstant& left, const RestoreConstant& right) {
			return left.id < right.id;
		});
	for (auto& item : emitModel.classes) {
		std::sort(item.functionIds.begin(), item.functionIds.end());
	}

	std::string resourceError;
	const std::vector<std::uint8_t> resourceBytes = BuildResourceSection(emitModel, &resourceError);
	if (!resourceError.empty()) {
		if (outError != nullptr) {
			*outError = resourceError;
		}
		return false;
	}

	const std::vector<std::uint8_t> systemBytes = BuildSystemInfoSection(emitModel);
	const std::vector<std::uint8_t> projectConfigBytes = BuildProjectConfigSection(emitModel);
	const BundleNativeProgramHeaderSnapshot* nativeProgramHeader =
		currentBundle != nullptr && currentBundle->nativeProgramHeader.has_value()
		? &(*currentBundle->nativeProgramHeader)
		: nullptr;
	const std::vector<std::uint8_t> codeBytes = BuildCodeSection(emitModel, nativeProgramHeader);
	const auto* originalEventIndicesSection =
		originalSections != nullptr ? FindNativeSectionSnapshot(*originalSections, kSectionEventIndices) : nullptr;
	const std::vector<std::uint8_t> eventIndicesBytes =
		BuildEventIndicesSection(emitModel, currentBundle, originalEventIndicesSection);
	const auto* originalEPackageInfoSection =
		originalSections != nullptr ? FindNativeSectionSnapshot(*originalSections, kSectionEPackageInfo) : nullptr;
	const std::vector<std::uint8_t> epackageInfoBytes =
		BuildEPackageInfoSection(emitModel, originalBundle, originalEPackageInfoSection);
	const std::vector<std::uint8_t> ecomBytes = BuildEcDependenciesSection(emitModel);
	const std::vector<std::uint8_t> publicityBytes = BuildClassPublicitySection(emitModel);
	const std::vector<std::uint8_t> folderBytes = BuildFolderSection(emitModel);
	const std::vector<std::uint8_t> losableBytes = BuildLosableSection();

	const auto* originalSystemSection =
		originalSections != nullptr ? FindNativeSectionSnapshot(*originalSections, kSectionSystemInfo) : nullptr;
	const auto* originalProjectConfigSection =
		originalSections != nullptr ? FindNativeSectionSnapshot(*originalSections, kSectionProjectConfig) : nullptr;
	const auto* originalResourceSection =
		originalSections != nullptr ? FindNativeSectionSnapshot(*originalSections, kSectionResource) : nullptr;
	const auto* originalCodeSection =
		originalSections != nullptr ? FindNativeSectionSnapshot(*originalSections, kSectionCode) : nullptr;
	const auto* originalEComSection =
		originalSections != nullptr ? FindNativeSectionSnapshot(*originalSections, kSectionEcDependencies) : nullptr;
	const auto* originalClassPublicitySection =
		originalSections != nullptr ? FindNativeSectionSnapshot(*originalSections, kSectionClassPublicity) : nullptr;
	const auto* originalFolderSection =
		originalSections != nullptr ? FindNativeSectionSnapshot(*originalSections, kSectionFolder) : nullptr;
	const auto* originalLosableSection =
		originalSections != nullptr ? FindNativeSectionSnapshot(*originalSections, kSectionLosable) : nullptr;
	const bool reuseProjectConfigSection =
		currentBundle != nullptr &&
		originalBundle != nullptr &&
		originalProjectConfigSection != nullptr &&
		AreProjectConfigEquivalent(*currentBundle, *originalBundle);
	const bool reuseResourceSection =
		currentBundle != nullptr &&
		originalBundle != nullptr &&
		originalResourceSection != nullptr &&
		AreResourceSectionsEquivalent(*currentBundle, *originalBundle);
	const bool reuseEComSection =
		currentBundle != nullptr &&
		originalBundle != nullptr &&
		originalEComSection != nullptr &&
		AreEComDependenciesEquivalent(*currentBundle, *originalBundle);
	const bool reuseFolderSection =
		currentBundle != nullptr &&
		originalBundle != nullptr &&
		originalFolderSection != nullptr &&
		AreFolderSectionsEquivalent(*currentBundle, *originalBundle);

	std::unordered_map<std::uint32_t, SectionEmitInfo> sectionsToEmit;
	const auto addBuiltSection =
		[&sectionsToEmit](const std::uint32_t key,
			const char* defaultName,
			const std::int32_t defaultFlags,
			const NativeSectionSnapshot* originalSection,
			std::vector<std::uint8_t> data) {
			SectionEmitInfo info;
			info.key = key;
			info.name = originalSection != nullptr ? originalSection->name : std::string(defaultName);
			info.flags = originalSection != nullptr ? originalSection->flags : defaultFlags;
			info.data = std::move(data);
			sectionsToEmit.insert_or_assign(key, std::move(info));
		};
	const auto addRawSection = [&sectionsToEmit](const NativeSectionSnapshot& snapshot) {
		SectionEmitInfo info;
		info.key = snapshot.key;
		info.name = snapshot.name;
		info.flags = snapshot.flags;
		info.data = snapshot.data;
		sectionsToEmit.insert_or_assign(snapshot.key, std::move(info));
	};

	if (originalSystemSection != nullptr) {
		addRawSection(*originalSystemSection);
	}
	else {
		addBuiltSection(kSectionSystemInfo, "系统信息段", 0, nullptr, systemBytes);
	}

	if (reuseProjectConfigSection) {
		addRawSection(*originalProjectConfigSection);
	}
	else {
		addBuiltSection(kSectionProjectConfig, "用户信息段", 1, originalProjectConfigSection, projectConfigBytes);
	}

	if (reuseResourceSection) {
		addRawSection(*originalResourceSection);
	}
	else {
		addBuiltSection(kSectionResource, "程序资源段", 0, originalResourceSection, resourceBytes);
	}

	addBuiltSection(kSectionCode, "程序段", 0, originalCodeSection, codeBytes);

	if (reuseResourceSection && originalEventIndicesSection != nullptr) {
		addRawSection(*originalEventIndicesSection);
	}
	else if (!eventIndicesBytes.empty()) {
		addBuiltSection(kSectionEventIndices, "辅助信息段1", 1, originalEventIndicesSection, eventIndicesBytes);
	}

	if (originalEPackageInfoSection != nullptr) {
		addBuiltSection(kSectionEPackageInfo, "易包信息段1", 1, originalEPackageInfoSection, epackageInfoBytes);
	}

	if (reuseEComSection) {
		addRawSection(*originalEComSection);
	}
	else if (!ecomBytes.empty()) {
		addBuiltSection(kSectionEcDependencies, "易模块记录段", 0, originalEComSection, ecomBytes);
	}

	if (!publicityBytes.empty()) {
		addBuiltSection(kSectionClassPublicity, "辅助信息段2", 1, originalClassPublicitySection, publicityBytes);
	}
	if (folderBytes.size() > sizeof(std::int32_t)) {
		if (reuseFolderSection) {
			addRawSection(*originalFolderSection);
		}
		else {
			addBuiltSection(kSectionFolder, "编辑过滤器信息段", 1, originalFolderSection, folderBytes);
		}
	}
	if (originalLosableSection != nullptr) {
		addRawSection(*originalLosableSection);
	}
	else {
		addBuiltSection(kSectionLosable, "可丢失程序段", 1, nullptr, losableBytes);
	}

	if (originalSections != nullptr) {
		for (const auto& snapshot : *originalSections) {
			if (!IsStandardSerializedSectionKey(snapshot.key)) {
				addRawSection(snapshot);
			}
		}
	}

	constexpr std::array<std::uint32_t, 10> kDefaultSectionOrder = {
		kSectionSystemInfo,
		kSectionProjectConfig,
		kSectionResource,
		kSectionCode,
		kSectionEventIndices,
		kSectionEPackageInfo,
		kSectionEcDependencies,
		kSectionClassPublicity,
		kSectionFolder,
		kSectionLosable,
	};

	std::vector<std::uint32_t> sectionOrder;
	std::unordered_set<std::uint32_t> orderedKeys;
	if (originalSections != nullptr && !originalSections->empty()) {
		for (const auto& snapshot : *originalSections) {
			if (!sectionsToEmit.contains(snapshot.key)) {
				continue;
			}
			if (orderedKeys.insert(snapshot.key).second) {
				sectionOrder.push_back(snapshot.key);
			}
		}

		const auto defaultIndexOf = [&kDefaultSectionOrder](const std::uint32_t key) -> size_t {
			for (size_t index = 0; index < kDefaultSectionOrder.size(); ++index) {
				if (kDefaultSectionOrder[index] == key) {
					return index;
				}
			}
			return static_cast<size_t>(-1);
		};

		for (const auto key : kDefaultSectionOrder) {
			if (!sectionsToEmit.contains(key) || orderedKeys.contains(key)) {
				continue;
			}

			size_t insertPos = sectionOrder.size();
			const size_t currentOrderIndex = defaultIndexOf(key);
			bool foundPosition = false;

			for (size_t probe = currentOrderIndex; probe > 0; --probe) {
				const auto previousKey = kDefaultSectionOrder[probe - 1];
				const auto it = std::find(sectionOrder.begin(), sectionOrder.end(), previousKey);
				if (it != sectionOrder.end()) {
					insertPos = static_cast<size_t>(std::distance(sectionOrder.begin(), it)) + 1;
					foundPosition = true;
					break;
				}
			}
			if (!foundPosition) {
				for (size_t probe = currentOrderIndex + 1; probe < kDefaultSectionOrder.size(); ++probe) {
					const auto nextKey = kDefaultSectionOrder[probe];
					const auto it = std::find(sectionOrder.begin(), sectionOrder.end(), nextKey);
					if (it != sectionOrder.end()) {
						insertPos = static_cast<size_t>(std::distance(sectionOrder.begin(), it));
						foundPosition = true;
						break;
					}
				}
			}

			sectionOrder.insert(sectionOrder.begin() + static_cast<std::ptrdiff_t>(insertPos), key);
			orderedKeys.insert(key);
		}
	}
	else {
		for (const auto key : kDefaultSectionOrder) {
			if (sectionsToEmit.contains(key)) {
				sectionOrder.push_back(key);
				orderedKeys.insert(key);
			}
		}
	}

	for (const auto& [key, _] : sectionsToEmit) {
		if (!orderedKeys.contains(key)) {
			sectionOrder.push_back(key);
		}
	}

	ByteWriter file;
	file.WriteU32(kMagicFileHeader1);
	file.WriteU32(kMagicFileHeader2);
	int sectionIndex = 1;
	for (const auto key : sectionOrder) {
		const auto it = sectionsToEmit.find(key);
		if (it == sectionsToEmit.end()) {
			continue;
		}
		WriteSection(file, it->second.key, it->second.name, it->second.flags, sectionIndex++, it->second.data);
	}
	WriteSection(file, kSectionEndOfFile, "", 0, sectionIndex++, {});
	outBytes = file.TakeBytes();
	return true;
}

}  // namespace

bool Restorer::ParseText(const std::string& inputPath, Document& outDocument, std::string* outError) const
{
	if (outError != nullptr) {
		outError->clear();
	}

	std::vector<std::uint8_t> bytes;
	if (!ReadFileBytes(inputPath, bytes)) {
		if (outError != nullptr) {
			*outError = "read_input_failed";
		}
		return false;
	}

	const std::string text = RemoveUtf8Bom(std::string(reinterpret_cast<const char*>(bytes.data()), bytes.size()));
	const auto lines = SplitLines(text);
	if (lines.empty() || lines.front() != "e2txt Generated Dump") {
		if (outError != nullptr) {
			*outError = "dump_header_invalid";
		}
		return false;
	}

	Document document;
	document.outputPath = inputPath;
	size_t index = 1;
	for (; index < lines.size(); ++index) {
		const std::string& line = lines[index];
		if (line == "================================================================================") {
			break;
		}
		if (line.empty()) {
			continue;
		}

		std::string value;
		if (ParseHeaderValueLine(line, "source", value)) {
			document.sourcePath = value;
		}
		else if (ParseHeaderValueLine(line, "output", value)) {
			document.outputPath = value;
		}
		else if (ParseHeaderValueLine(line, "project", value)) {
			document.projectName = value;
		}
		else if (ParseHeaderValueLine(line, "version", value)) {
			document.versionText = value;
		}
	}

	std::vector<DumpBlock> blocks;
	if (!ParseDumpBlocks(lines, blocks, outError)) {
		return false;
	}

	for (const auto& block : blocks) {
		if (block.kind == DumpBlock::Kind::Dependencies) {
			for (const auto& line : block.lines) {
				const std::string trimmed = TrimAsciiCopy(line);
				if (StartsWith(trimmed, "ELib ")) {
					Dependency dependency;
					dependency.kind = DependencyKind::ELib;
					dependency.name = ExtractNamedSegment(trimmed, "name", std::make_optional(std::string("file")));
					dependency.fileName = ExtractNamedSegment(trimmed, "file", std::make_optional(std::string("guid")));
					dependency.guid = ExtractNamedSegment(trimmed, "guid", std::make_optional(std::string("version")));
					dependency.versionText = ExtractNamedSegment(trimmed, "version", std::nullopt);
					document.dependencies.push_back(std::move(dependency));
				}
				else if (StartsWith(trimmed, "ECom ")) {
					Dependency dependency;
					dependency.kind = DependencyKind::ECom;
					dependency.name = ExtractNamedSegment(trimmed, "name", std::make_optional(std::string("path")));
					dependency.path = ExtractNamedSegment(trimmed, "path", std::make_optional(std::string("re_export")));
					const std::string reExportText = ExtractNamedSegment(trimmed, "re_export", std::nullopt);
					dependency.reExport = reExportText == "true" || reExportText == "1";
					document.dependencies.push_back(std::move(dependency));
				}
			}
			continue;
		}

		if (block.kind == DumpBlock::Kind::Page) {
			Page page;
			page.typeName = block.pageType;
			page.name = block.name;
			page.lines = block.lines;
			document.pages.push_back(std::move(page));
			continue;
		}

		FormXml formXml;
		formXml.name = block.name;
		formXml.lines = block.lines;
		document.formXmls.push_back(std::move(formXml));
	}

	outDocument = std::move(document);
	return true;
}

bool Restorer::RestoreToBytes(const Document& document, std::vector<std::uint8_t>& outBytes, std::string* outError) const
{
	if (outError != nullptr) {
		outError->clear();
	}

	RestoreDocumentModel model;
	if (!BuildRestoreModel(document, nullptr, model, outError)) {
		return false;
	}
	return SerializeToModuleBytes(model, outBytes, outError);
}

bool Restorer::RestoreToFile(
	const std::string& inputPath,
	const std::string& outputPath,
	std::string* outSummary,
	std::string* outError) const
{
	if (outError != nullptr) {
		outError->clear();
	}
	if (outSummary != nullptr) {
		outSummary->clear();
	}

	Document document;
	if (!ParseText(inputPath, document, outError)) {
		return false;
	}

	std::vector<std::uint8_t> bytes;
	if (!RestoreToBytes(document, bytes, outError)) {
		return false;
	}

	std::ofstream out(Utf8PathToPath(outputPath), std::ios::binary);
	if (!out.is_open()) {
		if (outError != nullptr) {
			*outError = "open_output_failed";
		}
		return false;
	}
	out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
	if (!out.good()) {
		if (outError != nullptr) {
			*outError = "write_output_failed";
		}
		return false;
	}

	if (outSummary != nullptr) {
		*outSummary = "bytes=" + std::to_string(bytes.size()) + ", output=" + outputPath;
	}
	return true;
}

bool Restorer::RestoreBundleToBytes(const ProjectBundle& bundle, std::vector<std::uint8_t>& outBytes, std::string* outError) const
{
	if (outError != nullptr) {
		outError->clear();
	}

	if (CanReuseNativeBundleSnapshot(bundle)) {
		outBytes = bundle.nativeSourceBytes;
		return true;
	}

	Document document;
	try {
		document = BuildDocumentFromBundle(bundle);
	}
	catch (const std::exception& ex) {
		if (outError != nullptr) {
			*outError = std::string("build_document_exception: ") + ex.what();
		}
		return false;
	}
	if (CanReuseNativeBytesForSemanticEquivalentSources(bundle, document)) {
		outBytes = bundle.nativeSourceBytes;
		return true;
	}

	ProjectBundle originalBundle;
	ProjectBundle* originalBundlePtr = nullptr;
	if (!bundle.nativeSourceBytes.empty()) {
		std::string ignoredError;
		Generator generator;
		if (generator.GenerateBundleFromBytes(bundle.nativeSourceBytes, bundle.sourcePath, originalBundle, &ignoredError)) {
			originalBundlePtr = &originalBundle;
		}
	}

	RestoreDocumentModel model;
	try {
		if (!BuildRestoreModel(document, &bundle, model, outError, originalBundlePtr)) {
			return false;
		}
	}
	catch (const std::exception& ex) {
		if (outError != nullptr) {
			*outError = std::string("build_restore_model_exception: ") + ex.what();
		}
		return false;
	}

	std::vector<NativeSectionSnapshot> originalSections;
	std::vector<NativeSectionSnapshot>* originalSectionsPtr = nullptr;
	if (!bundle.nativeSourceBytes.empty()) {
		std::string ignoredError;
		if (CaptureNativeSectionSnapshots(bundle.nativeSourceBytes, originalSections, &ignoredError)) {
			originalSectionsPtr = &originalSections;
		}
	}

	try {
		return SerializeToModuleBytes(model, outBytes, outError, &bundle, originalBundlePtr, originalSectionsPtr);
	}
	catch (const std::exception& ex) {
		if (outError != nullptr) {
			*outError = std::string("serialize_exception: ") + ex.what();
		}
		return false;
	}
}

bool Restorer::RestoreBundleToFile(
	const ProjectBundle& bundle,
	const std::string& outputPath,
	std::string* outSummary,
	std::string* outError) const
{
	if (outError != nullptr) {
		outError->clear();
	}
	if (outSummary != nullptr) {
		outSummary->clear();
	}

	std::vector<std::uint8_t> bytes;
	if (!RestoreBundleToBytes(bundle, bytes, outError)) {
		return false;
	}

	std::ofstream out(Utf8PathToPath(outputPath), std::ios::binary);
	if (!out.is_open()) {
		if (outError != nullptr) {
			*outError = "open_output_failed";
		}
		return false;
	}
	out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
	if (!out.good()) {
		if (outError != nullptr) {
			*outError = "write_output_failed";
		}
		return false;
	}

	if (outSummary != nullptr) {
		*outSummary = "bytes=" + std::to_string(bytes.size()) + ", output=" + outputPath;
	}
	return true;
}

}  // namespace e2txt
