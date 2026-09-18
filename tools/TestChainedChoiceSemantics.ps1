param(
    [string]$Packager = "$PSScriptRoot/../bin/Win32/Release/e-packager.exe",
    [string]$EDirectory = 'C:\Users\aiqin\OneDrive\e5.6',
    [string]$OutputRoot = "$PSScriptRoot/../temp/chained-choice-$([guid]::NewGuid().ToString('N'))"
)
$ErrorActionPreference = 'Stop'
$OutputRoot = [IO.Path]::GetFullPath($OutputRoot)
$workspace = Join-Path $OutputRoot 'workspace'
& $Packager unpack "$PSScriptRoot/../eproj/e-console-exe-new-proj.e" $workspace --main-only
if ($LASTEXITCODE -ne 0) { throw 'unpack failed' }
function Write-Source([string]$Name, [string]$Content) {
    [IO.File]::WriteAllText((Join-Path $workspace "src/$Name.txt"),
        ($Content -replace '\r?\n', "`r`n"), [Text.UTF8Encoding]::new($true))
}
Write-Source '计数器' @'
.版本 2
.程序集 计数器, <对象>
.程序集变量 数值, 整数型
.子程序 设置, 计数器, 公开
.参数 新值, 整数型
.局部变量 结果, 计数器
数值 ＝ 新值
结果.保存 (数值)
返回 (结果)
.子程序 增加, 计数器, 公开
.参数 增量, 整数型
.局部变量 结果, 计数器
数值 ＝ 数值 ＋ 增量
结果.保存 (数值)
返回 (结果)
.子程序 保存, , 公开
.参数 新值, 整数型
数值 ＝ 新值
.子程序 读取, 整数型, 公开
返回 (数值)
'@
$source = @'
.版本 2
.程序集 程序集1
.程序集变量 调用次数, 整数型
.子程序 _启动子程序, 整数型
.局部变量 对象, 计数器
.局部变量 结果, 计数器
.局部变量 数值, 整数型
.局部变量 时间, 系统时间
.局部变量 小数, 双精度小数型
结果 ＝ 对象.设置 (10).增加 (5).增加 (7)
.如果真 (结果.读取 () ≠ 22 或 对象.读取 () ≠ 10)
    返回 (1)
.如果真结束
数值 ＝ 创建对象 ().增加 (3).读取 ()
.如果真 (数值 ≠ 43 或 调用次数 ≠ 1)
    返回 (2)
.如果真结束
.如果真 (多项选择 (1, 11, 22, 33) ≠ 11 或 多项选择 (3, 11, 22, 33) ≠ 33)
    返回 (3)
.如果真结束
.如果真 (多项选择 (2, “甲”, “乙”) ≠ “乙” 或 多项选择 (2, { 1 }, { 2, 3 }) ≠ { 2, 3 })
    返回 (4)
.如果真结束
调用次数 ＝ 0
数值 ＝ 多项选择 (取索引 (), 11, 22, 33)
.如果真 (数值 ≠ 22 或 调用次数 ≠ 1)
    返回 (5)
.如果真结束
.如果真 (通用长度 (“abc”) ≠ 3 或 通用长度 ({ 97, 98, 0 }) ≠ 2)
    返回 (6)
.如果真结束
小数 ＝ -2.5
.如果真 (通用绝对值 (小数) ≠ 2.5)
    返回 (7)
.如果真结束
通用系统时间 (时间)
.如果真 (时间.年 ＜ 2020 或 时间.月 ＜ 1 或 时间.月 ＞ 12)
    返回 (8)
.如果真结束
数值 ＝ 40
通用递增 (数值)
.如果真 (数值 ≠ 41)
    返回 (9)
.如果真结束
.如果真 (到数值 ([0100年01月01日]) ≠ -657434 或 到数值 ([1899年12月29日06时00分00秒]) ≠ -1.25)
    返回 (10)
.如果真结束
返回 (0)
.子程序 创建对象, 计数器
.局部变量 对象, 计数器
调用次数 ＝ 调用次数 ＋ 1
返回 (对象.设置 (40))
.子程序 取索引, 整数型
调用次数 ＝ 调用次数 ＋ 1
返回 (2)
'@
Write-Source '.数据类型' @'
.版本 2
.数据类型 系统时间
    .成员 年, 短整数型
    .成员 月, 短整数型
    .成员 星期, 短整数型
    .成员 日, 短整数型
    .成员 时, 短整数型
    .成员 分, 短整数型
    .成员 秒, 短整数型
    .成员 毫秒, 短整数型
'@
Write-Source '.DLL声明' @'
.版本 2
.DLL命令 通用长度, 整数型, "kernel32.dll", "lstrlenA"
    .参数 数据, 通用型
.DLL命令 通用绝对值, 双精度小数型, "msvcrt.dll", "fabs", , ($cdecl)
    .参数 数据, 通用型
.DLL命令 通用系统时间, , "kernel32.dll", "GetSystemTime"
    .参数 数据, 通用型, 传址
.DLL命令 通用递增, 整数型, "kernel32.dll", "InterlockedIncrement"
    .参数 数据, 通用型, 传址
'@
Write-Source '程序集1' $source
foreach ($mode in @('baseline', 'typed')) {
    $output = Join-Path $OutputRoot "$mode.exe"
    & $Packager compile $workspace $output --arch x86 --e-dir $EDirectory --semantic-opt $mode
    if ($LASTEXITCODE -ne 0) { throw "$mode compilation failed" }
    $process = Start-Process $output -WorkingDirectory $OutputRoot -WindowStyle Hidden -PassThru
    $null=$process.Handle
    try {
        if (-not $process.WaitForExit(15000)) { Stop-Process -Id $process.Id; throw "$mode timed out" }
        if ($process.ExitCode -ne 0) { throw "$mode runtime check failed: $($process.ExitCode)" }
    } finally { $process.Dispose() }
    Write-Host "PASS $mode chained calls, choice values, generic DLL ABI and OLE date literals"
}
foreach ($index in @(0, 3)) {
    Write-Source '程序集1' ".版本 2`r`n.程序集 程序集1`r`n.子程序 _启动子程序, 整数型`r`n返回 (多项选择 ($index, 11, 22))"
    $output = Join-Path $OutputRoot "invalid-choice-$index.exe"
    & $Packager compile $workspace $output --arch x86 --e-dir $EDirectory
    if ($LASTEXITCODE -ne 0) { throw 'invalid choice fixture compilation failed' }
    $process = Start-Process $output -WorkingDirectory $OutputRoot -WindowStyle Hidden -PassThru -RedirectStandardError (Join-Path $OutputRoot "invalid-choice-$index.log")
    $null=$process.Handle
    try {
        if (-not $process.WaitForExit(15000)) { Stop-Process -Id $process.Id; throw 'invalid choice timed out' }
        if ($process.ExitCode -ne 87) { throw "invalid choice was not rejected: $($process.ExitCode)" }
    } finally { $process.Dispose() }
    Write-Host "PASS invalid choice index $index"
}
