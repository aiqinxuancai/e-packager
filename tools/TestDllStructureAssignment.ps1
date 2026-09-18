param(
    [string]$Packager = "$PSScriptRoot/../bin/Win32/Release/e-packager.exe",
    [string]$EDirectory = 'C:\Users\aiqin\OneDrive\e5.6',
    [string]$OutputRoot = "$PSScriptRoot/../temp/dll-structure-$([guid]::NewGuid().ToString('N'))"
)
$ErrorActionPreference='Stop'
$OutputRoot=[IO.Path]::GetFullPath($OutputRoot)
$workspace=Join-Path $OutputRoot 'workspace'
& $Packager unpack "$PSScriptRoot/../eproj/e-console-exe-new-proj.e" $workspace --main-only
if($LASTEXITCODE -ne 0){throw 'fixture unpack failed'}
function Write-Source([string]$Name,[string]$Content){
    [IO.File]::WriteAllText((Join-Path $workspace "src/$Name.txt"),($Content -replace '\r?\n',"`r`n"),[Text.UTF8Encoding]::new($true))
}
Write-Source '.数据类型' @'
.版本 2
.数据类型 窗口注册块
    .成员 长度, 整数型
    .成员 数据, 字节型, , "44"
.数据类型 坐标块
    .成员 横坐标, 整数型
    .成员 纵坐标, 整数型
'@
Write-Source '.DLL声明' @'
.版本 2
.DLL命令 原生大小, 整数型, "kernel32.dll", "LocalSize"
    .参数 数据, 通用型
.DLL命令 写入结构, , "kernel32.dll", "RtlFillMemory"
    .参数 数据, 窗口注册块
    .参数 长度, 整数型
    .参数 内容, 字节型
.DLL命令 写入指针, , "kernel32.dll", "RtlFillMemory"
    .参数 地址, 整数型
    .参数 长度, 整数型
    .参数 内容, 字节型
'@
Write-Source '程序集1' @'
.版本 2
.程序集 程序集1
.子程序 _启动子程序, 整数型
.局部变量 结构, 窗口注册块
.局部变量 索引, 整数型
.局部变量 坐标, 坐标块
.计次循环首 (1000, 索引)
    结构.长度 ＝ 原生大小 (结构)
    .如果真 (结构.长度 ≠ 48)
        返回 (1)
    .如果真结束
    写入结构 (结构, 48, 7)
    .如果真 (结构.数据 [44] ≠ 7)
        返回 (2)
    .如果真结束
.计次循环尾 ()
写入指针 (取结构地址 (坐标), 8, 9)
.如果真 (坐标.横坐标 ≠ 151587081 或 坐标.纵坐标 ≠ 151587081)
    返回 (3)
.如果真结束
返回 (0)
.子程序 取结构地址, 整数型
.参数 数据, 通用型, 参考
置入代码 ({139, 69, 8, 139, 0, 201, 194, 4, 0})
'@
foreach($mode in @('baseline','typed')){
    $output=Join-Path $OutputRoot "$mode.exe"
    & $Packager compile $workspace $output --arch x86 --e-dir $EDirectory --semantic-opt $mode
    if($LASTEXITCODE -ne 0){throw "$mode compile failed"}
    $process=Start-Process $output -WorkingDirectory $OutputRoot -WindowStyle Hidden -PassThru
    try{
        if(-not $process.WaitForExit(15000)){Stop-Process -Id $process.Id;throw "$mode timeout"}
        if($process.ExitCode -ne 0){throw "$mode runtime failure $($process.ExitCode)"}
    }finally{$process.Dispose()}
    Write-Host "PASS $mode native allocation size, structure writeback and assignment lifetime"
}
