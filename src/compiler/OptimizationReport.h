#pragma once
#include "CppEmitter.h"
#include <filesystem>
namespace ecompiler {
// 输出确定顺序的优化诊断及未压缩 PE 节大小，写入失败明确返回错误。
bool WriteOptimizationReport(const std::filesystem::path& report, const std::filesystem::path& executable,
    const Program&, const GeneratedSource&, SemanticOptimization, double elapsedMilliseconds, std::string& error, bool optimizeForSize = false);
}
