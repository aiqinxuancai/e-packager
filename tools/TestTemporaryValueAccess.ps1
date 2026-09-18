param(
    [string]$OutputRoot = "$PSScriptRoot/../temp/temporary-value-access",
    [string]$EDirectory = 'C:\Users\aiqin\OneDrive\e5.6',
    [string]$CoreDirectory = 'D:\git\BlackMoonKernelStaticLib\adapter'
)
$ErrorActionPreference='Stop'
$OutputRoot=[IO.Path]::GetFullPath($OutputRoot)
$workspace=Join-Path $OutputRoot 'workspace'
$x86=[IO.Path]::GetFullPath("$PSScriptRoot/../bin/Win32/Release/e-packager.exe")
& $x86 unpack "$PSScriptRoot/../eproj/e-console-exe-new-proj.e" $workspace --main-only
if($LASTEXITCODE -ne 0){throw 'unpack failed'}
function Write-Source($name,$text){[IO.File]::WriteAllText("$workspace/src/$name.txt",($text -replace '\r?\n',"`r`n"),[Text.UTF8Encoding]::new($true))}
Write-Source '.数据类型' @'
.版本 2
.数据类型 记录
    .成员 文本, 文本型
    .成员 数据, 字节集
    .成员 数字, 整数型
'@
Write-Source '程序集1' @'
.版本 2
.程序集 程序集1
.程序集变量 调用次数, 整数型
.子程序 _启动子程序, 整数型
.局部变量 本地, 记录
.局部变量 合计, 整数型
.如果真 (接收文本 (产生 ().文本) ≠ “中文”)
    返回 (1)
.如果真结束
.如果真 (调用次数 ≠ 1)
    返回 (11)
.如果真结束
.如果真 (接收数据 (产生 ().数据) ≠ 42)
    返回 (2)
.如果真结束
.如果真 (调用次数 ≠ 2)
    返回 (12)
.如果真结束
.如果真 (产生 ().数据[2] ≠ 43)
    返回 (3)
.如果真结束
.如果真 (调用次数 ≠ 3)
    返回 (13)
.如果真结束
.如果真 (参考数字 (产生 ().数字) ≠ 10)
    返回 (4)
.如果真结束
.如果真 (调用次数 ≠ 4)
    返回 (14)
.如果真结束
本地.数字 ＝ 8
参考数字 (本地.数字)
.如果真 (本地.数字 ≠ 9)
    返回 (5)
.如果真结束
.计次循环首 (2, )
    合计 ＝ 合计 ＋ 1
.计次循环尾 ()
.计次循环首 (3, )
    .计次循环首 (2, )
        合计 ＝ 合计 ＋ 1
    .计次循环尾 ()
.计次循环尾 ()
.如果真 (合计 ≠ 8)
    返回 (6)
.如果真结束
返回 (0)
.子程序 产生, 记录
.局部变量 结果, 记录
调用次数 ＝ 调用次数 ＋ 1
结果.文本 ＝ “中文”
结果.数据 ＝ {42, 43}
结果.数字 ＝ 9
返回 (结果)
.子程序 接收文本, 文本型
.参数 数据, 文本型
返回 (数据)
.子程序 接收数据, 整数型
.参数 数据, 字节集
返回 (数据[1])
.子程序 参考数字, 整数型
.参数 数据, 整数型, 参考
数据 ＝ 数据 ＋ 1
返回 (数据)
'@
foreach($arch in @('x86','x64')){
    $tool=if($arch -eq 'x86'){$x86}else{[IO.Path]::GetFullPath("$PSScriptRoot/../bin/x64/Release/e-packager.exe")}
    foreach($mode in @('baseline','typed')){
        $output=Join-Path $OutputRoot "$arch-$mode.exe"
        & $tool compile $workspace $output --arch $arch --e-dir $EDirectory --blackmoon-core-dir $CoreDirectory --semantic-opt $mode *> "$OutputRoot/$arch-$mode.log"
        if($LASTEXITCODE -ne 0){Get-Content "$OutputRoot/$arch-$mode.log" -Tail 15;throw 'compile failed'}
        $process=Start-Process $output -WorkingDirectory $OutputRoot -WindowStyle Hidden -PassThru
        try{
            if(-not $process.WaitForExit(15000)){Stop-Process -Id $process.Id;throw 'timeout'}
            if($process.ExitCode -ne 0){throw "$arch/$mode runtime failure $($process.ExitCode)"}
        }finally{$process.Dispose()}
        Write-Host "PASS $arch/${mode}: temporary members, indexing, references, single evaluation, sequential/nested count loops"
    }
}
