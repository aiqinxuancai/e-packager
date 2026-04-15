#pragma once

#include <string>
#include <unordered_set>

namespace e2txt {

std::string SanitizeRelativePath(const std::string& rawRelativePath);

std::string MakeUniqueRelativePath(
	const std::string& rawRelativePath,
	std::unordered_set<std::string>& usedRelativePaths);

void ReserveRelativePath(
	const std::string& rawRelativePath,
	std::unordered_set<std::string>& usedRelativePaths);

void ReserveBundleRelativePaths(std::unordered_set<std::string>& usedRelativePaths);

}  // namespace e2txt
