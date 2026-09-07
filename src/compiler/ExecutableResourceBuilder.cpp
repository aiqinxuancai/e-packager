#include "ExecutableResourceBuilder.h"

#include "../PathHelper.h"
#include "../e2txt.h"
#include "../../thirdparty/json.hpp"

#include <array>
#include <fstream>
#include <iterator>
#include <map>
#include <sstream>
#include <stdexcept>

namespace ecompiler {
namespace {

using json = nlohmann::json;

std::string LocalToUtf8(const std::string& value)
{
    if (value.empty()) return {};
    const int count = MultiByteToWideChar(936, MB_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()), nullptr, 0);
    if (!count) throw std::runtime_error("invalid_project_text_encoding");
    std::wstring wide(count, L'\0');
    MultiByteToWideChar(936, MB_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()), wide.data(), count);
    return WideToUtf8Text(wide);
}

std::string ReadBytes(const std::filesystem::path& path)
{
    std::ifstream input(path, std::ios::binary);
    if (!input) throw std::runtime_error("read_failed:" + PathToUtf8(path));
    std::string bytes{std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
    if (input.bad()) throw std::runtime_error("read_failed:" + PathToUtf8(path));
    return bytes;
}

void WriteBytes(const std::filesystem::path& path, const std::string& bytes)
{
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    output.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    output.close();
    if (!output) throw std::runtime_error("write_failed:" + PathToUtf8(path));
}

std::string QuoteResource(const std::string& text)
{
    std::string result = "\"";
    for (const unsigned char ch : text) {
        switch (ch) {
        case '\\': result += "\\\\"; break;
        case '"': result += "\"\""; break;
        case '\r': result += "\\r"; break;
        case '\n': result += "\\n"; break;
        case '\t': result += "\\t"; break;
        default:
            if (ch < 32) throw std::runtime_error("unsupported_control_character");
            result += static_cast<char>(ch);
        }
    }
    return result + "\"";
}

struct Version {
    std::array<unsigned int, 4> parts{};
    std::string Format(char separator) const
    {
        std::string result;
        for (auto part : parts) {
            if (!result.empty()) result += separator;
            result += std::to_string(part);
        }
        return result;
    }
};

Version ParseVersion(const std::string& value, const std::string& field)
{
    Version version;
    std::size_t index = 0;
    bool digit = false;
    for (const char ch : value) {
        if (ch == '.' && digit && index < 3) {
            ++index;
            digit = false;
        }
        else if (ch >= '0' && ch <= '9') {
            auto& part = version.parts[index];
            part = part * 10 + (ch - '0');
            if (part > 65535) throw std::runtime_error("invalid_version:" + field);
            digit = true;
        }
        else throw std::runtime_error("invalid_version:" + field);
    }
    if (!digit) throw std::runtime_error("invalid_version:" + field);
    return version;
}

void ValidateIcon(const std::string& bytes)
{
    const auto word = [&bytes](std::size_t offset) {
        return static_cast<unsigned int>(static_cast<unsigned char>(bytes[offset])) |
            (static_cast<unsigned int>(static_cast<unsigned char>(bytes[offset + 1])) << 8);
    };
    const auto dword = [&word](std::size_t offset) {
        return static_cast<std::uint32_t>(word(offset)) | (static_cast<std::uint32_t>(word(offset + 2)) << 16);
    };
    if (bytes.size() < 6 || word(0) != 0 || word(2) != 1 || word(4) == 0)
        throw std::runtime_error("invalid_ico_header");
    const std::size_t tableEnd = 6 + 16 * word(4);
    if (tableEnd > bytes.size()) throw std::runtime_error("truncated_ico_directory");
    for (std::size_t entry = 6; entry < tableEnd; entry += 16) {
        const auto size = dword(entry + 8);
        const auto offset = dword(entry + 12);
        if (size == 0 || offset < tableEnd || offset > bytes.size() || size > bytes.size() - offset)
            throw std::runtime_error("invalid_ico_image_bounds");
        HICON icon = CreateIconFromResourceEx(
            reinterpret_cast<PBYTE>(const_cast<char*>(bytes.data() + offset)), size,
            TRUE, 0x00030000, 0, 0, LR_DEFAULTCOLOR);
        if (!icon) throw std::runtime_error("invalid_ico_image");
        DestroyIcon(icon);
    }
}

} // namespace

bool PrepareExecutableResources(
    const e2txt::ProjectBundle& bundle, const std::filesystem::path& configPath,
    const std::filesystem::path& iconOverride, const std::filesystem::path& outputPath,
    bool buildDll, ExecutableResources& resources, std::string& error)
{
    resources = {};
    try {
        json config = json::object();
        if (!configPath.empty()) config = json::parse(ReadBytes(configPath));
        if (!config.is_object()) throw std::runtime_error("config_must_be_object");
        const std::map<std::string, std::string> fields{
            {"fileDescription", "FileDescription"}, {"productName", "ProductName"},
            {"companyName", "CompanyName"}, {"author", "Author"}, {"legalCopyright", "LegalCopyright"},
            {"fileVersion", "FileVersion"}, {"productVersion", "ProductVersion"}
        };
        for (const auto& [key, value] : config.items()) {
            if (key != "icon" && !fields.contains(key)) throw std::runtime_error("unknown_field:" + key);
            if (!value.is_string()) throw std::runtime_error("field_must_be_string:" + key);
            (void)QuoteResource(value.get<std::string>());
        }
        const std::string projectName = bundle.projectName.empty()
            ? PathToUtf8(outputPath.stem()) : LocalToUtf8(bundle.projectName);
        const std::string projectVersion = bundle.versionText.empty() ? "1.0.0.0" : bundle.versionText;
        const auto fileVersion = ParseVersion(config.value("fileVersion", projectVersion), "fileVersion");
        const auto productVersion = ParseVersion(config.value("productVersion", projectVersion), "productVersion");
        config["fileVersion"] = fileVersion.Format('.');
        config["productVersion"] = productVersion.Format('.');
        if (!config.contains("productName")) config["productName"] = projectName;
        if (!config.contains("fileDescription")) config["fileDescription"] = projectName;

        std::string iconBytes;
        std::filesystem::path iconSource = iconOverride;
        if (iconSource.empty() && config.contains("icon")) {
            const auto icon = config["icon"].get<std::string>();
            if (icon.empty()) throw std::runtime_error("empty_icon_path");
            iconSource = configPath.parent_path() / Utf8PathToPath(icon);
        }
        if (!iconSource.empty()) iconBytes = ReadBytes(iconSource);
        else if (bundle.nativeProgramHeader && !bundle.nativeProgramHeader->icon.empty()) {
            const auto& native = bundle.nativeProgramHeader->icon;
            iconBytes.assign(reinterpret_cast<const char*>(native.data()), native.size());
        }
        if (!iconBytes.empty() || !iconSource.empty()) ValidateIcon(iconBytes);

        resources.scriptPath = std::filesystem::absolute(outputPath);
        resources.scriptPath.replace_extension(L".resources.rc");
        resources.resourcePath = resources.scriptPath;
        resources.resourcePath.replace_extension(L".res");
        std::ostringstream script;
        script << "#pragma code_page(65001)\nLANGUAGE 4, 2\n";
        if (!iconBytes.empty()) {
            resources.iconPath = resources.scriptPath;
            resources.iconPath.replace_extension(L".ico");
            WriteBytes(resources.iconPath, iconBytes);
            script << "101 ICON " << QuoteResource(PathToUtf8(resources.iconPath)) << "\n";
        }
        script << "1 VERSIONINFO\n FILEVERSION " << fileVersion.Format(',')
            << "\n PRODUCTVERSION " << productVersion.Format(',')
            << "\n FILEFLAGSMASK 0x3fL\n FILEFLAGS 0\n FILEOS 0x40004L\n FILETYPE "
            << (buildDll ? "2" : "1") << "\n FILESUBTYPE 0\nBEGIN\n"
            << " BLOCK \"StringFileInfo\"\n BEGIN\n  BLOCK \"080404b0\"\n  BEGIN\n";
        for (const auto& [key, resourceName] : fields) {
            if (config.contains(key)) script << "   VALUE " << QuoteResource(resourceName) << ", "
                << QuoteResource(config[key].get<std::string>()) << "\n";
        }
        script << "   VALUE \"OriginalFilename\", " << QuoteResource(PathToUtf8(outputPath.filename()))
            << "\n  END\n END\n BLOCK \"VarFileInfo\"\n BEGIN\n"
            << "  VALUE \"Translation\", 0x0804, 1200\n END\nEND\n";
        std::string text = "\xEF\xBB\xBF";
        for (const char ch : script.str()) {
            if (ch == '\n') text += '\r';
            text += ch;
        }
        WriteBytes(resources.scriptPath, text);
        return true;
    }
    catch (const std::exception& exception) {
        error = "executable_resources_failed:" + std::string(exception.what());
        if (!configPath.empty()) error += ";config=" + PathToUtf8(configPath);
        return false;
    }
}

} // namespace ecompiler
