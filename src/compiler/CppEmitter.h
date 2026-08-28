#pragma once

#include "CompilerModel.h"

#include <cstddef>
#include <set>
#include <string>
#include <vector>

namespace ecompiler {

// 生成独立 Win32 编译单元时收集的支持库链接需求。
struct GeneratedSource {
	struct ExportedFunction {
		std::string name;
		std::string symbol;
		bool usesCdecl = false;
		std::size_t stackBytes = 0;
	};
	struct ImportedFunction {
		std::size_t commandIndex = 0;
		std::string moduleName;
		std::string entryName;
		std::string symbol;
		bool usesCdecl = false;
		std::size_t stackBytes = 0;
	};
	std::string text;
	std::set<std::size_t> reachableLibraries;
	std::vector<std::string> exportedNames;
	std::vector<ExportedFunction> exports;
	std::vector<ImportedFunction> imports;
	std::size_t reachableMethodCount = 0;
	std::size_t reachableCommandCount = 0;
};

// 将语义模型绑定到 FNE 的精确符号并生成可由 VC 编译的 C++ 源码。
bool EmitCppSource(const Program& program, GeneratedSource& outSource, std::string& outError);

}  // namespace ecompiler
