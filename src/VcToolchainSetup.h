#pragma once

#include <filesystem>
#include <string>
#include <vector>

// VC 工具链辅助：探测 VS2022/VS2026，确认后安装 VS2026 Build Tools。
namespace vc_toolchain_setup {
// 使用官方 vswhere 查询自定义安装位置；失败时由调用方继续其它探测。
std::vector<std::filesystem::path> FindVisualStudioInstallations();
// 识别工具链缺失，不包含用户显式指定的无效路径。
bool IsMissingToolchain(const std::string& error);
// 仅在用户确认后调用：安装 VC 工具和 SDK，不安装 IDE、不自动重启。
bool InstallBuildTools(std::string& error);
}
