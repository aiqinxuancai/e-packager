#pragma once

namespace e2txt {

struct ProjectBundle;
struct SourcePreflightReport;

// 对工程源码执行跨页面符号、表达式、调用和流程语义检查。
void ValidateProjectBundleSemantics(
	const ProjectBundle& bundle,
	SourcePreflightReport& report);

}  // namespace e2txt
