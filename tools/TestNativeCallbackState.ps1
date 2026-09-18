param(
    [string]$Packager = "$PSScriptRoot/../bin/Win32/Release/e-packager.exe",
    [string]$EDirectory = 'C:\Users\aiqin\OneDrive\e5.6',
    [string]$OutputRoot = "$PSScriptRoot/../temp/native-callback-$([guid]::NewGuid().ToString('N'))"
)
$ErrorActionPreference='Stop'
$OutputRoot=[IO.Path]::GetFullPath($OutputRoot)
$workspace=Join-Path $OutputRoot 'workspace'
& $Packager unpack "$PSScriptRoot/../eproj/e-console-exe-new-proj.e" $workspace --main-only
if($LASTEXITCODE -ne 0){throw 'fixture unpack failed'}
function Write-Source([string]$Name,[string]$Content){
    [IO.File]::WriteAllText((Join-Path $workspace "src/$Name.txt"),($Content -replace '\r?\n',"`r`n"),[Text.UTF8Encoding]::new($true))
}
Write-Source '计数器' @'
.版本 2
.程序集 计数器, <对象>
.程序集变量 数值, 整数型
.子程序 设置, , 公开
.参数 新值, 整数型
数值 ＝ 新值
.子程序 读取, 整数型, 公开
返回 (数值)
'@
Write-Source '.DLL声明' @'
.版本 2
.DLL命令 枚举窗口, 逻辑型, "user32.dll", "EnumWindows"
    .参数 回调, 子程序指针
    .参数 数据, 整数型
'@
Write-Source '程序集1' @'
.版本 2
.程序集 程序集1
.程序集变量 状态, 计数器
.子程序 _启动子程序, 整数型
状态.设置 (7)
机器码入口 ()
枚举窗口 (&窗口回调, 42)
.如果真 (状态.读取 () ≠ 42)
    返回 (1)
.如果真结束
返回 (0)
.子程序 窗口回调, 逻辑型
.参数 句柄, 整数型
.参数 数据, 整数型
状态.设置 (数据)
返回 (假)
.子程序 机器码入口
置入代码 ({144})
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
    Write-Host "PASS $mode callback preserves class state across DLL return"
}
