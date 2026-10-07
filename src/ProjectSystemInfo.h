#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace e2txt {

// EXE 的运行子系统；易模块和 DLL 不使用该属性。
enum class ProjectSubsystem { Unknown, Console, WindowsGui };

// 原生工程类型；未识别的整数值也必须保留，不能映射为控制台。
enum class ProjectType : std::int32_t {
    WindowsExecutable = 0,
    ConsoleExecutable = 1,
    DynamicLibrary = 2,
    Module = 1000,
};

// 系统信息的语义字段及尚未解释的保留数据，与方法体快照无关。
struct ProjectSystemInfo {
    std::int16_t compileMajor = 5;
    std::int16_t compileMinor = 6;
    std::int32_t unknown1 = 1;
    std::int32_t unknown2 = 1;
    std::int32_t unknownType = 0x00070001;
    std::int32_t fileType = 1;
    std::int32_t unknown3 = 0;
    ProjectType projectType = ProjectType::ConsoleExecutable;
    std::array<std::int32_t, 8> reserved = {};
    std::vector<std::uint8_t> extensionBytes;
    bool operator==(const ProjectSystemInfo&) const = default;
};

// 按原生小端格式读写系统信息，并保留扩展字节。
bool DecodeProjectSystemInfo(std::span<const std::uint8_t> bytes, ProjectSystemInfo& info, std::string* error);
std::vector<std::uint8_t> EncodeProjectSystemInfo(const ProjectSystemInfo& info);
// 从原生文件提取系统信息，供旧目录升级使用。
bool ExtractProjectSystemInfo(const std::vector<std::uint8_t>& bytes, std::optional<ProjectSystemInfo>& info, std::string* error);
// 仅 EXE 类型可映射为运行子系统。
ProjectSubsystem GetProjectSubsystem(const ProjectSystemInfo& info);
// 显式子系统设置只改变 EXE 类型；模块、DLL 的冲突设置必须报错。
bool ResolveProjectSystemInfo(const std::optional<ProjectSystemInfo>& stored, ProjectSubsystem subsystem,
    ProjectSystemInfo& result, std::string* error);

} // namespace e2txt
