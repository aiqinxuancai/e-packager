param(
    [string]$Packager = "$PSScriptRoot/../bin/Win32/Release/e-packager.exe",
    [string]$EDirectory = 'C:\Users\aiqin\OneDrive\e5.6',
    [string]$OutputRoot = "$PSScriptRoot/../temp/native-frame-publication-$([guid]::NewGuid().ToString('N'))"
)
$ErrorActionPreference='Stop'
$OutputRoot=[IO.Path]::GetFullPath($OutputRoot)
$workspace=Join-Path $OutputRoot 'workspace'
& $Packager unpack "$PSScriptRoot/../eproj/e-console-exe-new-proj.e" $workspace --main-only
if($LASTEXITCODE -ne 0){throw 'fixture unpack failed'}
# 验证延迟物化后，机器码仍能通过保存的 EBP 读取调用者当前的整数与数组。
$source=@'
.版本 2
.程序集 程序集1
.子程序 _启动子程序, 整数型
.如果真 (验证调用者整数 () ≠ 0)
    返回 (1)
.如果真结束
.如果真 (验证调用者数组 () ≠ 0)
    返回 (2)
.如果真结束
.如果真 (验证数值数组 () ≠ 0)
    返回 (3)
.如果真结束
返回 (0)
.子程序 验证调用者整数, 整数型
.局部变量 数值, 整数型
.局部变量 次数, 整数型
数值 ＝ 41
.计次循环首 (20, 次数)
    数值 ＝ 数值 ＋ 1
    .如果真 (读取调用者整数 () ≠ 数值)
        返回 (1)
    .如果真结束
.计次循环尾 ()
返回 (0)
.子程序 读取调用者整数, 整数型
置入代码 ({139, 69, 0, 139, 64, 252, 201, 194, 0, 0})
.子程序 验证调用者数组, 整数型
.局部变量 数据, 整数型, , "8"
.局部变量 次数, 整数型
.计次循环首 (20, 次数)
    数据 [1] ＝ 次数 × 7
    .如果真 (读取调用者数组 () ≠ 数据 [1])
        返回 (1)
    .如果真结束
.计次循环尾 ()
返回 (0)
.子程序 读取调用者数组, 整数型
置入代码 ({139, 69, 0, 139, 64, 252, 139, 64, 8, 201, 194, 0, 0})
.子程序 验证数值数组, 整数型
.局部变量 长整数数组, 长整数型, , "2"
.局部变量 双精度数组, 双精度小数型, , "2"
.局部变量 短整数数组, 短整数型, , "2"
.局部变量 字节数组, 字节型, , "2"
.局部变量 小数数组, 小数型, , "2"
.局部变量 逻辑数组, 逻辑型, , "2"
长整数数组 [1] ＝ 8589934635
双精度数组 [1] ＝ -123.5
短整数数组 [1] ＝ -1234
字节数组 [1] ＝ 255
小数数组 [1] ＝ 1.25
逻辑数组 [1] ＝ 真
' 改写长整数低 32 位，并让其余数组经过原生发布与回读。
置入代码 ({139, 69, 252, 199, 64, 8, 42, 0, 0, 0})
.如果真 (长整数数组 [1] ≠ 8589934634 或 双精度数组 [1] ≠ -123.5 或 短整数数组 [1] ≠ -1234 或 字节数组 [1] ≠ 255 或 小数数组 [1] ≠ 1.25 或 逻辑数组 [1] ≠ 真)
    返回 (1)
.如果真结束
返回 (0)
'@
[IO.File]::WriteAllText((Join-Path $workspace 'src/程序集1.txt'),($source -replace '\r?\n',"`r`n"),[Text.UTF8Encoding]::new($true))
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
    Write-Host "PASS $mode native code reads current caller scalar and array through saved EBP"
}
