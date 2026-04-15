#include "BundlePathUtils.h"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <utility>

namespace e2txt {

namespace {

std::string ToLowerAscii(std::string text)
{
	std::transform(
		text.begin(),
		text.end(),
		text.begin(),
		[](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
	return text;
}

std::string TrimWindowsFileName(std::string text)
{
	while (!text.empty() && (text.back() == ' ' || text.back() == '.')) {
		text.pop_back();
	}
	while (!text.empty() && text.front() == ' ') {
		text.erase(text.begin());
	}
	return text;
}

bool IsReservedWindowsName(const std::string& name)
{
	static const std::unordered_set<std::string> kReserved = {
		"con", "prn", "aux", "nul",
		"com1", "com2", "com3", "com4", "com5", "com6", "com7", "com8", "com9",
		"lpt1", "lpt2", "lpt3", "lpt4", "lpt5", "lpt6", "lpt7", "lpt8", "lpt9",
	};
	return kReserved.contains(ToLowerAscii(name));
}

std::string SanitizePathSegment(std::string segment)
{
	if (segment.empty() || segment == "." || segment == "..") {
		return "_";
	}

	for (char& ch : segment) {
		const unsigned char uch = static_cast<unsigned char>(ch);
		if (uch < 0x20 || ch == '<' || ch == '>' || ch == ':' || ch == '"' ||
			ch == '/' || ch == '\\' || ch == '|' || ch == '?' || ch == '*') {
			ch = '_';
		}
	}
	segment = TrimWindowsFileName(std::move(segment));
	if (segment.empty()) {
		segment = "_";
	}

	std::filesystem::path path(segment);
	const std::string stemLower = ToLowerAscii(path.stem().string());
	if (IsReservedWindowsName(stemLower)) {
		segment.insert(segment.begin(), '_');
	}
	return segment;
}

}  // namespace

std::string SanitizeRelativePath(const std::string& rawRelativePath)
{
	std::filesystem::path rawPath(rawRelativePath);
	std::filesystem::path sanitized;
	for (const auto& part : rawPath) {
		const std::string segment = part.generic_string();
		if (segment.empty() || segment == "/" || segment == "\\") {
			continue;
		}
		sanitized /= SanitizePathSegment(segment);
	}
	return sanitized.generic_string();
}

std::string MakeUniqueRelativePath(
	const std::string& rawRelativePath,
	std::unordered_set<std::string>& usedRelativePaths)
{
	std::filesystem::path path = SanitizeRelativePath(rawRelativePath);
	if (path.empty()) {
		path = "src/_";
	}

	const std::string extension = path.extension().generic_string();
	const std::string stem = path.stem().generic_string();
	const std::filesystem::path parent = path.parent_path();

	std::filesystem::path candidate = path;
	int suffix = 2;
	while (true) {
		const std::string key = ToLowerAscii(candidate.generic_string());
		if (!usedRelativePaths.contains(key)) {
			usedRelativePaths.insert(key);
			return candidate.generic_string();
		}

		candidate = parent / (stem + "_" + std::to_string(suffix) + extension);
		++suffix;
	}
}

void ReserveRelativePath(
	const std::string& rawRelativePath,
	std::unordered_set<std::string>& usedRelativePaths)
{
	const std::string sanitized = SanitizeRelativePath(rawRelativePath);
	if (!sanitized.empty()) {
		usedRelativePaths.insert(ToLowerAscii(sanitized));
	}
}

void ReserveBundleRelativePaths(std::unordered_set<std::string>& usedRelativePaths)
{
	ReserveRelativePath("project/模块.json", usedRelativePaths);
	ReserveRelativePath("project/_meta.json", usedRelativePaths);
	ReserveRelativePath("project/.native_source.bin", usedRelativePaths);
	ReserveRelativePath("project/.native_source_map.json", usedRelativePaths);
	ReserveRelativePath("project/.native_symbol_map.json", usedRelativePaths);
	ReserveRelativePath("src/.数据类型.txt", usedRelativePaths);
	ReserveRelativePath("src/.DLL声明.txt", usedRelativePaths);
	ReserveRelativePath("src/.常量.txt", usedRelativePaths);
	ReserveRelativePath("src/.全局变量.txt", usedRelativePaths);
	ReserveRelativePath("image/list.json", usedRelativePaths);
	ReserveRelativePath("audio/list.json", usedRelativePaths);
}

}  // namespace e2txt
