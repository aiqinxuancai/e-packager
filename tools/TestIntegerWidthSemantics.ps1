param(
    [string]$Packager = "$PSScriptRoot/../bin/Win32/Release/e-packager.exe",
    [string]$EDirectory = 'C:\Users\aiqin\OneDrive\e5.6',
    [string]$OutputRoot = "$PSScriptRoot/../temp/integer-width-$([guid]::NewGuid().ToString('N'))"
)
$ErrorActionPreference='Stop'
$OutputRoot=[IO.Path]::GetFullPath($OutputRoot)
$workspace=Join-Path $OutputRoot 'workspace'
& $Packager unpack "$PSScriptRoot/../eproj/e-console-exe-new-proj.e" $workspace --main-only
if($LASTEXITCODE -ne 0){throw 'fixture unpack failed'}
$source=@'
.版本 2
.程序集 程序集1
.子程序 _启动子程序, 整数型
.局部变量 整数, 整数型
.局部变量 短整数, 短整数型
.局部变量 字节, 字节型
.局部变量 长整数, 长整数型
.局部变量 数据, 字节集
.局部变量 索引, 整数型
整数 ＝ 2147483647
整数 ＝ 整数 ＋ 1
.如果真 (整数 ≠ -2147483648)
    返回 (1)
.如果真结束
短整数 ＝ 32767
短整数 ＝ 短整数 ＋ 1
.如果真 (短整数 ≠ -32768)
    返回 (2)
.如果真结束
字节 ＝ 255
字节 ＝ 字节 ＋ 2
.如果真 (字节 ≠ 1)
    返回 (3)
.如果真结束
长整数 ＝ 2147483647
长整数 ＝ 长整数 ＋ 2
.如果真 (长整数 ≠ 2147483649)
    返回 (4)
.如果真结束
数据 ＝ {108, 97, 121, 111, 117, 116, 95, 116, 121, 112, 101}
整数 ＝ 0
.计次循环首 (11, 索引)
    整数 ＝ 31 × 整数 ＋ 数据 [索引]
.计次循环尾 ()
.如果真 (整数 ≠ 2011608879)
    返回 (5)
.如果真结束
返回 (0)
'@
[IO.File]::WriteAllText((Join-Path $workspace 'src/程序集1.txt'),($source -replace '\r?\n',"`r`n"),[Text.UTF8Encoding]::new($true))
foreach($mode in @('baseline','typed')){
    $output=Join-Path $OutputRoot "$mode.exe"
    & $Packager compile $workspace $output --arch x86 --e-dir $EDirectory --semantic-opt $mode
    if($LASTEXITCODE -ne 0){throw "$mode compile failed"}
    $process=Start-Process $output -WorkingDirectory $OutputRoot -WindowStyle Hidden -PassThru
    try{
        if(-not $process.WaitForExit(15000)){Stop-Process -Id $process.Id;throw "$mode timeout"}
        if($process.ExitCode -ne 0){throw "$mode runtime failure $($process.ExitCode)"}
    }finally{$process.Dispose()}
    Write-Host "PASS $mode integer widths and overflowing string hash"
}
