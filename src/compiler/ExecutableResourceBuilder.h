#pragma once

#include <filesystem>
#include <string>
#include <vector>

namespace e2txt { struct ProjectBundle; }

namespace ecompiler {

// 编译产物资源；图标 ID 101 也用于生成的窗口类默认图标。
struct ExecutableResources {
    std::filesystem::path scriptPath;
    std::filesystem::path resourcePath;
    std::filesystem::path iconPath;
};

// 读取独立编译配置并生成资源脚本，不修改工程或原生快照。
bool PrepareExecutableResources(
    const e2txt::ProjectBundle& bundle,
    const std::filesystem::path& configPath,
    const std::filesystem::path& iconOverride,
    const std::filesystem::path& outputPath,
    bool buildDll,
    ExecutableResources& resources,
    std::string& error);

} // namespace ecompiler
