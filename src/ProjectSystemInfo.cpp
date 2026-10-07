#include "ProjectSystemInfoCodec.h"
#include "e2txt.h"

#include <bit>
#include <limits>
#include <stdexcept>
#include <type_traits>

namespace e2txt {
namespace {

template<class T>
T ReadInteger(std::span<const std::uint8_t> bytes, size_t& offset)
{
    using U = std::make_unsigned_t<T>;
    U value = 0;
    for (size_t i = 0; i < sizeof(T); ++i) value |= static_cast<U>(bytes[offset++]) << (i * 8);
    return std::bit_cast<T>(value);
}

template<class T>
void WriteInteger(std::vector<std::uint8_t>& bytes, T value)
{
    auto bits = std::bit_cast<std::make_unsigned_t<T>>(value);
    for (size_t i = 0; i < sizeof(T); ++i) bytes.push_back(static_cast<std::uint8_t>(bits >> (i * 8)));
}

template<class T>
T JsonInteger(const nlohmann::json& value)
{
    if (!value.is_number_integer()) throw std::runtime_error("integer_expected");
    if (value.is_number_unsigned() && value.get<std::uint64_t>() > static_cast<std::uint64_t>((std::numeric_limits<T>::max)()))
        throw std::runtime_error("integer_out_of_range");
    const auto number = value.get<std::int64_t>();
    if (number < (std::numeric_limits<T>::min)() || number > (std::numeric_limits<T>::max)())
        throw std::runtime_error("integer_out_of_range");
    return static_cast<T>(number);
}

} // namespace

bool DecodeProjectSystemInfo(std::span<const std::uint8_t> bytes, ProjectSystemInfo& info, std::string* error)
{
    if (bytes.size() < 60) {
        if (error) *error = "system_info_section_too_small";
        return false;
    }
    ProjectSystemInfo result;
    size_t offset = 0;
    result.compileMajor = ReadInteger<std::int16_t>(bytes, offset);
    result.compileMinor = ReadInteger<std::int16_t>(bytes, offset);
    result.unknown1 = ReadInteger<std::int32_t>(bytes, offset);
    result.unknown2 = ReadInteger<std::int32_t>(bytes, offset);
    result.unknownType = ReadInteger<std::int32_t>(bytes, offset);
    result.fileType = ReadInteger<std::int32_t>(bytes, offset);
    result.unknown3 = ReadInteger<std::int32_t>(bytes, offset);
    result.projectType = static_cast<ProjectType>(ReadInteger<std::int32_t>(bytes, offset));
    for (auto& value : result.reserved) value = ReadInteger<std::int32_t>(bytes, offset);
    result.extensionBytes.assign(bytes.begin() + offset, bytes.end());
    info = std::move(result);
    return true;
}

std::vector<std::uint8_t> EncodeProjectSystemInfo(const ProjectSystemInfo& info)
{
    std::vector<std::uint8_t> bytes;
    bytes.reserve(60 + info.extensionBytes.size());
    WriteInteger(bytes, info.compileMajor);
    WriteInteger(bytes, info.compileMinor);
    WriteInteger(bytes, info.unknown1);
    WriteInteger(bytes, info.unknown2);
    WriteInteger(bytes, info.unknownType);
    WriteInteger(bytes, info.fileType);
    WriteInteger(bytes, info.unknown3);
    WriteInteger(bytes, static_cast<std::int32_t>(info.projectType));
    for (const auto value : info.reserved) WriteInteger(bytes, value);
    bytes.insert(bytes.end(), info.extensionBytes.begin(), info.extensionBytes.end());
    return bytes;
}

bool ExtractProjectSystemInfo(const std::vector<std::uint8_t>& bytes, std::optional<ProjectSystemInfo>& info, std::string* error)
{
    info.reset();
    std::vector<NativeSectionSnapshot> sections;
    if (!CaptureNativeSectionSnapshots(bytes, sections, error)) return false;
    for (const auto& section : sections) {
        if (section.key != 0x02007319u) continue;
        ProjectSystemInfo result;
        if (!DecodeProjectSystemInfo(section.data, result, error)) return false;
        info = std::move(result);
        break;
    }
    return true;
}

ProjectSubsystem GetProjectSubsystem(const ProjectSystemInfo& info)
{
    if (info.projectType == ProjectType::WindowsExecutable) return ProjectSubsystem::WindowsGui;
    if (info.projectType == ProjectType::ConsoleExecutable) return ProjectSubsystem::Console;
    return ProjectSubsystem::Unknown;
}

bool ResolveProjectSystemInfo(const std::optional<ProjectSystemInfo>& stored, const ProjectSubsystem subsystem,
    ProjectSystemInfo& result, std::string* error)
{
    result = stored.value_or(ProjectSystemInfo{});
    if (subsystem == ProjectSubsystem::Unknown) return true;
    if (GetProjectSubsystem(result) == ProjectSubsystem::Unknown) {
        if (error) *error = "project_subsystem_not_applicable: compileType=" + std::to_string(static_cast<std::int32_t>(result.projectType));
        return false;
    }
    result.projectType = subsystem == ProjectSubsystem::WindowsGui
        ? ProjectType::WindowsExecutable : ProjectType::ConsoleExecutable;
    return true;
}

nlohmann::json ProjectSystemInfoToJson(const ProjectSystemInfo& info)
{
    return {
        {"compileMajor", info.compileMajor}, {"compileMinor", info.compileMinor},
        {"unknown1", info.unknown1}, {"unknown2", info.unknown2}, {"unknownType", info.unknownType},
        {"fileType", info.fileType}, {"unknown3", info.unknown3},
        {"compileType", static_cast<std::int32_t>(info.projectType)},
        {"reserved", info.reserved}, {"extensionBytes", info.extensionBytes},
    };
}

bool ProjectSystemInfoFromJson(const nlohmann::json& json, ProjectSystemInfo& info, std::string* error)
{
    try {
        ProjectSystemInfo result;
        result.compileMajor = JsonInteger<std::int16_t>(json.at("compileMajor"));
        result.compileMinor = JsonInteger<std::int16_t>(json.at("compileMinor"));
        result.unknown1 = JsonInteger<std::int32_t>(json.at("unknown1"));
        result.unknown2 = JsonInteger<std::int32_t>(json.at("unknown2"));
        result.unknownType = JsonInteger<std::int32_t>(json.at("unknownType"));
        result.fileType = JsonInteger<std::int32_t>(json.at("fileType"));
        result.unknown3 = JsonInteger<std::int32_t>(json.at("unknown3"));
        result.projectType = static_cast<ProjectType>(JsonInteger<std::int32_t>(json.at("compileType")));
        const auto& reserved = json.at("reserved");
        if (!reserved.is_array() || reserved.size() != result.reserved.size()) throw std::runtime_error("reserved_size_invalid");
        for (size_t i = 0; i < result.reserved.size(); ++i) result.reserved[i] = JsonInteger<std::int32_t>(reserved[i]);
        const auto& extension = json.at("extensionBytes");
        if (!extension.is_array()) throw std::runtime_error("extension_bytes_invalid");
        for (const auto& byte : extension) result.extensionBytes.push_back(JsonInteger<std::uint8_t>(byte));
        info = std::move(result);
        return true;
    }
    catch (const std::exception& exception) {
        if (error) *error = "project_system_info_invalid: " + std::string(exception.what());
        return false;
    }
}

} // namespace e2txt
