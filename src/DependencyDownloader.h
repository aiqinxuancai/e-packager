#pragma once

#include "compiler/CompilerTarget.h"

#include <filesystem>
#include <string>

namespace dependency_download {

// 为源码直接编译准备一个缺失的支持库依赖。
// 当前内置发布源为 BlackMoonModernCore；其他支持库应通过未来的包源
// 清单注册，而不是在编译器中加入按函数名区分的特殊分支。
bool EnsureDependency(
	const std::string& dependencyFileName,
	ecompiler::TargetArchitecture architecture,
	std::filesystem::path& outSearchRoot,
	std::string& outError);

}  // namespace dependency_download
