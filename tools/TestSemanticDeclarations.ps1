param([string]$OutputRoot = "$PSScriptRoot/../temp/semantic-declarations-$([guid]::NewGuid().ToString('N'))")
$ErrorActionPreference = 'Stop'
$repo = Split-Path -Parent $PSScriptRoot
$OutputRoot = [IO.Path]::GetFullPath($OutputRoot)
$workspace = Join-Path $OutputRoot 'workspace'
$decoder = Join-Path $repo 'bin/Win32/Release/e-packager.exe'
& $decoder unpack (Join-Path $repo 'eproj/e-console-exe-new-proj.e') $workspace
if ($LASTEXITCODE -ne 0) { throw 'unpack failed' }
$pages = @{
    '程序集1.txt' = @'
.版本 2
.程序集 程序集1
.子程序 _启动子程序, 整数型
全局值 ＝ 取计数 ()
全局结构.字段 ＝ 全局值
返回 (读取全局 ())
.子程序 读取全局, 整数型
返回 (全局结构.字段)
.子程序 局部遮蔽, 整数型
.局部变量 全局值, 整数型
全局值 ＝ 7
返回 (全局值)
'@
    '.全局变量.txt' = @'
.版本 2
.全局变量 全局值, 整数型
.全局变量 全局结构, 测试结构
'@
    '.数据类型.txt' = @'
.版本 2
.数据类型 测试结构
    .成员 字段, 整数型
'@
    '.DLL声明.txt' = @'
.版本 2
.DLL命令 取计数, 整数型, "kernel32.dll", "GetTickCount"
'@
}
foreach ($entry in $pages.GetEnumerator()) {
    [IO.File]::WriteAllText((Join-Path $workspace "src/$($entry.Key)"),
        ($entry.Value -replace '\r?\n', "`r`n"), [Text.UTF8Encoding]::new($true))
}
foreach ($arch in @('Win32', 'x64')) {
    $tool = Join-Path $repo "bin/$arch/Release/e-packager.exe"
    $output = Join-Path $OutputRoot "$arch.e"
    & $tool pack $workspace $output
    if ($LASTEXITCODE -ne 0) { throw "$arch declaration rebuild failed" }
    $decoded = Join-Path $OutputRoot "$arch-decoded"
    & $decoder unpack $output $decoded --main-only
    if ($LASTEXITCODE -ne 0) { throw 're-unpack failed' }
    $actual = [IO.File]::ReadAllText((Join-Path $decoded 'src/程序集1.txt'))
    foreach ($line in ($pages['程序集1.txt'] -split '\r?\n')) {
        if ($line -and -not $actual.Contains($line)) { throw "Missing source after rebuild: $line" }
    }
    Write-Host "PASS $arch globals, DLL, struct, forward call and local shadowing"
}
