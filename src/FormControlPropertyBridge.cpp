#include "FormControlPropertyBridge.h"
#include "PathHelper.h"

#include <Windows.h>
#include <atomic>
#include <fstream>
#include <mutex>
#include <unordered_map>

namespace e2txt {
namespace {
using json = nlohmann::json;

json ReadMessage(const std::filesystem::path& path)
{
    std::ifstream input(path, std::ios::binary);
    return json::from_cbor(input);
}

void WriteMessage(const std::filesystem::path& path, const json& value)
{
    const auto bytes = json::to_cbor(value);
    std::ofstream output(path, std::ios::binary);
    output.write(reinterpret_cast<const char*>(bytes.data()), bytes.size());
    if (!output) throw std::runtime_error("property_worker_write_failed");
}
}

int RunFormControlWorker(const std::filesystem::path& requestPath, const std::filesystem::path& responsePath)
{
#ifdef _WIN64
    return 2;
#else
    json response;
    try {
        const auto request = ReadMessage(requestPath);
        std::vector<std::filesystem::path> directories;
        for (const auto& directory : request.at("directories"))
            directories.push_back(Utf8PathToPath(directory.get<std::string>()));
        FormControlPropertyCodec codec(request.at("source"),
            request.at("libraries").get<std::vector<FormControlSupportLibrary>>(),
            directories, request.at("restrict"));
        std::string error;
        const auto type = request.at("type").get<std::int32_t>();
        const std::string operation = request.at("operation");
        if (operation == "events") {
            std::vector<FormControlEventDefinition> events;
            response["ok"] = codec.ReadEvents(type, events, &error);
            response["events"] = events;
        } else if (operation == "decode") {
            std::vector<FormControlPropertyValue> values;
            FormControlPropertySemanticData semantic;
            response["ok"] = codec.Decode(type, request.at("data"), request.at("form"),
                request.at("unit"), values, &error, &semantic);
            response["values"] = values;
            response["semantic"] = semantic;
        } else if (operation == "apply") {
            std::vector<std::uint8_t> data;
            response["ok"] = codec.Apply(type, request.at("data"), request.at("form"),
                request.at("unit"), request.at("attributes"), data, &error,
                request.at("children").get<std::vector<FormControlPropertyXmlNode>>());
            response["data"] = data;
        } else {
            throw std::runtime_error("property_worker_operation_invalid");
        }
        response["error"] = error;
    } catch (const std::exception& exception) {
        response = {{"ok", false}, {"error", exception.what()}};
    }
    WriteMessage(responsePath, response);
    return 0;
#endif
}

bool InvokeFormControlWorker(const json& request, json& response, std::string* error)
{
    // 缓存仅在当前进程有效，键包含库路径、组件类型与完整输入属性。
    static std::mutex mutex;
    static std::unordered_map<std::string, json> cache;
    const auto encoded = json::to_cbor(request);
    const std::string key(encoded.begin(), encoded.end());
    std::lock_guard lock(mutex);
    if (const auto found = cache.find(key); found != cache.end()) {
        response = found->second;
        return true;
    }
    wchar_t executable[MAX_PATH];
    GetModuleFileNameW(nullptr, executable, MAX_PATH);
    const auto base = std::filesystem::path(executable).parent_path();
    std::filesystem::path worker;
    for (const auto& candidate : {base / L"e-packager-x86.exe",
         base.parent_path().parent_path() / L"Win32" / base.filename() / L"e-packager.exe"}) {
        if (std::filesystem::is_regular_file(candidate)) { worker = candidate; break; }
    }
    if (worker.empty()) {
        if (error) *error = "property_worker_missing: place Win32 e-packager-x86.exe beside the x64 executable";
        return false;
    }
    static std::atomic<unsigned> serial = 0;
    const auto directory = std::filesystem::temp_directory_path() /
        (L"e-packager-property-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(++serial));
    std::filesystem::create_directory(directory);
    struct Cleanup {
        std::filesystem::path directory;
        ~Cleanup() { std::error_code ignored; std::filesystem::remove_all(directory, ignored); }
    } cleanup{directory};
    const auto input = directory / L"request.cbor", output = directory / L"response.cbor";
    WriteMessage(input, request);
    auto command = L"\"" + worker.wstring() + L"\" --form-control-worker \"" + input.wstring() +
        L"\" \"" + output.wstring() + L"\"";
    STARTUPINFOW startup{sizeof(startup)};
    PROCESS_INFORMATION process{};
    if (!CreateProcessW(worker.c_str(), command.data(), nullptr, nullptr, FALSE,
        CREATE_NO_WINDOW, nullptr, nullptr, &startup, &process)) {
        if (error) *error = "property_worker_start_failed: " + std::to_string(GetLastError());
        return false;
    }
    CloseHandle(process.hThread);
    const DWORD wait = WaitForSingleObject(process.hProcess, 30000);
    if (wait != WAIT_OBJECT_0) {
        TerminateProcess(process.hProcess, 1);
        WaitForSingleObject(process.hProcess, 5000);
    }
    DWORD code = 1;
    GetExitCodeProcess(process.hProcess, &code);
    CloseHandle(process.hProcess);
    if (code != 0 || !std::filesystem::exists(output)) {
        if (error) *error = "property_worker_failed: " + std::to_string(code);
        return false;
    }
    response = ReadMessage(output);
    if (!response.value("ok", false)) {
        if (error) *error = response.value("error", "property_worker_failed");
        return false;
    }
    cache.emplace(key, response);
    return true;
}
} // namespace e2txt
