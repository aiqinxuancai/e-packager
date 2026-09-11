param([string]$OutputRoot = "$PSScriptRoot/../temp/member-diagnostics-$([guid]::NewGuid().ToString('N'))")
$ErrorActionPreference = 'Stop'
$repo = Split-Path -Parent $PSScriptRoot
$OutputRoot = [IO.Path]::GetFullPath($OutputRoot)
$workspace = Join-Path $OutputRoot 'workspace'
$win32 = Join-Path $repo 'bin/Win32/Release/e-packager.exe'
& $win32 unpack (Join-Path $repo 'eproj/e-console-exe-new-proj.e') $workspace
if ($LASTEXITCODE -ne 0) { throw 'unpack failed' }
$source = @'
.版本 2
.程序集 程序集1
.子程序 _启动子程序, 整数型
.局部变量 测试实例, 测试类型
.局部变量 结果, 整数型
测试实例.缺失命令 ()
结果 ＝ 测试实例.缺失字段
返回 (0)
'@
$types = @'
.版本 2
.数据类型 测试类型
    .成员 已有字段, 整数型
'@
foreach ($entry in @(@('程序集1.txt', $source), @('.数据类型.txt', $types))) {
    [IO.File]::WriteAllText((Join-Path $workspace "src/$($entry[0])"),
        ($entry[1] -replace '\r?\n', "`r`n"), [Text.UTF8Encoding]::new($true))
}
foreach ($arch in @('Win32', 'x64')) {
    $tool = Join-Path $repo "bin/$arch/Release/e-packager.exe"
    foreach ($format in @('text', 'json')) {
        $output = & $tool validate $workspace --diagnostics $format 2>&1 | Out-String
        if ($LASTEXITCODE -eq 0) { throw 'invalid members unexpectedly accepted' }
        foreach ($member in @('缺失命令', '缺失字段')) {
            if (-not $output.Contains("member=$member, owner_type=测试类型, receiver=测试实例")) {
                throw "Missing diagnostic context for $member ($arch/$format): $output"
            }
        }
        $output | Set-Content -LiteralPath (Join-Path $OutputRoot "$arch-$format.log") -Encoding utf8
        Write-Host "PASS $arch $format member diagnostics"
    }
}
