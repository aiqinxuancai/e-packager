param(
    [ValidateSet("speed","size")][string]$CodegenOpt = "speed",
    [string]$Packager = "$PSScriptRoot/../bin/Win32/Release/e-packager.exe",
    [string]$CoreDirectory = '',
    [string]$EDirectory = 'C:\Users\aiqin\OneDrive\e5.6',
    [string]$OutputRoot = "$PSScriptRoot/../temp/semantic-optimization-$([guid]::NewGuid().ToString('N'))",
    [ValidateSet('x86','x64')][string]$Architecture = 'x86'
)
$ErrorActionPreference = 'Stop'
$OutputRoot = [IO.Path]::GetFullPath($OutputRoot)
$workspace = Join-Path $OutputRoot 'workspace'
& $Packager unpack "$PSScriptRoot/../eproj/e-console-exe-new-proj.e" $workspace --main-only
if ($LASTEXITCODE -ne 0) { throw 'fixture unpack failed' }
function Write-Source([string]$Name, [string]$Content) {
    [IO.File]::WriteAllText((Join-Path $workspace "src/$Name.txt"),
        ($Content -replace '\r?\n', "`r`n"), [Text.UTF8Encoding]::new($true))
}
Write-Source '程序集1' @'
.版本 2
.程序集 程序集1
.子程序 _启动子程序, 整数型
.局部变量 对象, 活类
.局部变量 派生, 派生类
.如果真 (对象.取值 () ≠ 42 或 派生.虚调用 () ≠ 43)
    返回 (1)
.如果真结束
.如果真 (求和 (10) ≠ 55 或 递归 (5) ≠ 120)
    返回 (2)
.如果真结束
.如果真 (动态返回 () ≠ 3.5)
    返回 (3)
.如果真结束
.如果真 (整除 () ≠ 3)
    返回 (4)
.如果真结束
.如果真 (字节存储 () ≠ 300)
    返回 (5)
.如果真结束
.如果真 (循环分支 () ≠ 15)
    返回 (6)
.如果真结束
返回 (0)
.子程序 循环分支, 整数型
.局部变量 i, 整数型
.局部变量 总和, 整数型
.计次循环首 (3, i)
    总和 ＝ 总和 ＋ i
.计次循环尾 ()
.变量循环首 (5, 1, -2, i)
    总和 ＝ 总和 ＋ i
.变量循环尾 ()
.循环判断首 ()
    总和 ＝ 总和 － 1
.循环判断尾 (总和 ＞ 15)
.判断开始 (总和 ＝ 14)
    总和 ＝ 15
.默认
    总和 ＝ 99
.判断结束
返回 (总和)
.子程序 求和, 整数型
.参数 上限, 整数型
.局部变量 i, 整数型
.局部变量 合计, 整数型
.判断循环首 (i ＜ 上限)
    i ＝ i ＋ 1
    合计 ＝ 合计 ＋ i
.判断循环尾 ()
返回 (合计)
.子程序 递归, 整数型
.参数 n, 整数型
.局部变量 结果, 整数型
.如果真 (n ≤ 1)
    返回 (1)
.如果真结束
结果 ＝ n × 递归 (n － 1)
返回 (结果)
.子程序 动态返回, 整数型
返回 (3.5)
.子程序 整除, 整数型
返回 (7 \ 2)
.子程序 字节存储, 整数型
.局部变量 字节, 字节型
.局部变量 结果, 整数型
字节 ＝ 300
结果 ＝ 字节
返回 (结果)
.子程序 无用函数, 整数型
返回 (999)
'@
Write-Source '活类' @'
.版本 2
.程序集 活类, <对象>
.子程序 _初始化
.子程序 取值, 整数型, 公开
返回 (42)
.子程序 虚调用, 整数型, 公开
返回 (取值 ())
.子程序 无用方法, 整数型, 公开
返回 (99)
'@
Write-Source '派生类' @'
.版本 2
.程序集 派生类, 活类
.子程序 取值, 整数型, 公开
返回 (活类.取值 () ＋ 1)
'@
Write-Source '死类' @'
.版本 2
.程序集 死类, <对象>
.子程序 _初始化
.子程序 无用方法, 整数型, 公开
返回 (123)
'@
$extra = @()
if ($CoreDirectory) { $extra = @('--blackmoon-core-dir', $CoreDirectory) }
$reports = @{}
foreach ($mode in @('baseline','reachable','typed')) {
    $exe = Join-Path $OutputRoot "$mode.exe"
    $report = Join-Path $OutputRoot "$mode.json"
    & $Packager compile $workspace $exe --arch $Architecture --e-dir $EDirectory --codegen-opt $CodegenOpt --semantic-opt=$mode --optimization-report=$report @extra
    if ($LASTEXITCODE -ne 0) { throw "$mode compilation failed" }
    $process = Start-Process -FilePath $exe -WorkingDirectory $OutputRoot -WindowStyle Hidden -PassThru
    try {
        if (-not $process.WaitForExit(15000)) { Stop-Process -Id $process.Id; throw "$mode timeout" }
        if ($process.ExitCode -ne 0) { throw "$mode returned $($process.ExitCode)" }
    } finally { $process.Dispose() }
    $reports[$mode] = Get-Content $report -Raw | ConvertFrom-Json
}
if ($Architecture -eq 'x86' -and $reports.reachable.reachable_methods -ge $reports.baseline.reachable_methods) { throw 'dead class methods were not removed' }
if ($reports.typed.typed_methods -lt 4) { throw 'scalar methods were not generated' }
if ($reports.typed.native_wrappers -ne 0) { throw 'pure semantic class retained native wrappers' }
foreach ($name in @('动态返回','整除')) {
    $method = $reports.typed.methods | Where-Object name -eq $name
    if ($method.typed -or -not $method.typed_rejection) { throw "$name must have an explicit rejection" }
}
$reports.GetEnumerator() | ForEach-Object { [pscustomobject]@{ Mode=$_.Key; Methods=$_.Value.reachable_methods; Typed=$_.Value.typed_methods; Bytes=$_.Value.executable_bytes } } | Format-Table

# 原生回调/传址会启用保守闭包；与纯语义裁剪测试使用不同入口。
if ($Architecture -eq 'x86') {
    Write-Source '程序集1' @'
.版本 2
.程序集 程序集1
.子程序 _启动子程序, 整数型
.局部变量 n, 整数型
n ＝ 7
参照 (n)
.如果真 (n ≠ 42)
    返回 (1)
.如果真结束
.如果真 (调用回调 (&回调, 1, 2, 3, 4) ≠ 10)
    返回 (2)
.如果真结束
返回 (0)
.子程序 参照
.参数 n, 整数型, 参考
n ＝ 42
.子程序 回调, 整数型
.参数 a, 整数型
.参数 b, 整数型
.参数 c, 整数型
.参数 d, 整数型
.局部变量 结果, 整数型
结果 ＝ a ＋ b ＋ c ＋ d
返回 (结果)
'@
    Write-Source '.DLL声明' @'
.版本 2
.DLL命令 调用回调, 整数型, "user32.dll", "CallWindowProcA"
    .参数 proc, 子程序指针
    .参数 a, 整数型
    .参数 b, 整数型
    .参数 c, 整数型
    .参数 d, 整数型
'@
    foreach ($mode in @('baseline','reachable','typed')) {
        $exe = Join-Path $OutputRoot "native-$mode.exe"
        $report = Join-Path $OutputRoot "native-$mode.json"
        & $Packager compile $workspace $exe --arch x86 --e-dir $EDirectory --codegen-opt $CodegenOpt --semantic-opt $mode --optimization-report $report @extra
        if ($LASTEXITCODE -ne 0) { throw "native $mode compilation failed" }
        $process = Start-Process -FilePath $exe -WorkingDirectory $OutputRoot -WindowStyle Hidden -PassThru
        try {
            if (-not $process.WaitForExit(15000)) { Stop-Process -Id $process.Id; throw "native $mode timeout" }
            if ($process.ExitCode -ne 0) { throw "native $mode returned $($process.ExitCode)" }
        } finally { $process.Dispose() }
        $native = Get-Content $report -Raw | ConvertFrom-Json
        if ($mode -ne 'baseline' -and (-not $native.opaque_native_access -or $native.native_wrappers -lt 5)) { throw 'native method slots were not retained' }
        if ($mode -ne 'baseline') {
            foreach ($reason in @('external_call','callback_address')) {
                $sites = @($native.native_boundaries | Where-Object reason -eq $reason)
                if ($sites.Count -eq 0 -or $sites[0].line -le 0 -or -not $sites[0].source) { throw "Missing native boundary source: $reason" }
            }
        }
        if ($mode -eq 'typed') {
            $callback = $native.methods | Where-Object name -eq '回调'
            if (-not $callback.reachable -or -not $callback.typed) { throw 'typed callback bridge was not exercised' }
        }
    }
}
# DLL 导出是独立根；使用同架构原生调用方实际加载导出，而非只检查符号表。
$exportWorkspace = Join-Path $OutputRoot 'exports'
$callerWorkspace = Join-Path $OutputRoot 'export-caller'
foreach ($directory in @($exportWorkspace,$callerWorkspace)) {
    & $Packager unpack "$PSScriptRoot/../eproj/e-console-exe-new-proj.e" $directory --main-only
    if ($LASTEXITCODE -ne 0) { throw 'DLL fixture unpack failed' }
}
$workspace = $exportWorkspace
Write-Source '程序集1' @'
.版本 2
.程序集 程序集1
.子程序 _启动子程序, 整数型
返回 (0)
.子程序 ExportScalar, 整数型, 公开
.参数 n, 整数型
.局部变量 结果, 整数型
结果 ＝ n × 3
返回 (结果)
.子程序 UnusedScalar, 整数型
返回 (123)
'@
foreach ($mode in @('baseline','reachable','typed')) {
    $dll = Join-Path $OutputRoot "export-$mode.dll"
    $report = Join-Path $OutputRoot "export-$mode.json"
    & $Packager compile $exportWorkspace $dll --arch $Architecture --e-dir $EDirectory --codegen-opt $CodegenOpt --semantic-opt $mode --optimization-report $report @extra
    if ($LASTEXITCODE -ne 0) { throw "DLL $mode compilation failed" }
    $exportReport = Get-Content $report -Raw | ConvertFrom-Json
    $exported = $exportReport.methods | Where-Object name -eq 'ExportScalar'
    if (-not $exported.reachable -or ($mode -eq 'typed' -and -not $exported.typed)) { throw 'export root missing or scalar bridge not exercised' }
    if (($exportReport.methods | Where-Object name -eq 'UnusedScalar').reachable) { throw 'private unreachable export retained' }
    $workspace = $callerWorkspace
    Write-Source '程序集1' @'
.版本 2
.程序集 程序集1
.子程序 _启动子程序, 整数型
.如果真 (ExportScalar (14) ≠ 42)
    返回 (1)
.如果真结束
返回 (0)
'@
    Write-Source '.DLL声明' @"
.版本 2
.DLL命令 ExportScalar, 整数型, "export-$mode.dll", "ExportScalar"
    .参数 n, 整数型
"@
    $caller = Join-Path $OutputRoot "export-caller-$mode.exe"
    & $Packager compile $callerWorkspace $caller --arch $Architecture --e-dir $EDirectory --codegen-opt $CodegenOpt --semantic-opt baseline @extra
    if ($LASTEXITCODE -ne 0) { throw "DLL caller $mode compilation failed" }
    $process = Start-Process -FilePath $caller -WorkingDirectory $OutputRoot -WindowStyle Hidden -PassThru
    try {
        if (-not $process.WaitForExit(15000)) { Stop-Process -Id $process.Id; throw "DLL caller $mode timeout" }
        if ($process.ExitCode -ne 0) { throw "DLL caller $mode returned $($process.ExitCode)" }
    } finally { $process.Dispose() }
}
Write-Host "PASS semantic optimization ${Architecture}: $OutputRoot"
