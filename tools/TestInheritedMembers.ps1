param([string]$OutputRoot = "$PSScriptRoot/../temp/inherited-members-$([guid]::NewGuid().ToString('N'))")
$ErrorActionPreference = 'Stop'
$repo = Split-Path -Parent $PSScriptRoot
$OutputRoot = [IO.Path]::GetFullPath($OutputRoot)
$workspace = Join-Path $OutputRoot 'workspace'
$decoder = Join-Path $repo 'bin/Win32/Release/e-packager.exe'
& $decoder unpack (Join-Path $repo 'eproj/e-console-exe-new-proj.e') $workspace
if ($LASTEXITCODE -ne 0) { throw 'unpack failed' }
function Write-Source([string]$Name, [string]$Content) {
    [IO.File]::WriteAllText((Join-Path $workspace "src/$Name.txt"),
        ($Content -replace '\r?\n', "`r`n"), [Text.UTF8Encoding]::new($true))
}
Write-Source '基类' @'
.版本 2
.程序集 基类, , 公开
.子程序 取值, 整数型, 公开
返回 (42)
'@
Write-Source '中间类' @'
.版本 2
.程序集 中间类, 基类, 公开
'@
Write-Source '派生类' @'
.版本 2
.程序集 派生类, 中间类, 公开
'@
$source = @'
.版本 2
.程序集 程序集1
.子程序 _启动子程序, 整数型
.局部变量 实例, 派生类, , "2"
.局部变量 数值, 整数型
.局部变量 文本, 文本型
数值 ＝ 实例[1].取值 ()
.如果真（数值 ＞ 0）
    文本 ＝ “保留（中文）括号”
    数值 ＝ 实例[2].取值 ()  ' 保留（注释）括号
.如果真结束
返回 (数值)
'@
Write-Source '程序集1' $source
foreach ($arch in @('Win32', 'x64')) {
    $tool = Join-Path $repo "bin/$arch/Release/e-packager.exe"
    & $tool validate $workspace
    if ($LASTEXITCODE -ne 0) { throw "$arch inheritance validation failed" }
    $output = Join-Path $OutputRoot "$arch.e"
    & $tool pack $workspace $output
    if ($LASTEXITCODE -ne 0) { throw "$arch inheritance packing failed" }
    $unpacked = Join-Path $OutputRoot "$arch-unpacked"
    & $decoder unpack $output $unpacked --main-only
    if ($LASTEXITCODE -ne 0) { throw 're-unpack failed' }
    $actual = [IO.File]::ReadAllText((Join-Path $unpacked 'src/程序集1.txt'))
    foreach ($expected in @('保留（中文）括号', '保留（注释）括号', '.如果真 (', '.取值 (')) {
        if (-not $actual.Contains($expected)) { throw "Missing preserved text: $expected" }
    }
    Write-Host "PASS $arch inherited method and fullwidth parentheses"
}
Write-Source '程序集1' ($source.Replace('.取值 ()', '.不存在 ()'))
foreach ($arch in @('Win32', 'x64')) {
    $tool = Join-Path $repo "bin/$arch/Release/e-packager.exe"
    $output = & $tool validate $workspace 2>&1 | Out-String
    if ($LASTEXITCODE -eq 0 -or -not $output.Contains('member=不存在, owner_type=派生类')) {
        throw "Unknown inherited member accepted: $output"
    }
    Write-Host "PASS $arch unknown inherited member rejected"
}

$windowWorkspace = Join-Path $OutputRoot 'window'
$template = Join-Path $repo 'eproj/e-window-exe-new-proj.e'
& $decoder unpack $template $windowWorkspace --main-only
if ($LASTEXITCODE -ne 0) { throw 'window unpack failed' }
$xmlFile = Get-ChildItem (Join-Path $windowWorkspace 'src') -Filter '*.xml' | Select-Object -First 1
$xml = [IO.File]::ReadAllText($xmlFile.FullName)
[IO.File]::WriteAllText($xmlFile.FullName, ([char]0xFEFF + $xml), [Text.UTF8Encoding]::new($true))
foreach ($arch in @('Win32', 'x64')) {
    $tool = Join-Path $repo "bin/$arch/Release/e-packager.exe"
    & $tool validate $windowWorkspace
    if ($LASTEXITCODE -ne 0) { throw "$arch duplicate BOM validation failed" }
    $output = Join-Path $OutputRoot "window-$arch.e"
    & $tool pack $windowWorkspace $output
    if ($LASTEXITCODE -ne 0 -or (Get-FileHash $output).Hash -ne (Get-FileHash $template).Hash) {
        throw "$arch duplicate BOM changed native output"
    }
    Write-Host "PASS $arch duplicate BOM byte-identical roundtrip"
}
