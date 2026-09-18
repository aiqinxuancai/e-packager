param(
    [string]$Packager = "$PSScriptRoot/../bin/Win32/Release/e-packager.exe",
    [string]$EDirectory = 'C:\Users\aiqin\OneDrive\e5.6',
    [string]$OutputRoot = "$PSScriptRoot/../temp/native-object-sync-$([guid]::NewGuid().ToString('N'))"
)
$ErrorActionPreference='Stop'
$OutputRoot=[IO.Path]::GetFullPath($OutputRoot)
$workspace=Join-Path $OutputRoot 'workspace'
& $Packager unpack "$PSScriptRoot/../eproj/e-console-exe-new-proj.e" $workspace --main-only
if($LASTEXITCODE -ne 0){throw 'fixture unpack failed'}
function Write-Source([string]$Name,[string]$Content){
    [IO.File]::WriteAllText((Join-Path $workspace "src/$Name.txt"),($Content -replace '\r?\n',"`r`n"),[Text.UTF8Encoding]::new($true))
}
# 同时覆盖嵌套对象去重、混合数值布局、原生改写以及 DLL 回调重入。
Write-Source '.数据类型' @'
.版本 2
.数据类型 坐标
    .成员 横, 整数型
    .成员 纵, 整数型
.数据类型 嵌套状态
    .成员 位置, 坐标
    .成员 名称, 文本型
.数据类型 混合值
    .成员 标志, 字节型
    .成员 短值, 短整数型
    .成员 长值, 长整数型
    .成员 浮值, 小数型
    .成员 双值, 双精度小数型
    .成员 真值, 逻辑型
'@
Write-Source '.DLL声明' @'
.版本 2
.DLL命令 写入地址, , "kernel32.dll", "RtlFillMemory"
    .参数 地址, 整数型
    .参数 长度, 整数型
    .参数 数据, 字节型
.DLL命令 枚举窗口, 逻辑型, "user32.dll", "EnumWindows"
    .参数 回调, 子程序指针
    .参数 数据, 整数型
'@
Write-Source '程序集1' @'
.版本 2
.程序集 程序集1
.程序集变量 状态, 嵌套状态
.程序集变量 位置地址, 整数型
.程序集变量 回调成功, 逻辑型
.子程序 _启动子程序, 整数型
.局部变量 混合, 混合值
.局部变量 次数, 整数型
.如果真 (验证原生改维 () ≠ 0)
    返回 (4)
.如果真结束
混合.标志 ＝ 255
混合.短值 ＝ -1234
混合.长值 ＝ 8589934635
混合.浮值 ＝ 1.25
混合.双值 ＝ -123.5
混合.真值 ＝ 真
写入地址 (取结构地址 (混合), 1, 7)
.如果真 (混合.标志 ≠ 7 或 混合.短值 ≠ -1234 或 混合.长值 ≠ 8589934635 或 混合.浮值 ≠ 1.25 或 混合.双值 ≠ -123.5 或 混合.真值 ≠ 真)
    返回 (1)
.如果真结束
.计次循环首 (20, 次数)
    状态.名称 ＝ “嵌套同步”
    状态.位置.横 ＝ 次数
    状态.位置.纵 ＝ 次数 ＋ 1
    位置地址 ＝ 取结构地址 (状态.位置)
    写入地址 (位置地址, 8, 9)
    .如果真 (状态.位置.横 ≠ 151587081 或 状态.位置.纵 ≠ 151587081)
        返回 (2)
    .如果真结束
    回调成功 ＝ 假
    枚举窗口 (&窗口回调, 次数)
    .如果真 (回调成功 ≠ 真 或 状态.位置.横 ≠ 次数 或 状态.位置.纵 ≠ 次数 ＋ 1 或 状态.名称 ≠ “嵌套同步”)
        返回 (3)
    .如果真结束
.计次循环尾 ()
返回 (0)
.子程序 验证原生改维, 整数型
.局部变量 数据, 坐标, , "2"
数据 [1].横 ＝ 42
数据 [1].纵 ＝ 43
数据 [2].横 ＝ 99
' 机器码把数组元素计数从 2 改为 1，回读时旧元素存储必须仍然有效。
置入代码 ({139, 69, 252, 199, 64, 4, 1, 0, 0, 0})
.如果真 (取数组成员数 (数据) ≠ 1 或 数据 [1].横 ≠ 42 或 数据 [1].纵 ≠ 43)
    返回 (1)
.如果真结束
返回 (0)
.子程序 窗口回调, 逻辑型
.参数 句柄, 整数型
.参数 数据, 整数型
写入地址 (位置地址, 8, 1)
回调成功 ＝ 状态.位置.横 ＝ 16843009 且 状态.位置.纵 ＝ 16843009
状态.位置.横 ＝ 数据
状态.位置.纵 ＝ 数据 ＋ 1
返回 (假)
.子程序 取结构地址, 整数型
.参数 数据, 通用型, 参考
置入代码 ({139, 69, 8, 139, 0, 201, 194, 4, 0})
'@
foreach($mode in @('baseline','typed')){
    $output=Join-Path $OutputRoot "$mode.exe"
    & $Packager compile $workspace $output --arch x86 --e-dir $EDirectory --semantic-opt $mode
    if($LASTEXITCODE -ne 0){throw "$mode compile failed"}
    $process=Start-Process $output -WorkingDirectory $OutputRoot -WindowStyle Hidden -PassThru
    $null=$process.Handle
    try{
        if(-not $process.WaitForExit(15000)){throw "$mode timeout"}
        if($process.ExitCode -ne 0){throw "$mode runtime failure $($process.ExitCode)"}
    }finally{if(-not $process.HasExited){Stop-Process -Id $process.Id};$process.Dispose()}
    Write-Host "PASS $mode mixed scalar layouts, nested native writes and reentrant callback synchronization"
}
