#pragma once

#include <cstddef>
#include <string>
#include <vector>

namespace e2txt {

struct ProjectBundle;

// 源码预检诊断。
struct SourcePreflightDiagnostic {
	std::string filePath;
	size_t line = 0;
	std::string code;
	std::string message;
};

// 源码预检结果。
struct SourcePreflightReport {
	size_t checkedFiles = 0;
	size_t checkedLines = 0;
	std::vector<SourcePreflightDiagnostic> errors;
	std::vector<SourcePreflightDiagnostic> warnings;

	bool IsValid() const;
};

// 对目录工程中的声明、基础语法和可确定类型关系执行快速预检。
SourcePreflightReport ValidateProjectBundleSource(const ProjectBundle& bundle);

// 生成适合命令行显示的预检摘要或错误详情。
std::string FormatSourcePreflightReport(const SourcePreflightReport& report);

// 生成供 IDE、脚本和其他工具消费的稳定 JSON 诊断报告。
std::string FormatSourcePreflightReportJson(
	const SourcePreflightReport& report,
	const ProjectBundle* bundle = nullptr);

}  // namespace e2txt
