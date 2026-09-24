#include "VcToolchainSetup.h"

#include "PathHelper.h"
#include "../thirdparty/json.hpp"

#include <Windows.h>
#include <wincrypt.h>
#include <array>

#pragma comment(lib, "Crypt32.lib")

namespace vc_toolchain_setup {
namespace {

// 合并捕获日志，避免安装程序污染 --diagnostics json 的标准输出。
bool RunHiddenProcess(const std::filesystem::path& executable, const std::wstring& arguments,
    DWORD timeout, DWORD& exitCode, std::string& output)
{
    output.clear();
    SECURITY_ATTRIBUTES security{sizeof(security), nullptr, TRUE};
    HANDLE reader = nullptr, writer = nullptr;
    if (!CreatePipe(&reader, &writer, &security, 0)) return false;
    if (!SetHandleInformation(reader, HANDLE_FLAG_INHERIT, 0)) {
        CloseHandle(reader); CloseHandle(writer); return false;
    }
    HANDLE input = CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
        &security, OPEN_EXISTING, 0, nullptr);
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    startup.dwFlags = STARTF_USESTDHANDLES;
    startup.hStdInput = input;
    startup.hStdOutput = writer;
    startup.hStdError = writer;
    PROCESS_INFORMATION process{};
    std::wstring command = L"\"" + executable.wstring() + L"\" " + arguments;
    const BOOL started = CreateProcessW(executable.c_str(), command.data(), nullptr, nullptr,
        TRUE, CREATE_NO_WINDOW, nullptr, nullptr, &startup, &process);
    CloseHandle(writer);
    if (input != INVALID_HANDLE_VALUE) CloseHandle(input);
    if (!started) { CloseHandle(reader); return false; }
    CloseHandle(process.hThread);
    const ULONGLONG start = GetTickCount64();
    bool ok = true;
    const auto drain = [&] {
        DWORD available = 0;
        while (PeekNamedPipe(reader, nullptr, 0, nullptr, &available, nullptr) && available) {
            std::array<char, 4096> buffer{}; DWORD count = 0;
            if (!ReadFile(reader, buffer.data(), static_cast<DWORD>(buffer.size()), &count, nullptr) || !count) break;
            if (output.size() < 4 * 1024 * 1024) output.append(buffer.data(), count);
        }
    };
    for (;;) {
        drain();
        if (WaitForSingleObject(process.hProcess, 100) == WAIT_OBJECT_0) { drain(); break; }
        if (timeout != INFINITE && GetTickCount64() - start >= timeout) {
            TerminateProcess(process.hProcess, ERROR_TIMEOUT);
            WaitForSingleObject(process.hProcess, 5000);
            ok = false; break;
        }
    }
    if (!GetExitCodeProcess(process.hProcess, &exitCode)) ok = false;
    CloseHandle(process.hProcess); CloseHandle(reader);
    return ok;
}

// 固定脚本以 EncodedCommand 传递，避免路径被解释为 shell 代码。
const wchar_t* InstallScript = LR"PS(
$ErrorActionPreference = 'Stop'
[Console]::OutputEncoding = New-Object System.Text.UTF8Encoding($false)
$ProgressPreference = 'SilentlyContinue'
$stage = 'prepare'
$work = Join-Path ([IO.Path]::GetTempPath()) ('e-packager-vc-' + [Guid]::NewGuid().ToString('N'))
try {
    New-Item -ItemType Directory -Path $work | Out-Null
    $installer = Join-Path $work 'vs_BuildTools.exe'
    $stage = 'download'
    [Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12
    Invoke-WebRequest -UseBasicParsing -Uri 'https://aka.ms/vs/stable/vs_BuildTools.exe' -OutFile $installer
    $stage = 'signature'
    $signature = Get-AuthenticodeSignature -LiteralPath $installer
    if ($signature.Status -ne 'Valid' -or $signature.SignerCertificate.Subject -notmatch '(^|,\s*)O=Microsoft Corporation(,|$)') {
        throw 'Microsoft signature verification failed'
    }
    # stable 将来可能切换主版本，必须确认仍为 VS2026。
    $version = [Diagnostics.FileVersionInfo]::GetVersionInfo($installer)
    if ($version.ProductMajorPart -ne 18) { throw 'Expected Visual Studio 2026 (18.x) bootstrapper' }
    $stage = 'install'
    $arguments = @('--quiet', '--wait', '--norestart', '--add',
        'Microsoft.VisualStudio.Component.VC.Tools.x86.x64', '--add',
        'Microsoft.VisualStudio.Component.Windows11SDK.26100')
    # 已有 VS2026 Build Tools 时补充组件；其它 VS 产品不作修改。
    $vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
    if (Test-Path -LiteralPath $vswhere) {
        $existing = & $vswhere -latest -products Microsoft.VisualStudio.Product.BuildTools -version '[18.0,19.0)' -property installationPath
        if ($LASTEXITCODE -ne 0) { throw 'vswhere failed' }
        if ($existing) { $arguments = @('modify') + $arguments + @('--installPath', ('"' + $existing.Trim() + '"')) }
    }
    $process = Start-Process -FilePath $installer -ArgumentList $arguments -Verb RunAs -WindowStyle Hidden -Wait -PassThru
    $code = $process.ExitCode
    if ($code -eq 3010 -or $code -eq 1641) {
        [Console]::Error.WriteLine('vc_toolchain_install_reboot_required:' + $code)
    } elseif ($code -ne 0) {
        [Console]::Error.WriteLine('vc_toolchain_installer_exit:' + $code + '; logs: %TEMP%\dd_*')
    }
    exit $code
} catch {
    [Console]::Error.WriteLine('vc_toolchain_install_' + $stage + '_failed:' + $_.Exception.Message)
    exit 1
} finally {
    # 只清理本次下载文件和空目录，避免递归删除临时目录的其它内容。
    if ($installer -and (Test-Path -LiteralPath $installer)) { Remove-Item -LiteralPath $installer -Force -ErrorAction SilentlyContinue }
    if (Test-Path -LiteralPath $work) { try { [IO.Directory]::Delete($work, $false) } catch {} }
}
)PS";

} // namespace

std::vector<std::filesystem::path> FindVisualStudioInstallations()
{
    wchar_t root[32768]{};
    const DWORD length = GetEnvironmentVariableW(L"ProgramFiles(x86)", root, static_cast<DWORD>(std::size(root)));
    if (!length || length >= std::size(root)) return {};
    const auto executable = std::filesystem::path(root) / L"Microsoft Visual Studio/Installer/vswhere.exe";
    std::string output; DWORD code = 1;
    if (!RunHiddenProcess(executable,
        L"-products * -version \"[17.0,19.0)\" -format json -utf8", 15000, code, output) || code != 0) return {};
    const auto instances = nlohmann::json::parse(output, nullptr, false);
    std::vector<std::filesystem::path> result;
    if (!instances.is_array()) return result;
    for (const auto& instance : instances) {
        if (!instance.is_object()) continue;
        const auto path = instance.find("installationPath");
        if (path != instance.end() && path->is_string()) result.push_back(Utf8PathToPath(path->get<std::string>()));
    }
    return result;
}

bool IsMissingToolchain(const std::string& error)
{
    for (const char* prefix : {"visual_cpp_or_windows_sdk_not_found", "matching_x86_compiler_not_found:",
        "matching_x64_compiler_not_found:", "compiler_include_directory_not_found:",
        "compiler_library_directory_not_found:", "linker_not_found:", "vc_toolchain_incomplete:"}) {
        if (error.starts_with(prefix)) return true;
    }
    return false;
}

bool InstallBuildTools(std::string& error)
{
    error.clear();
    wchar_t system[MAX_PATH]{};
    if (!GetSystemDirectoryW(system, static_cast<UINT>(std::size(system)))) {
        error = "vc_toolchain_powershell_path_failed"; return false;
    }
    const DWORD bytes = static_cast<DWORD>(wcslen(InstallScript) * sizeof(wchar_t));
    DWORD length = 0;
    if (!CryptBinaryToStringW(reinterpret_cast<const BYTE*>(InstallScript), bytes,
        CRYPT_STRING_BASE64 | CRYPT_STRING_NOCRLF, nullptr, &length)) {
        error = "vc_toolchain_script_encode_failed"; return false;
    }
    std::wstring encoded(length, L'\0');
    if (!CryptBinaryToStringW(reinterpret_cast<const BYTE*>(InstallScript), bytes,
        CRYPT_STRING_BASE64 | CRYPT_STRING_NOCRLF, encoded.data(), &length)) {
        error = "vc_toolchain_script_encode_failed"; return false;
    }
    encoded.resize(length);
    std::string output; DWORD code = 1;
    const auto powershell = std::filesystem::path(system) / L"WindowsPowerShell/v1.0/powershell.exe";
    if (!RunHiddenProcess(powershell, L"-NoLogo -NoProfile -NonInteractive -EncodedCommand " + encoded,
        INFINITE, code, output)) {
        error = "vc_toolchain_install_process_failed"; return false;
    }
    if (code != 0) {
        error = output.empty() ? "vc_toolchain_installer_exit:" + std::to_string(code) : output;
        return false;
    }
    return true;
}
} // namespace vc_toolchain_setup
