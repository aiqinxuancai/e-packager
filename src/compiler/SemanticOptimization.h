#pragma once
// 语义优化选项与分析结果；保留原因用于解释保守边界。
#include "CompilerModel.h"
#include <functional>
#include <map>
#include <set>
namespace ecompiler {
enum class SemanticOptimization { Baseline, Reachable, Typed };
// 调用类别来自同一绑定器，分析与生成不得再次各自解析目标。
enum class SemanticCallKind { External, Owned, Unqualified, Qualified, Member };
struct BoundCall {
    const Method* target = nullptr;
    bool virtualDispatch = false;
    bool nativeBoundary = false;
    SemanticCallKind kind = SemanticCallKind::External;
};
// 原生边界位置用于解释整类保留；typeCode 非零时表示类型生命周期。
struct NativeBoundarySite {
    std::size_t methodId = static_cast<std::size_t>(-1);
    std::size_t sourceLine = 0;
    std::uint32_t typeCode = 0;
    std::string reason;
    std::string target;
};
struct OptimizationAnalysis {
    std::set<std::size_t> reachable;
    std::set<std::size_t> callbacks;
    std::set<std::uint32_t> instantiated;
    std::map<std::size_t, std::set<std::string>> reasons;
    std::map<std::size_t, std::set<std::size_t>> edges;
    std::map<const e2txt::SourceExpressionNode*, BoundCall> calls;
    std::vector<NativeBoundarySite> nativeBoundaries;
    bool opaqueNativeAccess = false;
};
using BindOptimizationCall = std::function<BoundCall(const Method&, const e2txt::SourceExpressionNode&)>;
OptimizationAnalysis AnalyzeSemanticReachability(const Program&, const BindOptimizationCall&);
}
