param(
    [string]$Packager = "$PSScriptRoot/../bin/Win32/Release/e-packager.exe",
    [string]$EDirectory = 'C:\Users\aiqin\OneDrive\e5.6',
    [string]$OutputRoot = "$PSScriptRoot/../temp/native-whole-method-$([guid]::NewGuid().ToString('N'))"
)
$ErrorActionPreference='Stop'
$OutputRoot=[IO.Path]::GetFullPath($OutputRoot)
$workspace=Join-Path $OutputRoot 'workspace'
& $Packager unpack "$PSScriptRoot/../eproj/e-console-exe-new-proj.e" $workspace --main-only
if($LASTEXITCODE -ne 0){throw 'fixture unpack failed'}
# 验证整个子程序只进入一次原生栈，片段间赋值不破坏寄存器和引用写回。
$source=@'
.版本 2
.程序集 程序集1
.子程序 _启动子程序, 整数型
.局部变量 数值, 整数型
数值 ＝ 3
.如果真 (整体栈 (数值) ≠ 42)
    返回 (1)
.如果真结束
.如果真 (数值 ≠ 42)
    返回 (4)
.如果真结束
.如果真 (原生返回 () ≠ 73)
    返回 (2)
.如果真结束
.如果真 (验证父栈 () ≠ 123)
    返回 (3)
.如果真结束
返回 (0)
.子程序 整体栈, 整数型
.参数 引用值, 整数型, 参考
.局部变量 数值, 整数型
.局部变量 复制值, 整数型
数值 ＝ 11
置入代码 ({184, 31, 0, 0, 0})
复制值 ＝ 数值
置入代码 ({3, 69, 248, 137, 69, 252, 139, 85, 8, 137, 2})
返回 (数值)
.子程序 原生返回, 整数型
置入代码 ({184, 73, 0, 0, 0, 201, 195})
返回 (9)
.子程序 验证父栈, 整数型
.局部变量 数值, 整数型
数值 ＝ 123
返回 (读取父栈 ())
.子程序 读取父栈, 整数型
置入代码 ({139, 69, 0, 139, 64, 252, 201, 195})
返回 (0)
'@
[IO.File]::WriteAllText((Join-Path $workspace 'src/程序集1.txt'),($source -replace '\r?\n',"`r`n"),[Text.UTF8Encoding]::new($true))
foreach($mode in @('baseline','typed')){
    $output=Join-Path $OutputRoot "$mode.exe"
    & $Packager compile $workspace $output --arch x86 --e-dir $EDirectory --semantic-opt $mode
    if($LASTEXITCODE -ne 0){throw "$mode compile failed"}
    $generated=[IO.File]::ReadAllText([IO.Path]::ChangeExtension($output,'.generated.cpp'))
    $whole=[regex]::Matches($generated,'static void __declspec\(naked\) ecompiler_native_method_').Count
    if($whole -ne 3 -or $generated.Contains('static void __declspec(naked) ecompiler_machine_')){throw "Expected three complete native methods, got $whole"}
    $process=Start-Process $output -WorkingDirectory $OutputRoot -WindowStyle Hidden -PassThru
    $null=$process.Handle
    try{
        if(-not $process.WaitForExit(15000)){throw "$mode timeout"}
        if($process.ExitCode -ne 0){throw "$mode runtime failure $($process.ExitCode)"}
    }finally{if(-not $process.HasExited){Stop-Process -Id $process.Id};$process.Dispose()}
    Write-Host "PASS $mode whole native frame, register continuity, reference writeback, early return and caller EBP"
}
