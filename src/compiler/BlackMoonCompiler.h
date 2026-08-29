// 黑月源码编译方式：将原生易代码 PE 转为 OBJ 并按黑月模式链接。
// 黑月编译方式的独立入口声明。
#pragma once

#include <filesystem>

namespace ecompiler {
struct Options;
struct Result;

namespace blackmoon_compiler {

bool Compile(
	const std::filesystem::path& inputPath,
	const std::filesystem::path& outputPath,
	const Options& options,
	Result& result);

}  // namespace blackmoon_compiler
}  // namespace ecompiler
