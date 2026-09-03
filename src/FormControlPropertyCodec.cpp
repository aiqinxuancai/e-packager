#include "FormControlPropertyCodec.h"

// 通过 lib2.h 的公开窗口单元接口编解码核心及第三方控件属性。
#include <Windows.h>

#include <algorithm>
#include <array>
#include <charconv>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <iomanip>
#include <iostream>
#include <limits>
#include <sstream>
#include <string_view>

#include <intrin.h>

#include <lib2.h>

#include "PathHelper.h"

namespace e2txt {
namespace {

constexpr std::size_t kFixedPropertyCount = FIXED_WIN_UNIT_PROPERTY_COUNT;
constexpr std::size_t kMaxPropertyDataSize = 64u * 1024u * 1024u;
constexpr std::size_t kMaxPublishedStringLength = 4096;

// Older core FNEs expose these design-time text editors without publishing
// their symbolic type constants through lib2.h.  They are still part of the
// stable window-property ABI and must be decoded as text rather than as the
// integer arm of UNIT_PROPERTY_VALUE.
constexpr std::int16_t kInternalTextProperty = 1018;
constexpr std::int16_t kInternalProviderProperty = 1019;
constexpr std::int16_t kInternalColumnProperty = 1020;
constexpr std::int16_t kInternalConnectProperty = 1021;
constexpr std::int16_t kInternalSqlProperty = 1022;

constexpr std::array<const char*, kFixedPropertyCount> kFixedPropertyNames = {
	"左边",
	"顶边",
	"宽度",
	"高度",
	"标记",
	"可视",
	"禁止",
	"鼠标指针",
};

constexpr std::array<std::int16_t, kFixedPropertyCount> kFixedPropertyTypes = {
	UD_INT,
	UD_INT,
	UD_INT,
	UD_INT,
	UD_TEXT,
	UD_BOOL,
	UD_BOOL,
	UD_CURSOR,
};

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

bool IsReadableMemoryRange(const void* address, const std::size_t size)
{
	if (address == nullptr) {
		return false;
	}
	if (size == 0) {
		return true;
	}

	const auto* current = static_cast<const std::uint8_t*>(address);
	std::size_t remaining = size;
	while (remaining > 0) {
		MEMORY_BASIC_INFORMATION mbi = {};
		if (VirtualQuery(current, &mbi, sizeof(mbi)) != sizeof(mbi) ||
			mbi.State != MEM_COMMIT || !IsReadablePageProtection(mbi.Protect)) {
			return false;
		}

		const auto* regionBase = static_cast<const std::uint8_t*>(mbi.BaseAddress);
		const std::size_t offset = static_cast<std::size_t>(current - regionBase);
		if (offset >= mbi.RegionSize) {
			return false;
		}

		const std::size_t available = mbi.RegionSize - offset;
		if (available >= remaining) {
			return true;
		}

		current += available;
		remaining -= available;
	}
	return true;
}

std::size_t GetSafeCStringLength(const char* text, const std::size_t maxLength)
{
	if (text == nullptr) {
		return 0;
	}

	std::size_t length = 0;
#if defined(_MSC_VER)
	__try {
		for (; length < maxLength; ++length) {
			if (text[length] == '\0') {
				break;
			}
		}
	}
	__except (EXCEPTION_EXECUTE_HANDLER) {
		return static_cast<std::size_t>(-1);
	}
#else
	for (; length < maxLength; ++length) {
		if (text[length] == '\0') {
			break;
		}
	}
#endif
	return length;
}

bool TryReadCString(const char* text, std::string& out)
{
	out.clear();
	if (text == nullptr) {
		return true;
	}
	const std::size_t length = GetSafeCStringLength(text, kMaxPublishedStringLength);
	if (length == static_cast<std::size_t>(-1) || length >= kMaxPublishedStringLength) {
		return false;
	}
	if (length > 0 && !IsReadableMemoryRange(text, length)) {
		return false;
	}

#if defined(_MSC_VER)
	__try {
		out.assign(text, length);
	}
	__except (EXCEPTION_EXECUTE_HANDLER) {
		out.clear();
		return false;
	}
#else
	out.assign(text, length);
#endif
	return true;
}

bool IsValidUtf8(const std::string& text)
{
	for (std::size_t index = 0; index < text.size();) {
		const unsigned char lead = static_cast<unsigned char>(text[index]);
		if (lead < 0x80) {
			++index;
			continue;
		}

		std::size_t continuationCount = 0;
		std::uint32_t codePoint = 0;
		std::uint32_t minimum = 0;
		if (lead >= 0xC2 && lead <= 0xDF) {
			continuationCount = 1;
			codePoint = lead & 0x1Fu;
			minimum = 0x80;
		}
		else if (lead >= 0xE0 && lead <= 0xEF) {
			continuationCount = 2;
			codePoint = lead & 0x0Fu;
			minimum = 0x800;
		}
		else if (lead >= 0xF0 && lead <= 0xF4) {
			continuationCount = 3;
			codePoint = lead & 0x07u;
			minimum = 0x10000;
		}
		else {
			return false;
		}

		if (index + continuationCount >= text.size()) {
			return false;
		}
		for (std::size_t offset = 1; offset <= continuationCount; ++offset) {
			const unsigned char byte = static_cast<unsigned char>(text[index + offset]);
			if ((byte & 0xC0u) != 0x80u) {
				return false;
			}
			codePoint = (codePoint << 6) | (byte & 0x3Fu);
		}
		if (codePoint < minimum || codePoint > 0x10FFFFu ||
			(codePoint >= 0xD800u && codePoint <= 0xDFFFu)) {
			return false;
		}
		index += continuationCount + 1;
	}
	return true;
}

bool TryConvertUtf8ToLocal(const std::string& text, std::string& out)
{
	if (text.empty()) {
		out.clear();
		return true;
	}
	const int wideLength = MultiByteToWideChar(
		CP_UTF8,
		MB_ERR_INVALID_CHARS,
		text.data(),
		static_cast<int>(text.size()),
		nullptr,
		0);
	if (wideLength <= 0) {
		return false;
	}
	std::wstring wide(static_cast<std::size_t>(wideLength), L'\0');
	if (MultiByteToWideChar(
			CP_UTF8,
			MB_ERR_INVALID_CHARS,
			text.data(),
			static_cast<int>(text.size()),
			wide.data(),
			wideLength) <= 0) {
		return false;
	}
	const int localLength = WideCharToMultiByte(
		CP_ACP,
		0,
		wide.data(),
		wideLength,
		nullptr,
		0,
		nullptr,
		nullptr);
	if (localLength <= 0) {
		return false;
	}
	out.assign(static_cast<std::size_t>(localLength), '\0');
	return WideCharToMultiByte(
		CP_ACP,
		0,
		wide.data(),
		wideLength,
		out.data(),
		localLength,
		nullptr,
		nullptr) > 0;
}

bool TryConvertLocalToUtf8(const std::string& text, std::string& out)
{
	if (text.empty()) {
		out.clear();
		return true;
	}
	const int wideLength = MultiByteToWideChar(
		CP_ACP,
		0,
		text.data(),
		static_cast<int>(text.size()),
		nullptr,
		0);
	if (wideLength <= 0) {
		return false;
	}
	std::wstring wide(static_cast<std::size_t>(wideLength), L'\0');
	if (MultiByteToWideChar(
			CP_ACP,
			0,
			text.data(),
			static_cast<int>(text.size()),
			wide.data(),
			wideLength) <= 0) {
		return false;
	}
	const int utf8Length = WideCharToMultiByte(
		CP_UTF8,
		0,
		wide.data(),
		wideLength,
		nullptr,
		0,
		nullptr,
		nullptr);
	if (utf8Length <= 0) {
		return false;
	}
	out.assign(static_cast<std::size_t>(utf8Length), '\0');
	return WideCharToMultiByte(
		CP_UTF8,
		0,
		wide.data(),
		wideLength,
		out.data(),
		utf8Length,
		nullptr,
		nullptr) > 0;
}

bool DetectUtf8LibraryStrings(const LIB_INFO* info)
{
	if (info == nullptr) {
		return false;
	}
	unsigned int validFieldCount = 0;
	for (const char* field : { info->m_szName, info->m_szExplain, info->m_szAuthor }) {
		std::string raw;
		if (!TryReadCString(field, raw) || raw.empty() ||
			!std::any_of(raw.begin(), raw.end(), [](const unsigned char value) { return value >= 0x80; }) ||
			!IsValidUtf8(raw)) {
			continue;
		}
		++validFieldCount;
	}
	return validFieldCount >= 2;
}

std::string ReadPublishedString(const char* text, const bool utf8)
{
	std::string raw;
	if (!TryReadCString(text, raw)) {
		return std::string();
	}
	if (!utf8 || !IsValidUtf8(raw)) {
		return raw;
	}
	std::string local;
	return TryConvertUtf8ToLocal(raw, local) ? local : raw;
}

bool CallGetNewInfoSafely(const PFN_GET_LIB_INFO procedure, const LIB_INFO*& outInfo)
{
	outInfo = nullptr;
	if (procedure == nullptr) {
		return false;
	}
#if defined(_MSC_VER)
	__try {
		outInfo = procedure();
	}
	__except (EXCEPTION_EXECUTE_HANDLER) {
		outInfo = nullptr;
	}
#else
	outInfo = procedure();
#endif
	return outInfo != nullptr;
}

PFN_INTERFACE CallGetInterfaceSafely(const PFN_GET_INTERFACE procedure, const int interfaceNumber)
{
	if (procedure == nullptr) {
		return nullptr;
	}
#if defined(_MSC_VER)
	__try {
		return procedure(interfaceNumber);
	}
	__except (EXCEPTION_EXECUTE_HANDLER) {
		return nullptr;
	}
#else
	return procedure(interfaceNumber);
#endif
}

INT_PTR CallNotifyLibrarySafely(
	const PFN_NOTIFY_LIB procedure,
	const INT message,
	const DWORD_PTR param1 = 0,
	const DWORD_PTR param2 = 0)
{
	if (procedure == nullptr) {
		return NR_ERR;
	}
#if defined(_MSC_VER)
	__try {
		return procedure(message, param1, param2);
	}
	__except (EXCEPTION_EXECUTE_HANDLER) {
		return NR_ERR;
	}
#else
	return procedure(message, param1, param2);
#endif
}

HUNIT CallCreateUnitSafely(
	const PFN_CREATE_UNIT procedure,
	LPBYTE data,
	const INT dataSize,
	const HWND parent,
	const DWORD formId,
	const DWORD unitId)
{
	if (procedure == nullptr || parent == nullptr) {
		return 0;
	}
#if defined(_MSC_VER)
	__try {
		return procedure(
			data,
			dataSize,
			0,
			parent,
			100,
			nullptr,
			10,
			20,
			300,
			100,
			formId,
			unitId,
			parent,
			TRUE);
	}
	__except (EXCEPTION_EXECUTE_HANDLER) {
		return 0;
	}
#else
	return procedure(data, dataSize, 0, parent, 100, nullptr, 10, 20, 300, 100, formId, unitId, parent, TRUE);
#endif
}

bool CallGetPropertyDataSafely(
	const PFN_GET_PROPERTY_DATA procedure,
	const HUNIT unit,
	const int callbackIndex,
	UNIT_PROPERTY_VALUE& value)
{
	if (procedure == nullptr || unit == 0) {
		return false;
	}
#if defined(_MSC_VER)
	__try {
		return procedure(unit, callbackIndex, &value) != FALSE;
	}
	__except (EXCEPTION_EXECUTE_HANDLER) {
		return false;
	}
#else
	return procedure(unit, callbackIndex, &value) != FALSE;
#endif
}

void FreePropertyValueDataSafely(
	const PFN_NOTIFY_LIB notifyLibrary,
	const void* data)
{
	if (notifyLibrary == nullptr || data == nullptr) {
		return;
	}
#if defined(_MSC_VER)
	__try {
		(void)notifyLibrary(NRS_MFREE, reinterpret_cast<DWORD_PTR>(data), 0);
	}
	__except (EXCEPTION_EXECUTE_HANDLER) {
		// 支持库违反返回值契约时，不能让属性探针影响解包流程。
	}
#else
	(void)notifyLibrary(NRS_MFREE, reinterpret_cast<DWORD_PTR>(data), 0);
#endif
}

bool CallNotifyPropertyChangedSafely(
	const PFN_NOTIFY_PROPERTY_CHANGED procedure,
	const HUNIT unit,
	const int callbackIndex,
	UNIT_PROPERTY_VALUE& value,
	bool& outNeedsRecreate)
{
	outNeedsRecreate = false;
	if (procedure == nullptr || unit == 0) {
		return false;
	}
#if defined(_MSC_VER)
	__try {
		outNeedsRecreate = procedure(unit, callbackIndex, &value, nullptr) != FALSE;
		return true;
	}
	__except (EXCEPTION_EXECUTE_HANDLER) {
		return false;
	}
#else
	outNeedsRecreate = procedure(unit, callbackIndex, &value, nullptr) != FALSE;
	return true;
#endif
}

HGLOBAL CallGetAllPropertyDataSafely(const PFN_GET_ALL_PROPERTY_DATA procedure, const HUNIT unit)
{
	if (procedure == nullptr || unit == 0) {
		return nullptr;
	}
#if defined(_MSC_VER)
	__try {
		return procedure(unit);
	}
	__except (EXCEPTION_EXECUTE_HANDLER) {
		return nullptr;
	}
#else
	return procedure(unit);
#endif
}

bool CopyGlobalBytes(const HGLOBAL globalMemory, std::vector<std::uint8_t>& out)
{
	out.clear();
	if (globalMemory == nullptr) {
		return false;
	}
	const SIZE_T size = GlobalSize(globalMemory);
	if (size > kMaxPropertyDataSize) {
		return false;
	}
	if (size == 0) {
		return true;
	}
	const auto* data = static_cast<const std::uint8_t*>(GlobalLock(globalMemory));
	if (data == nullptr) {
		return false;
	}
	if (!IsReadableMemoryRange(data, static_cast<std::size_t>(size))) {
		GlobalUnlock(globalMemory);
		return false;
	}
	out.assign(data, data + static_cast<std::size_t>(size));
	GlobalUnlock(globalMemory);
	return true;
}

bool CopyDirectBytes(const std::uint8_t* data, const std::int32_t size, std::vector<std::uint8_t>& out)
{
	out.clear();
	if (size < 0 || static_cast<std::size_t>(size) > kMaxPropertyDataSize) {
		return false;
	}
	if (size == 0) {
		return true;
	}
	if (data == nullptr || !IsReadableMemoryRange(data, static_cast<std::size_t>(size))) {
		return false;
	}
	out.assign(data, data + size);
	return true;
}

bool IsTextPropertyType(const std::int16_t type)
{
	return type == UD_TEXT || type == UD_PICK_TEXT || type == UD_EDIT_PICK_TEXT ||
		type == UD_FILE_NAME || type == kInternalTextProperty ||
		type == kInternalProviderProperty || type == kInternalColumnProperty ||
		type == kInternalConnectProperty || type == kInternalSqlProperty;
}

bool IsBinaryPropertyType(const std::int16_t type)
{
	return type == UD_PIC || type == UD_ICON || type == UD_CURSOR || type == UD_MUSIC ||
		type == UD_FONT || type == UD_CUSTOMIZE || type == UD_IMAGE_LIST;
}

bool IsDoublePropertyType(const std::int16_t type)
{
	return type == UD_DOUBLE || type == UD_DATE_TIME;
}

bool IsBooleanPropertyType(const std::int16_t type)
{
	return type == UD_BOOL;
}

bool TryReadUnknownTextValue(
	const UNIT_PROPERTY_VALUE& raw,
	const bool utf8,
	std::string& outText)
{
	// A property type not defined by lib2.h may still be a support-library
	// text editor type.  The public getter exposes only UNIT_PROPERTY_VALUE,
	// so identify the pointer representation conservatively: a non-null,
	// readable C string is a valid text value; integer values normally fail the
	// readable-range check immediately.
	if (raw.m_szText == nullptr || raw.m_data.m_nDataSize > 0) return false;
	std::string rawText;
	if (!TryReadCString(raw.m_szText, rawText)) return false;
	if (!utf8) {
		outText = std::move(rawText);
		return true;
	}
	return TryConvertUtf8ToLocal(rawText, outText);
}

bool TryReadUnknownBinaryValue(
	const UNIT_PROPERTY_VALUE& raw,
	std::vector<std::uint8_t>& outBytes)
{
	if (raw.m_data.m_nDataSize <= 0 || raw.m_data.m_nDataSize > static_cast<INT>(kMaxPropertyDataSize) ||
		raw.m_data.m_pData == nullptr) return false;
	return CopyDirectBytes(raw.m_data.m_pData, raw.m_data.m_nDataSize, outBytes);
}

std::string TrimAscii(std::string value)
{
	while (!value.empty() && std::isspace(static_cast<unsigned char>(value.front())) != 0) {
		value.erase(value.begin());
	}
	while (!value.empty() && std::isspace(static_cast<unsigned char>(value.back())) != 0) {
		value.pop_back();
	}
	return value;
}

bool TryParseInt32(const std::string& raw, std::int32_t& out)
{
	const std::string text = TrimAscii(raw);
	if (text.empty()) {
		return false;
	}
	const char* begin = text.data();
	const char* end = begin + text.size();
	const auto result = std::from_chars(begin, end, out, 10);
	return result.ec == std::errc() && result.ptr == end;
}

bool TryParseDouble(const std::string& raw, double& out)
{
	const std::string text = TrimAscii(raw);
	if (text.empty()) {
		return false;
	}
	char* end = nullptr;
	out = std::strtod(text.c_str(), &end);
	return end != text.c_str() && *end == '\0' && std::isfinite(out);
}

bool TryParseBoolean(const std::string& raw, bool& out)
{
	const std::string text = TrimAscii(raw);
	if (text == "真" || text == "true" || text == "TRUE" || text == "1") {
		out = true;
		return true;
	}
	if (text == "假" || text == "false" || text == "FALSE" || text == "0") {
		out = false;
		return true;
	}
	return false;
}

constexpr char kBase64Table[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

std::string EncodeBase64(const std::vector<std::uint8_t>& data)
{
	std::string out;
	out.reserve(((data.size() + 2) / 3) * 4);
	std::size_t index = 0;
	while (index + 3 <= data.size()) {
		const std::uint32_t value =
			(static_cast<std::uint32_t>(data[index]) << 16) |
			(static_cast<std::uint32_t>(data[index + 1]) << 8) |
			static_cast<std::uint32_t>(data[index + 2]);
		out.push_back(kBase64Table[(value >> 18) & 0x3F]);
		out.push_back(kBase64Table[(value >> 12) & 0x3F]);
		out.push_back(kBase64Table[(value >> 6) & 0x3F]);
		out.push_back(kBase64Table[value & 0x3F]);
		index += 3;
	}
	const std::size_t remaining = data.size() - index;
	if (remaining == 1) {
		const std::uint32_t value = static_cast<std::uint32_t>(data[index]) << 16;
		out.push_back(kBase64Table[(value >> 18) & 0x3F]);
		out.push_back(kBase64Table[(value >> 12) & 0x3F]);
		out.append("==");
	}
	else if (remaining == 2) {
		const std::uint32_t value =
			(static_cast<std::uint32_t>(data[index]) << 16) |
			(static_cast<std::uint32_t>(data[index + 1]) << 8);
		out.push_back(kBase64Table[(value >> 18) & 0x3F]);
		out.push_back(kBase64Table[(value >> 12) & 0x3F]);
		out.push_back(kBase64Table[(value >> 6) & 0x3F]);
		out.push_back('=');
	}
	return out;
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

bool DecodeBase64(const std::string& text, std::vector<std::uint8_t>& out)
{
	out.clear();
	if (text.empty()) {
		return true;
	}
	if ((text.size() % 4) != 0) {
		return false;
	}
	for (std::size_t index = 0; index < text.size(); index += 4) {
		const int a = Base64Value(static_cast<unsigned char>(text[index]));
		const int b = Base64Value(static_cast<unsigned char>(text[index + 1]));
		if (a < 0 || b < 0) {
			return false;
		}
		const unsigned char cChar = static_cast<unsigned char>(text[index + 2]);
		const unsigned char dChar = static_cast<unsigned char>(text[index + 3]);
		const bool cPadding = cChar == '=';
		const bool dPadding = dChar == '=';
		const int c = cPadding ? 0 : Base64Value(cChar);
		const int d = dPadding ? 0 : Base64Value(dChar);
		if (c < 0 || d < 0 || (cPadding && !dPadding) ||
			((cPadding || dPadding) && index + 4 != text.size())) {
			return false;
		}

		const std::uint32_t value =
			(static_cast<std::uint32_t>(a) << 18) |
			(static_cast<std::uint32_t>(b) << 12) |
			(static_cast<std::uint32_t>(c) << 6) |
			static_cast<std::uint32_t>(d);
		out.push_back(static_cast<std::uint8_t>((value >> 16) & 0xFF));
		if (!cPadding) out.push_back(static_cast<std::uint8_t>((value >> 8) & 0xFF));
		if (!dPadding) out.push_back(static_cast<std::uint8_t>(value & 0xFF));
	}
	return true;
}

const std::uint8_t* FindByteSequence(
	const std::uint8_t* data,
	const std::size_t dataSize,
	const char* sequence,
	const std::size_t sequenceSize)
{
	if (data == nullptr || sequence == nullptr || sequenceSize == 0 || dataSize < sequenceSize) {
		return nullptr;
	}
	for (std::size_t offset = 0; offset + sequenceSize <= dataSize; ++offset) {
		if (std::memcmp(data + offset, sequence, sequenceSize) == 0) {
			return data + offset;
		}
	}
	return nullptr;
}

bool IsExecutableAddress(const void* address)
{
	if (address == nullptr) {
		return false;
	}
	MEMORY_BASIC_INFORMATION mbi = {};
	if (VirtualQuery(address, &mbi, sizeof(mbi)) != sizeof(mbi) || mbi.State != MEM_COMMIT) {
		return false;
	}
	switch (mbi.Protect & 0xFFu) {
	case PAGE_EXECUTE:
	case PAGE_EXECUTE_READ:
	case PAGE_EXECUTE_READWRITE:
	case PAGE_EXECUTE_WRITECOPY:
		return (mbi.Protect & PAGE_GUARD) == 0;
	default:
		return false;
	}
}

bool TryGetModuleImage(
	const HMODULE module,
	const std::uint8_t*& outImage,
	std::size_t& outImageSize)
{
	outImage = nullptr;
	outImageSize = 0;
	if (module == nullptr) {
		return false;
	}
	const auto* image = reinterpret_cast<const std::uint8_t*>(module);
	const auto* dosHeader = reinterpret_cast<const IMAGE_DOS_HEADER*>(image);
	if (dosHeader->e_magic != IMAGE_DOS_SIGNATURE || dosHeader->e_lfanew <= 0 || dosHeader->e_lfanew > 0x1000000) {
		return false;
	}
#if defined(_WIN64)
	const auto* ntHeader = reinterpret_cast<const IMAGE_NT_HEADERS64*>(image + dosHeader->e_lfanew);
	if (ntHeader->Signature != IMAGE_NT_SIGNATURE || ntHeader->OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC) {
		return false;
	}
	outImageSize = ntHeader->OptionalHeader.SizeOfImage;
#else
	const auto* ntHeader = reinterpret_cast<const IMAGE_NT_HEADERS32*>(image + dosHeader->e_lfanew);
	if (ntHeader->Signature != IMAGE_NT_SIGNATURE || ntHeader->OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR32_MAGIC) {
		return false;
	}
	outImageSize = ntHeader->OptionalHeader.SizeOfImage;
#endif
	if (outImageSize == 0 || outImageSize > 512u * 1024u * 1024u) {
		return false;
	}
	outImage = image;
	return true;
}

void** FindCWndVtable(const HMODULE module)
{
	const std::uint8_t* image = nullptr;
	std::size_t imageSize = 0;
	if (!TryGetModuleImage(module, image, imageSize)) {
		return nullptr;
	}

	// 静态 MFC 不导出 CWnd 工厂，使用其 RTTI 找到同一 DLL 的虚表。
	static constexpr char kCWndTypeName[] = ".?AVCWnd@@";
	const auto* typeName = FindByteSequence(
		image,
		imageSize,
		kCWndTypeName,
		sizeof(kCWndTypeName) - 1);
	const std::size_t typeDescriptorPrefixSize = sizeof(void*) == 8 ? 16u : 8u;
	if (typeName == nullptr || typeName < image + typeDescriptorPrefixSize) {
		return nullptr;
	}
	const auto* typeDescriptor = typeName - typeDescriptorPrefixSize;
	const auto moduleAddress = reinterpret_cast<std::uintptr_t>(module);
	const auto typeDescriptorAddress = reinterpret_cast<std::uintptr_t>(typeDescriptor);
	const auto typeDescriptorOffset = static_cast<std::size_t>(typeDescriptor - image);

	std::uintptr_t completeObjectLocatorAddress = 0;
	std::size_t completeObjectLocatorOffset = 0;
	for (std::size_t fieldOffset = 12; fieldOffset + sizeof(std::uint32_t) <= imageSize; fieldOffset += sizeof(std::uint32_t)) {
		const auto fieldValue = *reinterpret_cast<const std::uint32_t*>(image + fieldOffset);
#if defined(_WIN64)
		if (fieldValue != static_cast<std::uint32_t>(typeDescriptorOffset)) {
			continue;
		}
#else
		if (fieldValue != static_cast<std::uint32_t>(typeDescriptorAddress)) {
			continue;
		}
#endif
		const std::size_t locatorOffset = fieldOffset - 12;
#if defined(_WIN64)
		constexpr std::size_t kLocatorSize = 24;
#else
		constexpr std::size_t kLocatorSize = 20;
#endif
		if (locatorOffset + kLocatorSize > imageSize ||
			*reinterpret_cast<const std::uint32_t*>(image + locatorOffset) != 0) {
			continue;
		}
		const auto classDescriptorValue = *reinterpret_cast<const std::uint32_t*>(image + fieldOffset + 4);
#if defined(_WIN64)
		if (classDescriptorValue >= imageSize) {
			continue;
		}
		const std::size_t classDescriptorOffset = classDescriptorValue;
#else
		if (classDescriptorValue < moduleAddress ||
			classDescriptorValue >= moduleAddress + imageSize) {
			continue;
		}
		const std::size_t classDescriptorOffset =
			static_cast<std::size_t>(classDescriptorValue - moduleAddress);
#endif
		if (classDescriptorOffset + 16 > imageSize) {
			continue;
		}
		const auto baseClassCount = *reinterpret_cast<const std::uint32_t*>(image + classDescriptorOffset + 8);
		const auto baseClassArray = *reinterpret_cast<const std::uint32_t*>(image + classDescriptorOffset + 12);
		if (baseClassCount == 0 || baseClassCount > 1024 ||
#if defined(_WIN64)
			baseClassArray >= imageSize || baseClassArray + baseClassCount * sizeof(std::uint32_t) > imageSize) {
#else
			baseClassArray < moduleAddress ||
			baseClassArray >= moduleAddress + imageSize ||
			baseClassArray - moduleAddress + baseClassCount * sizeof(std::uint32_t) > imageSize) {
#endif
			continue;
		}
		completeObjectLocatorOffset = locatorOffset;
		completeObjectLocatorAddress = moduleAddress + locatorOffset;
		break;
	}
	if (completeObjectLocatorAddress == 0) {
		return nullptr;
	}

	const std::size_t pointerSize = sizeof(void*);
	for (std::size_t slotOffset = pointerSize; slotOffset + pointerSize * 8 <= imageSize; slotOffset += pointerSize) {
		const auto slotValue = *reinterpret_cast<const std::uintptr_t*>(image + slotOffset - pointerSize);
		const auto expectedLocator = sizeof(void*) == 8
			? static_cast<std::uintptr_t>(completeObjectLocatorOffset)
			: completeObjectLocatorAddress;
		if (slotValue != expectedLocator) {
			continue;
		}
		bool validVtable = true;
		for (std::size_t slot = 0; slot < 8; ++slot) {
			const auto functionAddress = *reinterpret_cast<const void* const*>(image + slotOffset + slot * pointerSize);
			if (!IsExecutableAddress(functionAddress)) {
				validVtable = false;
				break;
			}
		}
		if (validVtable) {
			return reinterpret_cast<void**>(const_cast<std::uint8_t*>(image + slotOffset));
		}
	}
	return nullptr;
}

// 老版静态 MFC 的 CWnd 布局：controls_w.fne.dll 的 x86 m_hWnd 位于 +0x1c，
// 对应的 x64 布局位于 +0x38；这里只访问第三方控件公开接口实际需要的字段。
constexpr std::size_t kCWndHwndOffset = sizeof(void*) == 8 ? 56u : 28u;
// 图形按钮的创建路径会调用 CWnd 的较深虚函数，尾部需要保留足够的
// CWnd 状态空间；128 字节与独立 FNE 探针使用的对象大小一致。
constexpr std::size_t kCWndProbeObjectSize = 128u;
constexpr DWORD kPropertyProbeFormId = 1u;

void* CreateHostCWndObject(const HWND window, void** vtable)
{
	if (window == nullptr || !IsWindow(window)) {
		return nullptr;
	}
	if (vtable == nullptr) {
		return nullptr;
	}
	auto* object = static_cast<std::uint8_t*>(HeapAlloc(
		GetProcessHeap(),
		HEAP_ZERO_MEMORY,
		kCWndProbeObjectSize));
	if (object == nullptr) {
		return nullptr;
	}
	*reinterpret_cast<void***>(object) = vtable;
	std::memcpy(object + kCWndHwndOffset, &window, sizeof(window));
	return object;
}

HWND GetHostCWndWindow(const void* object)
{
	if (object == nullptr) {
		return nullptr;
	}
	HWND window = nullptr;
	std::memcpy(&window, static_cast<const std::uint8_t*>(object) + kCWndHwndOffset, sizeof(window));
	return window;
}

void SetHostCWndWindow(void* object, const HWND window)
{
	if (object != nullptr) {
		std::memcpy(static_cast<std::uint8_t*>(object) + kCWndHwndOffset, &window, sizeof(window));
	}
}

__declspec(noinline) INT_PTR WINAPI HostSystemNotify(
	const INT message,
	const DWORD_PTR param1,
	const DWORD_PTR param2)
{
	switch (message) {
	case NAS_CREATE_CWND_OBJECT_FROM_HWND: {
		HMODULE callerModule = nullptr;
		MEMORY_BASIC_INFORMATION mbi = {};
		const auto returnAddress = _ReturnAddress();
		if (returnAddress != nullptr && VirtualQuery(returnAddress, &mbi, sizeof(mbi)) == sizeof(mbi)) {
			callerModule = static_cast<HMODULE>(mbi.AllocationBase);
		}
		return reinterpret_cast<INT_PTR>(CreateHostCWndObject(
			reinterpret_cast<HWND>(param1),
			FindCWndVtable(callerModule)));
	}
	case NAS_DELETE_CWND_OBJECT:
		if (param1 != 0) {
			HeapFree(GetProcessHeap(), 0, reinterpret_cast<void*>(param1));
			return 1;
		}
		return 0;
	case NAS_DETACH_CWND_OBJECT: {
		const HWND window = GetHostCWndWindow(reinterpret_cast<const void*>(param1));
		SetHostCWndWindow(reinterpret_cast<void*>(param1), nullptr);
		return reinterpret_cast<INT_PTR>(window);
	}
	case NAS_GET_HWND_OF_CWND_OBJECT:
		return reinterpret_cast<INT_PTR>(GetHostCWndWindow(reinterpret_cast<const void*>(param1)));
	case NAS_ATTACH_CWND_OBJECT:
		if (param2 == 0 || param1 == 0) {
			return 0;
		}
		SetHostCWndWindow(reinterpret_cast<void*>(param2), reinterpret_cast<HWND>(param1));
		return 1;
	case NAS_IS_EWIN:
		return 0;
	case NRS_MALLOC:
		return reinterpret_cast<INT_PTR>(HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, static_cast<SIZE_T>(param1)));
	case NRS_MFREE:
		if (param1 != 0) {
			HeapFree(GetProcessHeap(), 0, reinterpret_cast<void*>(param1));
		}
		return 0;
	case NRS_MREALLOC:
		if (param1 == 0) {
			return reinterpret_cast<INT_PTR>(HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, static_cast<SIZE_T>(param2)));
		}
		return reinterpret_cast<INT_PTR>(HeapReAlloc(
			GetProcessHeap(),
			HEAP_ZERO_MEMORY,
			reinterpret_cast<void*>(param1),
			static_cast<SIZE_T>(param2)));
	case NRS_GET_PRG_TYPE:
		return PT_EDIT_VER;
	case NAS_GET_LANG_ID:
		return 1;
	case NAS_GET_VER:
		return 0x00050007;
	case NRS_GET_WINFORM_COUNT:
	case NRS_GET_WINFORM_HWND:
	case NRS_GET_UNIT_PTR:
	case NRS_GET_AND_CHECK_UNIT_PTR:
	case NAS_GET_HBITMAP:
	case NAS_GET_LIB_DATA_TYPE_INFO:
	case NRS_GET_BITMAP_DATA:
		return 0;
	case NAS_GET_PATH:
		if (param2 != 0) {
			static_cast<char*>(reinterpret_cast<void*>(param2))[0] = '\0';
		}
		return 0;
	default:
		return 0;
	}
}

constexpr std::size_t kMaxStructuredItemCount = 16384;

std::uint16_t ReadLittleEndianU16(const std::uint8_t* data)
{
	return static_cast<std::uint16_t>(data[0]) |
		static_cast<std::uint16_t>(static_cast<std::uint16_t>(data[1]) << 8);
}

std::uint32_t ReadLittleEndianU32(const std::uint8_t* data)
{
	return static_cast<std::uint32_t>(data[0]) |
		(static_cast<std::uint32_t>(data[1]) << 8) |
		(static_cast<std::uint32_t>(data[2]) << 16) |
		(static_cast<std::uint32_t>(data[3]) << 24);
}

void AppendLittleEndianU16(const std::uint16_t value, std::vector<std::uint8_t>& out)
{
	out.push_back(static_cast<std::uint8_t>(value & 0xFFu));
	out.push_back(static_cast<std::uint8_t>(value >> 8));
}

void AppendLittleEndianU32(const std::uint32_t value, std::vector<std::uint8_t>& out)
{
	out.push_back(static_cast<std::uint8_t>(value & 0xFFu));
	out.push_back(static_cast<std::uint8_t>((value >> 8) & 0xFFu));
	out.push_back(static_cast<std::uint8_t>((value >> 16) & 0xFFu));
	out.push_back(static_cast<std::uint8_t>(value >> 24));
}

bool DecodeLengthPrefixedStringArray(
	const std::vector<std::uint8_t>& data,
	const bool utf8,
	std::vector<std::string>& out)
{
	out.clear();
	if (data.size() < sizeof(std::uint16_t)) {
		return false;
	}

	std::size_t offset = 0;
	std::uint32_t count = ReadLittleEndianU16(data.data());
	offset += sizeof(std::uint16_t);
	if (count == 0xFFFFu) {
		if (data.size() - offset < sizeof(std::uint32_t)) {
			return false;
		}
		count = ReadLittleEndianU32(data.data() + offset);
		offset += sizeof(std::uint32_t);
	}
	if (count > kMaxStructuredItemCount) {
		return false;
	}

	out.reserve(static_cast<std::size_t>(count));
	for (std::uint32_t index = 0; index < count; ++index) {
		if (data.size() - offset < sizeof(std::uint32_t)) {
			return false;
		}
		const std::uint32_t length = ReadLittleEndianU32(data.data() + offset);
		offset += sizeof(std::uint32_t);
		if (length > data.size() - offset) {
			return false;
		}
		std::string raw(
			reinterpret_cast<const char*>(data.data() + offset),
			static_cast<std::size_t>(length));
		offset += static_cast<std::size_t>(length);
		if (utf8 && IsValidUtf8(raw)) {
			std::string local;
			if (TryConvertUtf8ToLocal(raw, local)) {
				raw = std::move(local);
			}
		}
		out.push_back(std::move(raw));
	}
	return offset == data.size();
}

bool EncodeLengthPrefixedStringArray(
	const std::vector<std::string>& values,
	const bool utf8,
	std::vector<std::uint8_t>& out)
{
	out.clear();
	if (values.size() > kMaxStructuredItemCount) {
		return false;
	}
	const auto count = static_cast<std::uint32_t>(values.size());
	if (count < 0xFFFFu) {
		AppendLittleEndianU16(static_cast<std::uint16_t>(count), out);
	}
	else {
		AppendLittleEndianU16(0xFFFFu, out);
		AppendLittleEndianU32(count, out);
	}

	for (const auto& value : values) {
		std::string encoded;
		if (utf8 && !TryConvertLocalToUtf8(value, encoded)) {
			return false;
		}
		if (!utf8) {
			encoded = value;
		}
		if (encoded.size() > (std::numeric_limits<std::uint32_t>::max)()) {
			return false;
		}
		AppendLittleEndianU32(static_cast<std::uint32_t>(encoded.size()), out);
		out.insert(out.end(), encoded.begin(), encoded.end());
	}
	return out.size() <= kMaxPropertyDataSize;
}

bool DecodeInt32Array(
	const std::vector<std::uint8_t>& data,
	std::vector<std::int32_t>& out)
{
	out.clear();
	if ((data.size() % sizeof(std::int32_t)) != 0 ||
		data.size() / sizeof(std::int32_t) > kMaxStructuredItemCount) {
		return false;
	}
	out.reserve(data.size() / sizeof(std::int32_t));
	for (std::size_t offset = 0; offset < data.size(); offset += sizeof(std::int32_t)) {
		out.push_back(static_cast<std::int32_t>(ReadLittleEndianU32(data.data() + offset)));
	}
	return true;
}

bool EncodeInt32Array(
	const std::vector<std::int32_t>& values,
	std::vector<std::uint8_t>& out)
{
	out.clear();
	if (values.size() > kMaxStructuredItemCount ||
		values.size() > (kMaxPropertyDataSize / sizeof(std::int32_t))) {
		return false;
	}
	out.reserve(values.size() * sizeof(std::int32_t));
	for (const auto value : values) {
		AppendLittleEndianU32(static_cast<std::uint32_t>(value), out);
	}
	return true;
}

bool IsFixedProperty(
	const UNIT_PROPERTY& property,
	const std::size_t index,
	const bool utf8)
{
	if (index >= kFixedPropertyCount || property.m_shtType != kFixedPropertyTypes[index]) {
		return false;
	}
	return ReadPublishedString(property.m_szName, utf8) == kFixedPropertyNames[index];
}

std::string NormalizePropertyXmlName(
	const std::string& name,
	const std::string& englishName,
	const std::size_t metadataIndex)
{
	if (!name.empty()) {
		return name;
	}
	if (!englishName.empty()) {
		return englishName;
	}
	return "属性索引" + std::to_string(metadataIndex);
}

bool IsLikelyXmlAttributeName(const std::string& name)
{
	if (name.empty()) {
		return false;
	}
	const unsigned char first = static_cast<unsigned char>(name.front());
	if (!(std::isalpha(first) != 0 || first == '_' || first == ':' || first >= 0x80)) {
		return false;
	}
	for (const unsigned char ch : name) {
		if (ch >= 0x80) {
			continue;
		}
		if (std::isalnum(ch) != 0 || ch == '_' || ch == '-' || ch == '.' || ch == ':') {
			continue;
		}
		return false;
	}
	return true;
}

bool IsReservedXmlAttribute(const std::string& name)
{
	static constexpr std::array<std::string_view, 15> kReserved = {
		"名称",
		"备注",
		"左边",
		"顶边",
		"宽度",
		"高度",
		"标记",
		"可视",
		"禁止",
		"鼠标指针",
		"可停留焦点",
		"停留顺序",
		"扩展属性数据",
		"父级",
		"锁定",
	};
	return std::find(kReserved.begin(), kReserved.end(), name) != kReserved.end();
}

bool IsStructuredPropertyNodeName(const std::string& nodeName, const std::string& propertyName)
{
	if (nodeName == propertyName) {
		return true;
	}
	const std::size_t dot = nodeName.rfind('.');
	return dot != std::string::npos && nodeName.substr(dot + 1) == propertyName;
}

const FormControlPropertyXmlNode* FindStructuredPropertyNode(
	const std::vector<FormControlPropertyXmlNode>& children,
	const std::string& propertyName,
	bool& outDuplicate)
{
	outDuplicate = false;
	const FormControlPropertyXmlNode* match = nullptr;
	for (const auto& child : children) {
		if (!IsStructuredPropertyNodeName(child.name, propertyName)) {
			continue;
		}
		if (match != nullptr) {
			outDuplicate = true;
			continue;
		}
		match = &child;
	}
	return match;
}

std::string GetXmlNodeAttribute(
	const FormControlPropertyXmlNode& node,
	const std::string& name)
{
	for (const auto& attribute : node.attributes) {
		if (attribute.first == name) {
			return attribute.second;
		}
	}
	return std::string();
}

bool TryGetXmlNodeIndex(
	const FormControlPropertyXmlNode& node,
	const std::size_t defaultIndex,
	std::size_t& outIndex)
{
	const std::string indexText = GetXmlNodeAttribute(node, "索引");
	if (indexText.empty()) {
		outIndex = defaultIndex;
		return true;
	}
	std::int32_t parsedIndex = 0;
	if (!TryParseInt32(indexText, parsedIndex) || parsedIndex < 0 ||
		static_cast<std::size_t>(parsedIndex) >= kMaxStructuredItemCount) {
		return false;
	}
	outIndex = static_cast<std::size_t>(parsedIndex);
	return true;
}

bool TryReadStructuredTextValue(
	const FormControlPropertyXmlNode& node,
	std::string& outValue)
{
	for (const char* name : { "文本", "标题", "值" }) {
		const std::string value = GetXmlNodeAttribute(node, name);
		if (!value.empty() || std::any_of(
				node.attributes.begin(), node.attributes.end(),
				[name](const auto& attribute) { return attribute.first == name; })) {
			outValue = value;
			return true;
		}
	}
	return false;
}

bool EncodeStructuredProperty(
	const FormControlPropertyXmlNode& node,
	const FormControlPropertyCollectionKind kind,
	const bool utf8,
	std::vector<std::uint8_t>& outData)
{
	std::vector<std::string> textValues;
	std::vector<std::int32_t> integerValues;
	std::vector<bool> assigned;
	std::size_t nextIndex = 0;
	for (const auto& item : node.children) {
		std::size_t index = 0;
		if (!TryGetXmlNodeIndex(item, nextIndex, index)) return false;
		if (index >= assigned.size()) {
			assigned.resize(index + 1, false);
			if (kind == FormControlPropertyCollectionKind::Text) textValues.resize(index + 1);
			else integerValues.resize(index + 1, 0);
		}
		if (assigned[index]) return false;
		if (kind == FormControlPropertyCollectionKind::Integer) {
			std::int32_t value = 0;
			if (!TryParseInt32(GetXmlNodeAttribute(item, "数值"), value)) return false;
			integerValues[index] = value;
		}
		else {
			if (!TryReadStructuredTextValue(item, textValues[index])) return false;
		}
		assigned[index] = true;
		nextIndex = index + 1;
	}
	if (kind == FormControlPropertyCollectionKind::Integer) {
		return EncodeInt32Array(integerValues, outData);
	}
	return EncodeLengthPrefixedStringArray(textValues, utf8, outData);
}

bool TryInferCollectionKind(
	const FormControlPropertyValue& value,
	FormControlPropertyCollectionKind& outKind)
{
	if (value.kind != FormControlPropertyValueKind::Binary || value.binaryValue.empty()) return false;
	std::vector<std::string> strings;
	if (DecodeLengthPrefixedStringArray(value.binaryValue, false, strings)) {
		outKind = FormControlPropertyCollectionKind::Text;
		return true;
	}
	std::vector<std::int32_t> integers;
	if (DecodeInt32Array(value.binaryValue, integers)) {
		outKind = FormControlPropertyCollectionKind::Integer;
		return true;
	}
	return false;
}

bool HasXmlNodeAttribute(
	const FormControlPropertyXmlNode& node,
	const std::string& name)
{
	return std::any_of(
		node.attributes.begin(),
		node.attributes.end(),
		[&name](const auto& attribute) { return attribute.first == name; });
}

const FormControlPropertyXmlNode* FindPropertyXmlNode(
	const std::vector<FormControlPropertyXmlNode>& children,
	const FormControlPropertyDefinition& definition,
	bool& outDuplicate)
{
	outDuplicate = false;
	const FormControlPropertyXmlNode* result = nullptr;
	const auto matches = [&definition](const std::string& nodeName) {
		for (const std::string& candidate : {
			definition.name, definition.englishName, definition.xmlName }) {
			if (!candidate.empty() && IsStructuredPropertyNodeName(nodeName, candidate)) return true;
		}
		return false;
	};
	for (const auto& child : children) {
		if (!matches(child.name)) continue;
		if (result != nullptr) {
			outDuplicate = true;
			continue;
		}
		result = &child;
	}
	return result;
}

bool TryInferXmlCollectionKind(
	const FormControlPropertyXmlNode& node,
	FormControlPropertyCollectionKind& outKind)
{
	bool found = false;
	for (const auto& item : node.children) {
		FormControlPropertyCollectionKind candidate;
		if (HasXmlNodeAttribute(item, "数值")) {
			candidate = FormControlPropertyCollectionKind::Integer;
		}
		else if (HasXmlNodeAttribute(item, "文本") ||
			HasXmlNodeAttribute(item, "标题") || HasXmlNodeAttribute(item, "值")) {
			candidate = FormControlPropertyCollectionKind::Text;
		}
		else {
			return false;
		}
		if (found && candidate != outKind) return false;
		outKind = candidate;
		found = true;
	}
	return found;
}

bool AreValuesEquivalent(const FormControlPropertyValue& left, const FormControlPropertyValue& right)
{
	if (left.kind != right.kind) {
		return false;
	}
	switch (left.kind) {
	case FormControlPropertyValueKind::Integer: return left.integerValue == right.integerValue;
	case FormControlPropertyValueKind::Double: return left.doubleValue == right.doubleValue;
	case FormControlPropertyValueKind::Boolean: return left.booleanValue == right.booleanValue;
	case FormControlPropertyValueKind::Text: return left.textValue == right.textValue;
	case FormControlPropertyValueKind::Binary: return left.binaryValue == right.binaryValue;
	default: return false;
	}
}

bool ParseXmlPropertyValue(
	const FormControlPropertyDefinition& definition,
	const std::string& text,
	FormControlPropertyValue& outValue,
	const FormControlPropertyValueKind unknownKind = FormControlPropertyValueKind::Unknown)
{
	outValue = {};
	outValue.definition = definition;
	if (IsBooleanPropertyType(definition.dataType)) {
		outValue.kind = FormControlPropertyValueKind::Boolean;
		return TryParseBoolean(text, outValue.booleanValue);
	}
	if (IsDoublePropertyType(definition.dataType)) {
		outValue.kind = FormControlPropertyValueKind::Double;
		return TryParseDouble(text, outValue.doubleValue);
	}
	if (IsTextPropertyType(definition.dataType)) {
		outValue.kind = FormControlPropertyValueKind::Text;
		outValue.textValue = text;
		return true;
	}
	if (IsBinaryPropertyType(definition.dataType)) {
		outValue.kind = FormControlPropertyValueKind::Binary;
		return DecodeBase64(text, outValue.binaryValue);
	}
	if (unknownKind == FormControlPropertyValueKind::Text) {
		outValue.kind = FormControlPropertyValueKind::Text;
		outValue.textValue = text;
		return true;
	}
	if (unknownKind == FormControlPropertyValueKind::Binary) {
		outValue.kind = FormControlPropertyValueKind::Binary;
		return DecodeBase64(text, outValue.binaryValue);
	}
	if (unknownKind == FormControlPropertyValueKind::Integer) {
		outValue.kind = FormControlPropertyValueKind::Integer;
		return TryParseInt32(text, outValue.integerValue);
	}

	// Unknown property editor types are preserved as text unless their public
	// metadata supplied a concrete type above.  This keeps a new FNE's
	// extension attributes editable without guessing a private numeric code.
	outValue.kind = FormControlPropertyValueKind::Text;
	outValue.textValue = text;
	return true;
}

bool ReadUnitPropertyValue(
	const FormControlPropertyDefinition& definition,
	const UNIT_PROPERTY_VALUE& raw,
	const bool utf8,
	FormControlPropertyValue& outValue)
{
	outValue = {};
	outValue.definition = definition;
	if (IsBooleanPropertyType(definition.dataType)) {
		outValue.kind = FormControlPropertyValueKind::Boolean;
		outValue.booleanValue = raw.m_bool != FALSE;
		return true;
	}
	if (IsDoublePropertyType(definition.dataType)) {
		outValue.kind = FormControlPropertyValueKind::Double;
		outValue.doubleValue = raw.m_double;
		return std::isfinite(outValue.doubleValue);
	}
	if (IsTextPropertyType(definition.dataType)) {
		outValue.kind = FormControlPropertyValueKind::Text;
		std::string rawText;
		if (!TryReadCString(raw.m_szText, rawText)) {
			return false;
		}
		if (!utf8) {
			outValue.textValue = std::move(rawText);
			return true;
		}
		return TryConvertUtf8ToLocal(rawText, outValue.textValue);
	}
	if (IsBinaryPropertyType(definition.dataType)) {
		outValue.kind = FormControlPropertyValueKind::Binary;
		return CopyDirectBytes(raw.m_data.m_pData, raw.m_data.m_nDataSize, outValue.binaryValue);
	}
	// Unknown editor types are intentionally not guessed as text.  The
	// UNIT_PROPERTY_VALUE union has no discriminator; interpreting an integer
	// bit pattern as a pointer can produce readable-looking garbage and corrupt
	// the generated XML.  Only public metadata-declared text types above may be
	// decoded as strings.
	if (TryReadUnknownBinaryValue(raw, outValue.binaryValue)) {
		outValue.kind = FormControlPropertyValueKind::Binary;
		return true;
	}

	outValue.kind = FormControlPropertyValueKind::Integer;
	outValue.integerValue = raw.m_int;
	return true;
}

}  // namespace

struct FormControlPropertyCodec::LibraryState {
	HMODULE module = nullptr;
	const LIB_INFO* info = nullptr;
	bool attempted = false;
	bool utf8 = false;
	std::string path;
};

FormControlPropertyCodec::FormControlPropertyCodec(
	const std::string& sourcePath,
	const std::vector<FormControlSupportLibrary>& libraries,
	const std::vector<std::filesystem::path>& searchDirectories,
	const bool restrictSearch)
	: m_sourcePath(sourcePath)
	, m_libraries(libraries)
	, m_searchDirectories(searchDirectories)
	, m_restrictSearch(restrictSearch)
	, m_libraryStates(libraries.size())
{
}

FormControlPropertyCodec::~FormControlPropertyCodec()
{
	if (m_parentWindow != nullptr) {
		DestroyWindow(static_cast<HWND>(m_parentWindow));
		m_parentWindow = nullptr;
	}
	// Keep support-library modules loaded until process exit. Published function
	// pointers and component objects may still refer to their module code.
}

std::vector<std::filesystem::path> BuildSupportLibraryCandidates(
	const std::string& sourcePath,
	const FormControlSupportLibrary& library,
	const std::vector<std::filesystem::path>& searchDirectories,
	const bool restrictSearch)
{
	std::vector<std::filesystem::path> candidates;
	const auto pushUnique = [&candidates](std::filesystem::path candidate) {
		if (candidate.empty()) {
			return;
		}
		candidate = candidate.lexically_normal();
		if (std::find(candidates.begin(), candidates.end(), candidate) == candidates.end()) {
			candidates.push_back(std::move(candidate));
		}
	};

	if (!library.resolvedPath.empty()) {
		pushUnique(Utf8PathToPath(library.resolvedPath));
	}

	std::string fileName = TrimAscii(library.fileName);
	if (fileName.size() >= 2 && fileName.front() == '"' && fileName.back() == '"') {
		fileName = fileName.substr(1, fileName.size() - 2);
	}
	if (!fileName.empty() && fileName.front() == '$') {
		fileName.erase(fileName.begin());
	}
	std::filesystem::path filePath(fileName);
	std::vector<std::filesystem::path> fileVariants;
	if (!filePath.empty()) {
		if (filePath.extension().empty()) {
			fileVariants.push_back(filePath.string() + ".fne");
			fileVariants.push_back(filePath.string() + ".fne.dll");
			fileVariants.push_back(filePath.string() + ".fnr");
			fileVariants.push_back(filePath.string() + ".dll");
			fileVariants.push_back(filePath);
		}
		else {
			fileVariants.push_back(filePath);
			if (filePath.extension() == L".fne") {
				fileVariants.push_back(filePath.string() + ".dll");
			}
		}
	}
	if (filePath.is_absolute()) {
		for (const auto& variant : fileVariants) {
			pushUnique(variant);
		}
		return candidates;
	}
	if (restrictSearch && searchDirectories.empty()) {
		return candidates;
	}

	const auto addBase = [&](const std::filesystem::path& base) {
		if (base.empty()) {
			return;
		}
		for (const auto& variant : fileVariants) {
			pushUnique(base / variant);
			pushUnique(base / L"lib" / variant);
			std::filesystem::path current = base;
			while (!current.empty()) {
				pushUnique(current / L"lib" / variant);
				if (current == current.root_path()) {
					break;
				}
				current = current.parent_path();
			}
		}
	};

	for (const auto& directory : searchDirectories) {
		addBase(directory);
	}
	if (restrictSearch) {
		return candidates;
	}

	std::error_code ec;
	if (!sourcePath.empty()) {
		addBase(Utf8PathToPath(sourcePath).parent_path());
	}
	addBase(std::filesystem::current_path(ec));
	addBase(std::filesystem::path(GetBasePath()));
	for (const auto& directory : GetRegisteredEplOpenCommandBaseDirs()) {
		addBase(directory);
	}
	return candidates;
}

FormControlPropertyCodec::LibraryState* FormControlPropertyCodec::EnsureLibrary(
	const std::uint16_t supportIndex)
{
	if (supportIndex == 0 || supportIndex > m_libraryStates.size()) {
		return nullptr;
	}
	auto& state = m_libraryStates[static_cast<std::size_t>(supportIndex - 1)];
	if (state.attempted) {
		return state.info == nullptr ? nullptr : &state;
	}
	state.attempted = true;
	const auto candidates = BuildSupportLibraryCandidates(
		m_sourcePath,
		m_libraries[static_cast<std::size_t>(supportIndex - 1)],
		m_searchDirectories,
		m_restrictSearch);
	for (const auto& candidate : candidates) {
		HMODULE module = LoadLibraryExW(candidate.c_str(), nullptr, 0);
		if (module == nullptr) {
			continue;
		}
		const auto getInfo = reinterpret_cast<PFN_GET_LIB_INFO>(GetProcAddress(module, FUNCNAME_GET_LIB_INFO));
		const LIB_INFO* info = nullptr;
		if (!CallGetNewInfoSafely(getInfo, info) ||
			!IsReadableMemoryRange(info, sizeof(LIB_INFO))) {
			FreeLibrary(module);
			continue;
		}
		(void)CallNotifyLibrarySafely(
			info->m_pfnNotify,
			NL_SYS_NOTIFY_FUNCTION,
			reinterpret_cast<DWORD_PTR>(&HostSystemNotify),
			0);
		state.module = module;
		state.info = info;
		state.utf8 = DetectUtf8LibraryStrings(info);
		state.path = PathToUtf8(candidate);
		return &state;
	}
	return nullptr;
}

struct FormControlPropertyCodec::TypeContext {
	const LIB_DATA_TYPE_INFO* dataType = nullptr;
	std::vector<FormControlPropertyDefinition> properties;
	std::size_t fixedPropertyCount = 0;
	bool utf8 = false;
	PFN_NOTIFY_LIB notifyLibrary = nullptr;
	PFN_CREATE_UNIT create = nullptr;
	PFN_NOTIFY_PROPERTY_CHANGED notify = nullptr;
	PFN_GET_ALL_PROPERTY_DATA getAll = nullptr;
	PFN_GET_PROPERTY_DATA getProperty = nullptr;
};

bool FormControlPropertyCodec::BuildTypeContext(
	const std::int32_t rawType,
	TypeContext& out,
	std::string* outError)
{
	out = {};
	if (outError != nullptr) {
		outError->clear();
	}
	const std::uint32_t value = static_cast<std::uint32_t>(rawType);
	if ((value & 0x80000000u) != 0) {
		if (outError != nullptr) *outError = "window_control_type_is_not_library_type";
		return false;
	}
	const auto supportIndex = static_cast<std::uint16_t>(value >> 16);
	const auto typeIndex = static_cast<std::uint16_t>(value & 0xFFFFu);
	if (supportIndex == 0 || typeIndex == 0) {
		if (outError != nullptr) *outError = "window_control_type_index_invalid";
		return false;
	}
	const auto* library = EnsureLibrary(supportIndex);
	if (library == nullptr || library->info == nullptr ||
		library->info->m_nDataTypeCount <= 0 ||
		library->info->m_nDataTypeCount > 16384 ||
		typeIndex > static_cast<std::uint16_t>(library->info->m_nDataTypeCount) ||
		library->info->m_pDataType == nullptr ||
		!IsReadableMemoryRange(
			library->info->m_pDataType,
			sizeof(LIB_DATA_TYPE_INFO) * static_cast<std::size_t>(library->info->m_nDataTypeCount))) {
		if (outError != nullptr) *outError = "window_control_type_metadata_unavailable";
		return false;
	}

	const auto& dataType = library->info->m_pDataType[typeIndex - 1];
	if ((dataType.m_dwState & LDT_WIN_UNIT) == 0 ||
		dataType.m_nPropertyCount < 0 ||
		dataType.m_nPropertyCount > 16384 ||
		(dataType.m_nPropertyCount > 0 &&
			(dataType.m_pPropertyBegin == nullptr ||
			!IsReadableMemoryRange(
				dataType.m_pPropertyBegin,
				sizeof(UNIT_PROPERTY) * static_cast<std::size_t>(dataType.m_nPropertyCount))))) {
		if (outError != nullptr) *outError = "window_control_property_metadata_invalid";
		return false;
	}

	out.dataType = &dataType;
	out.utf8 = library->utf8;
	out.notifyLibrary = library->info->m_pfnNotify;
	out.properties.reserve(static_cast<std::size_t>(dataType.m_nPropertyCount));
	for (int index = 0; index < dataType.m_nPropertyCount; ++index) {
		const auto& source = dataType.m_pPropertyBegin[index];
		FormControlPropertyDefinition property;
		property.name = ReadPublishedString(source.m_szName, library->utf8);
		property.englishName = ReadPublishedString(source.m_szEgName, library->utf8);
		property.dataType = source.m_shtType;
		property.state = source.m_wState;
		property.metadataIndex = static_cast<std::size_t>(index);
		property.xmlName = NormalizePropertyXmlName(property.name, property.englishName, property.metadataIndex);
		if (!IsLikelyXmlAttributeName(property.xmlName) ||
			IsReservedXmlAttribute(property.xmlName) ||
			std::any_of(
				out.properties.begin(),
				out.properties.end(),
				[&property](const FormControlPropertyDefinition& existing) {
					return existing.xmlName == property.xmlName;
				})) {
			property.xmlName = "属性索引" + std::to_string(property.metadataIndex);
			std::size_t suffix = 1;
			while (std::any_of(
					out.properties.begin(),
					out.properties.end(),
					[&property](const FormControlPropertyDefinition& existing) {
						return existing.xmlName == property.xmlName;
					})) {
				property.xmlName = "属性索引" + std::to_string(property.metadataIndex) + "_" + std::to_string(suffix++);
			}
		}
		out.properties.push_back(std::move(property));
	}

	for (std::size_t index = 0; index < out.properties.size() && index < kFixedPropertyCount; ++index) {
		if (!IsFixedProperty(
				dataType.m_pPropertyBegin[index],
				index,
				library->utf8)) {
			break;
		}
		out.fixedPropertyCount = index + 1;
	}
	if (out.fixedPropertyCount != 0 && out.fixedPropertyCount != kFixedPropertyCount) {
		out.fixedPropertyCount = 0;
	}
	for (std::size_t index = out.fixedPropertyCount; index < out.properties.size(); ++index) {
		out.properties[index].callbackIndex = index - out.fixedPropertyCount;
	}

	const auto getter = reinterpret_cast<PFN_GET_INTERFACE>(dataType.m_pfnGetInterface);
	out.create = reinterpret_cast<PFN_CREATE_UNIT>(CallGetInterfaceSafely(getter, ITF_CREATE_UNIT));
	out.notify = reinterpret_cast<PFN_NOTIFY_PROPERTY_CHANGED>(CallGetInterfaceSafely(getter, ITF_NOTIFY_PROPERTY_CHANGED));
	out.getAll = reinterpret_cast<PFN_GET_ALL_PROPERTY_DATA>(CallGetInterfaceSafely(getter, ITF_GET_ALL_PROPERTY_DATA));
	out.getProperty = reinterpret_cast<PFN_GET_PROPERTY_DATA>(CallGetInterfaceSafely(getter, ITF_GET_PROPERTY_DATA));
	std::cerr << "property probe interfaces type=" << rawType
		<< " getter=" << reinterpret_cast<const void*>(getter)
		<< " create=" << reinterpret_cast<const void*>(out.create)
		<< " get=" << reinterpret_cast<const void*>(out.getProperty)
		<< " all=" << reinterpret_cast<const void*>(out.getAll)
		<< " set=" << reinterpret_cast<const void*>(out.notify) << "\n";
	return true;
}

bool FormControlPropertyCodec::EnsureParentWindow()
{
	if (m_parentWindow != nullptr) {
		return true;
	}
	const HWND parent = CreateWindowExW(
		0,
		L"STATIC",
		L"e-packager property probe",
		WS_POPUP,
		0,
		0,
		800,
		600,
		nullptr,
		nullptr,
		GetModuleHandleW(nullptr),
		nullptr);
	if (parent == nullptr) {
		return false;
	}
	m_parentWindow = parent;
	return true;
}

std::uint32_t FormControlPropertyCodec::CreateUnit(
	const TypeContext& context,
	const std::vector<std::uint8_t>& data,
	const std::uint32_t formId,
	const std::uint32_t unitId)
{
	if (!EnsureParentWindow()) {
		return 0;
	}
	LPBYTE pointer = data.empty() ? nullptr : const_cast<LPBYTE>(data.data());
	// 原始窗体 ID 带有易语言运行时对象标志。属性接口只需要设计期上下文，
	// 传入该 ID 会让核心库误走真实运行时窗体查询路径。
	(void)formId;
	return static_cast<std::uint32_t>(CallCreateUnitSafely(
		context.create,
		pointer,
		static_cast<INT>(data.size()),
		static_cast<HWND>(m_parentWindow),
		kPropertyProbeFormId,
		unitId));
}

namespace {

bool FillNativePropertyValue(
	const FormControlPropertyValue& value,
	const bool utf8,
	std::string& textStorage,
	UNIT_PROPERTY_VALUE& out)
{
	std::memset(&out, 0, sizeof(out));
	switch (value.kind) {
	case FormControlPropertyValueKind::Integer:
		out.m_int = value.integerValue;
		return true;
	case FormControlPropertyValueKind::Double:
		out.m_double = value.doubleValue;
		return true;
	case FormControlPropertyValueKind::Boolean:
		out.m_bool = value.booleanValue ? TRUE : FALSE;
		return true;
	case FormControlPropertyValueKind::Text:
		if (utf8 && !TryConvertLocalToUtf8(value.textValue, textStorage)) {
			return false;
		}
		out.m_szText = (utf8 ? textStorage : value.textValue).c_str();
		return true;
	case FormControlPropertyValueKind::Binary:
		out.m_data.m_pData = value.binaryValue.empty()
			? nullptr
			: const_cast<LPBYTE>(value.binaryValue.data());
		out.m_data.m_nDataSize = static_cast<INT>(value.binaryValue.size());
		return true;
	default:
		return false;
	}
}

}  // namespace

}  // namespace e2txt

namespace e2txt {

std::string FormControlPropertyCodec::ValueToXmlText(const FormControlPropertyValue& value)
{
	switch (value.kind) {
	case FormControlPropertyValueKind::Integer:
		return std::to_string(value.integerValue);
	case FormControlPropertyValueKind::Double: {
		std::ostringstream stream;
		stream << std::setprecision(17) << value.doubleValue;
		return stream.str();
	}
	case FormControlPropertyValueKind::Boolean:
		return value.booleanValue ? "真" : "假";
	case FormControlPropertyValueKind::Text:
		return value.textValue;
	case FormControlPropertyValueKind::Binary:
		return EncodeBase64(value.binaryValue);
	default:
		return std::string();
	}
}

bool FormControlPropertyCodec::Decode(
	const std::int32_t dataType,
	const std::vector<std::uint8_t>& propertyData,
	const std::uint32_t formId,
	const std::uint32_t unitId,
	std::vector<FormControlPropertyValue>& outValues,
	std::string* outError,
	FormControlPropertySemanticData* outSemantic)
{
	outValues.clear();
	if (outSemantic != nullptr) {
		*outSemantic = {};
	}
	TypeContext context;
	if (!BuildTypeContext(dataType, context, outError)) {
		return false;
	}
	if (context.properties.size() <= context.fixedPropertyCount) {
		return true;
	}
	// A library may publish property definitions while intentionally omitting
	// the design-time unit interfaces (for example a runtime-only or partially
	// implemented control).  The raw extension bytes remain authoritative; a
	// missing getter therefore means “not decoded”, rather than a malformed
	// window file.
	if (context.getProperty == nullptr || context.create == nullptr) return true;

	const HUNIT unit = static_cast<HUNIT>(CreateUnit(context, propertyData, formId, unitId));
	if (unit == 0) {
		if (outError != nullptr) *outError = "window_control_create_failed";
		return false;
	}
	for (std::size_t index = context.fixedPropertyCount; index < context.properties.size(); ++index) {
		UNIT_PROPERTY_VALUE rawValue = {};
		if (!CallGetPropertyDataSafely(
				context.getProperty,
				unit,
				static_cast<int>(context.properties[index].callbackIndex),
				rawValue)) {
			continue;
		}
		FormControlPropertyValue value;
		const bool valueRead = ReadUnitPropertyValue(context.properties[index], rawValue, context.utf8, value);
		if (valueRead &&
			(value.kind == FormControlPropertyValueKind::Text ||
				value.kind == FormControlPropertyValueKind::Binary)) {
			const void* returnedData = value.kind == FormControlPropertyValueKind::Text
				? static_cast<const void*>(rawValue.m_szText)
				: static_cast<const void*>(rawValue.m_data.m_pData);
			// lib2.h 明确规定 getter 返回的文本/字节集由调用方释放。
			FreePropertyValueDataSafely(context.notifyLibrary, returnedData);
		}
		if (!valueRead) {
			continue;
		}
		if (outSemantic != nullptr && value.definition.dataType == UD_CUSTOMIZE) {
			FormControlPropertyCollectionKind collectionKind;
			if (TryInferCollectionKind(value, collectionKind)) {
				FormControlPropertyCollection collection;
				collection.definition = value.definition;
				collection.kind = collectionKind;
				if (collectionKind == FormControlPropertyCollectionKind::Text) {
					(void)DecodeLengthPrefixedStringArray(
						value.binaryValue,
						context.utf8,
						collection.textValues);
				}
				else {
					(void)DecodeInt32Array(value.binaryValue, collection.integerValues);
				}
				outSemantic->collections.push_back(std::move(collection));
			}
		}
		outValues.push_back(std::move(value));
	}
	return true;
}

bool FormControlPropertyCodec::Apply(
	const std::int32_t dataType,
	const std::vector<std::uint8_t>& originalData,
	const std::uint32_t formId,
	const std::uint32_t unitId,
	const std::vector<std::pair<std::string, std::string>>& xmlAttributes,
	std::vector<std::uint8_t>& outData,
	std::string* outError,
	const std::vector<FormControlPropertyXmlNode>& xmlChildren)
{
	outData = originalData;
	if (outError != nullptr) {
		outError->clear();
	}

	std::vector<std::pair<std::string, std::string>> propertyAttributes;
	for (const auto& attribute : xmlAttributes) {
		if (!IsReservedXmlAttribute(attribute.first)) {
			propertyAttributes.push_back(attribute);
		}
	}
	if (propertyAttributes.empty() && xmlChildren.empty()) {
		return true;
	}

	TypeContext context;
	if (!BuildTypeContext(dataType, context, outError)) {
		return false;
	}
	std::vector<FormControlPropertyValue> originalValues;
	const bool canReadOriginal = Decode(
		dataType,
		originalData,
		formId,
		unitId,
		originalValues,
		nullptr);

	std::vector<FormControlPropertyValue> updates;
	for (const auto& attribute : propertyAttributes) {
		const auto definitionIt = std::find_if(
			context.properties.begin() + static_cast<std::ptrdiff_t>(context.fixedPropertyCount),
			context.properties.end(),
			[&attribute](const FormControlPropertyDefinition& definition) {
				return definition.xmlName == attribute.first ||
					(!definition.name.empty() && definition.name == attribute.first) ||
					(!definition.englishName.empty() && definition.englishName == attribute.first);
			});
		if (definitionIt == context.properties.end()) {
			continue;
		}
		bool duplicateStructuredNode = false;
		const auto* structuredNode = definitionIt->dataType == UD_CUSTOMIZE
			? FindPropertyXmlNode(xmlChildren, *definitionIt, duplicateStructuredNode) : nullptr;
		if (structuredNode != nullptr) {
			if (duplicateStructuredNode) {
				if (outError != nullptr) *outError = "window_control_customize_data_duplicate: " + definitionIt->xmlName;
				return false;
			}
			continue;
		}

		FormControlPropertyValue parsed;
		FormControlPropertyValueKind unknownKind = FormControlPropertyValueKind::Unknown;
		if (definitionIt->dataType == UD_CUSTOMIZE) {
			// A scalar representation of a custom value is base64 by convention;
			// preserve it as bytes without needing to know the private payload ABI.
			unknownKind = FormControlPropertyValueKind::Binary;
		}
		if (!ParseXmlPropertyValue(*definitionIt, attribute.second, parsed, unknownKind)) {
			if (outError != nullptr) {
				*outError = "window_control_property_value_invalid: " + attribute.first;
			}
			return false;
		}
		const auto originalIt = std::find_if(
			originalValues.begin(),
			originalValues.end(),
			[&definitionIt](const FormControlPropertyValue& value) {
				return value.definition.metadataIndex == definitionIt->metadataIndex;
			});
		if (canReadOriginal && originalIt != originalValues.end() && AreValuesEquivalent(*originalIt, parsed)) {
			continue;
		}
		updates.push_back(std::move(parsed));
	}

	for (const auto& definition : context.properties) {
		if (definition.dataType != UD_CUSTOMIZE) continue;
		bool duplicateStructuredNode = false;
		const FormControlPropertyXmlNode* structuredNode = FindPropertyXmlNode(
			xmlChildren, definition, duplicateStructuredNode);
		if (structuredNode == nullptr) {
			continue;
		}
		if (duplicateStructuredNode) {
			if (outError != nullptr) {
				*outError = "window_control_customize_data_duplicate: " + definition.xmlName;
			}
			return false;
		}
		FormControlPropertyCollectionKind collectionKind;
		if (!TryInferXmlCollectionKind(*structuredNode, collectionKind)) {
			if (outError != nullptr) {
				*outError = "window_control_customize_data_invalid: " + definition.xmlName;
			}
			return false;
		}
		std::vector<std::uint8_t> structuredData;
		if (!EncodeStructuredProperty(*structuredNode, collectionKind, context.utf8, structuredData)) {
			if (outError != nullptr) *outError = "window_control_customize_data_invalid: " + definition.xmlName;
			return false;
		}
		FormControlPropertyValue parsed;
		parsed.definition = definition;
		parsed.kind = FormControlPropertyValueKind::Binary;
		parsed.binaryValue = std::move(structuredData);
		const auto originalIt = std::find_if(
			originalValues.begin(),
			originalValues.end(),
			[&definition](const FormControlPropertyValue& value) {
				return value.definition.metadataIndex == definition.metadataIndex;
			});
		if (canReadOriginal && originalIt != originalValues.end() && AreValuesEquivalent(*originalIt, parsed)) {
			continue;
		}
		updates.push_back(std::move(parsed));
	}

	if (updates.empty()) {
		return true;
	}
	if (context.notify == nullptr || context.getAll == nullptr) {
		if (outError != nullptr) *outError = "window_control_property_update_interface_unavailable";
		return false;
	}
	const HUNIT unit = static_cast<HUNIT>(CreateUnit(context, originalData, formId, unitId));
	if (unit == 0) {
		if (outError != nullptr) *outError = "window_control_create_failed";
		return false;
	}
	for (const auto& update : updates) {
		UNIT_PROPERTY_VALUE nativeValue = {};
		std::string nativeText;
		bool needsRecreate = false;
		if (!FillNativePropertyValue(update, context.utf8, nativeText, nativeValue) ||
			!CallNotifyPropertyChangedSafely(
				context.notify,
				unit,
				static_cast<int>(update.definition.callbackIndex),
				nativeValue,
				needsRecreate)) {
			if (outError != nullptr) *outError = "window_control_property_update_failed: " + update.definition.xmlName;
			return false;
		}
	}

	const HGLOBAL globalData = CallGetAllPropertyDataSafely(context.getAll, unit);
	if (globalData == nullptr) {
		if (outError != nullptr) *outError = "window_control_property_save_failed";
		return false;
	}
	const bool copied = CopyGlobalBytes(globalData, outData);
	GlobalFree(globalData);
	if (!copied) {
		if (outError != nullptr) *outError = "window_control_property_save_data_invalid";
		return false;
	}
	return true;
}

}  // namespace e2txt
