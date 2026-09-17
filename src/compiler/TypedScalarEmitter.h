#pragma once
#include "SemanticOptimization.h"
#include <map>
#include <string>

namespace ecompiler {
// 标量方法整体降级或整体生成；边界包装保留 Value 的原有转换语义。
struct TypedScalarOutput {
    std::map<std::size_t, std::string> definitions;
    std::map<std::size_t, std::string> rejected;
    std::string declarations;
};
TypedScalarOutput GenerateTypedScalarMethods(const Program&, const OptimizationAnalysis&);
}
