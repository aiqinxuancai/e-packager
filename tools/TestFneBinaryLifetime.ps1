param(
    [string]$Packager = "$PSScriptRoot/../bin/Win32/Release/e-packager.exe",
    [string]$EDirectory = 'C:\Users\aiqin\OneDrive\e5.6',
    [string]$OutputRoot = "$PSScriptRoot/../temp/fne-binary-lifetime-$([guid]::NewGuid().ToString('N'))"
)
$ErrorActionPreference='Stop'
$OutputRoot=[IO.Path]::GetFullPath($OutputRoot)
$workspace=Join-Path $OutputRoot 'workspace'
& $Packager unpack "$PSScriptRoot/../eproj/e-console-exe-new-proj.e" $workspace --main-only
if($LASTEXITCODE -ne 0){throw 'fixture unpack failed'}

& $Packager update $workspace --add-elib (Join-Path $EDirectory 'lib/eziputils.fne')
if($LASTEXITCODE -ne 0){throw 'ZIP dependency setup failed'}
Add-Type -AssemblyName System.IO.Compression
$payload=New-Object byte[] (2*1024*1024)
for($i=0;$i -lt $payload.Length;$i++){$payload[$i]=[byte]($i -band 255)}
$stream=[IO.File]::Create((Join-Path $OutputRoot 'payload.zip'))
$zip=[IO.Compression.ZipArchive]::new($stream,[IO.Compression.ZipArchiveMode]::Create,$false)
try{
    $entry=$zip.CreateEntry('payload.bin',[IO.Compression.CompressionLevel]::NoCompression)
    $data=$entry.Open()
    try{$data.Write($payload,0,$payload.Length)}finally{$data.Dispose()}
}finally{$zip.Dispose();$stream.Dispose()}
$source=@'
.版本 2
.支持库 eziputils
.程序集 程序集1
.子程序 _启动子程序, 整数型
.局部变量 数据, 字节集
数据 ＝ 读入文件 (“payload.zip”)
返回 (验证解压 (数据))
.子程序 验证解压, 整数型
.参数 数据, 字节集
.局部变量 解压, ZIP解压
.局部变量 信息, ZIP项目信息
.局部变量 次数, 整数型
.局部变量 干扰, 字节集
.如果真 (解压.打开自内存 (数据, ) ＝ 假)
    返回 (1)
.如果真结束
.如果真 (写到文件 (“borrowed-copy.zip”, 数据) ＝ 假)
    返回 (5)
.如果真结束
.计次循环首 (12, 次数)
    干扰 ＝ 取空白字节集 (2097280)
.计次循环尾 ()
.如果真 (解压.取项目数 () ≠ 1)
    返回 (2)
.如果真结束
信息 ＝ 解压.取项目信息 (0)
.如果真 (信息.名称 ≠ “payload.bin”)
    返回 (3)
.如果真结束
.如果真 (解压.解压项目 (0, “extracted.bin”) ≠ 0)
    返回 (4)
.如果真结束
解压.关闭 ()
返回 (0)
'@

[IO.File]::WriteAllText((Join-Path $workspace 'src/程序集1.txt'),($source -replace '\r?\n',"`r`n"),[Text.UTF8Encoding]::new($true))
$sha=[Security.Cryptography.SHA256]::Create()
try{
    $expected=[Convert]::ToBase64String($sha.ComputeHash($payload))
    foreach($mode in @('baseline','typed')){
        $output=Join-Path $OutputRoot "$mode.exe"
        & $Packager compile $workspace $output --arch x86 --e-dir $EDirectory --semantic-opt $mode
        if($LASTEXITCODE -ne 0){throw "$mode compile failed"}
        $destination=Join-Path $OutputRoot 'extracted.bin'
        if(Test-Path -LiteralPath $destination){Remove-Item -LiteralPath $destination}
        $process=Start-Process $output -WorkingDirectory $OutputRoot -WindowStyle Hidden -PassThru
        $null=$process.Handle
        try{
            if(-not $process.WaitForExit(20000)){throw "$mode timeout"}
            if($process.ExitCode -ne 0){throw "$mode runtime failure $($process.ExitCode)"}
        }finally{if(-not $process.HasExited){Stop-Process -Id $process.Id};$process.Dispose()}
        if(-not (Test-Path -LiteralPath $destination)){throw "$mode did not extract the file"}
        $actual=[Convert]::ToBase64String($sha.ComputeHash([IO.File]::ReadAllBytes($destination)))
        if($expected -ne $actual){throw "$mode extracted bytes differ"}
        Write-Host "PASS $mode FNE binary lifetime across calls, allocation churn, ZIP entry lookup and exact 2 MiB extraction"
    }
}finally{$sha.Dispose()}
