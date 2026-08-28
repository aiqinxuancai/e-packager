// 黑月源码后端：将原生易代码 PE 转为 OBJ 并按黑月模式链接。
// 黑月编译后端的独立入口声明。
#pragma once

#include <filesystem>

namespace ecompiler {
struct Options;
struct Result;

namespace blackmoon_backend {

bool Compile(
	const std::filesystem::path& inputPath,
	const std::filesystem::path& outputPath,
	const Options& options,
	Result& result);

}  // namespace blackmoon_backend
}  // namespace ecompiler
