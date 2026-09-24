[CmdletBinding()]
param([Parameter(Mandatory)][string]$Bootstrapper)

# 使用已下载的 VS2026 Build Tools 引导程序检查脚本；所有安装调用均被替身拦截。
$ErrorActionPreference = 'Stop'
$PSNativeCommandUseErrorActionPreference = $false
$repo = Split-Path -Parent $PSScriptRoot
$root = Join-Path $env:TEMP ('e-packager-vc-test-' + [guid]::NewGuid().ToString('N'))
[IO.Directory]::CreateDirectory($root) | Out-Null
$Bootstrapper = (Resolve-Path -LiteralPath $Bootstrapper).Path
$source = [IO.File]::ReadAllText((Join-Path $repo 'src/VcToolchainSetup.cpp'))
$match = [regex]::Match($source, 'LR"PS\((.*?)\)PS"', 'Singleline')
if (-not $match.Success) { throw 'Installer script not found' }
$script = $match.Groups[1].Value
$powershell = Join-Path $env:SystemRoot 'System32/WindowsPowerShell/v1.0/powershell.exe'

$mocks = @'
param($Fixture, $Case, $Record)
function Invoke-WebRequest {
    param([switch]$UseBasicParsing, $Uri, $OutFile)
    if ($Uri -ne 'https://aka.ms/vs/stable/vs_BuildTools.exe') { throw 'Unexpected download URL' }
    if ($Case -eq 'download') { throw 'simulated network failure' }
    if ($Case -eq 'version') { Copy-Item -LiteralPath (Join-Path $env:SystemRoot 'System32/cmd.exe') -Destination $OutFile }
    else { Copy-Item -LiteralPath $Fixture -Destination $OutFile }
}
function Get-AuthenticodeSignature {
    param($LiteralPath)
    if ($Case -eq 'signature') { return [pscustomobject]@{ Status='NotSigned'; SignerCertificate=$null } }
    if ($Case -eq 'publisher') { return [pscustomobject]@{ Status='Valid'; SignerCertificate=[pscustomobject]@{Subject='O=Other Company'} } }
    if ($Case -eq 'version') { return [pscustomobject]@{ Status='Valid'; SignerCertificate=[pscustomobject]@{Subject='O=Microsoft Corporation'} } }
    Microsoft.PowerShell.Security\Get-AuthenticodeSignature -LiteralPath $LiteralPath
}
function Start-Process {
    param($FilePath, $ArgumentList, $Verb, $WindowStyle, [switch]$Wait, [switch]$PassThru)
    if ($Verb -ne 'RunAs' -or $WindowStyle -ne 'Hidden' -or -not $Wait -or -not $PassThru) { throw 'Incorrect elevation/wait options' }
    $ArgumentList | ConvertTo-Json | Set-Content -LiteralPath $Record
    if ($Case -eq 'uac') { throw 'simulated UAC cancellation' }
    $code = switch ($Case) { 'reboot' {3010} 'failure' {1603} default {0} }
    [pscustomobject]@{ ExitCode=$code }
}
'@

foreach ($case in @('success', 'download', 'signature', 'publisher', 'version', 'uac', 'reboot', 'failure')) {
    $path = Join-Path $root "$case.ps1"
    $record = Join-Path $root "$case.json"
    [IO.File]::WriteAllText($path, (($mocks + "`n" + $script) -replace '\r?\n', "`r`n"), [Text.UTF8Encoding]::new($true))
    $output = & $powershell -NoLogo -NoProfile -NonInteractive -ExecutionPolicy Bypass -File $path $Bootstrapper $case $record 2>&1 | Out-String
    $code = $LASTEXITCODE
    $expected = switch ($case) { 'success' {0} 'reboot' {3010} 'failure' {1603} default {1} }
    if ($code -ne $expected) { throw "$case expected exit $expected, got $code`n$output" }
    if ($case -in @('download', 'signature', 'publisher', 'version')) {
        if (Test-Path -LiteralPath $record) { throw "$case unexpectedly launched installer" }
    } else {
        $arguments = Get-Content -Raw -LiteralPath $record | ConvertFrom-Json
        foreach ($required in @('--quiet', '--wait', '--norestart',
            'Microsoft.VisualStudio.Component.VC.Tools.x86.x64',
            'Microsoft.VisualStudio.Component.Windows11SDK.26100')) {
            if ($arguments -notcontains $required) { throw "Missing installer option $required" }
        }
        if (@($arguments | Where-Object { $_ -eq '--add' }).Count -ne 2 -or
            $arguments -contains '--includeRecommended' -or $arguments -contains '--includeOptional') {
            throw 'Unexpected installation scope'
        }
    }
    if ($case -eq 'reboot' -and -not $output.Contains('vc_toolchain_install_reboot_required')) { throw 'Missing reboot diagnostic' }
    if ($case -eq 'failure' -and -not $output.Contains('vc_toolchain_installer_exit:1603')) { throw 'Missing installer failure diagnostic' }
    Write-Output "PASS installer $case (no real installation)"
}
