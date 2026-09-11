param([string]$OutputRoot = "$PSScriptRoot/../temp/function-scopes-$([guid]::NewGuid().ToString('N'))")
$ErrorActionPreference = 'Stop'
$repo = Split-Path -Parent $PSScriptRoot
$OutputRoot = [IO.Path]::GetFullPath($OutputRoot)
if (Test-Path -LiteralPath $OutputRoot) { throw "Output already exists: $OutputRoot" }
$workspace = Join-Path $OutputRoot 'workspace'
$decoder = Join-Path $repo 'bin/Win32/Release/e-packager.exe'
& $decoder unpack (Join-Path $repo 'eproj/e-console-exe-new-proj.e') $workspace
if ($LASTEXITCODE -ne 0) { throw 'unpack failed' }
$pages = @{
    '程序集1.txt' = @'
.版本 2
.程序集 程序集1, , , 普通程序集带备注
.子程序 _启动子程序, 整数型
返回 (0)
.子程序 全局调用, 逻辑型
返回 (写到文件 ("", {}))
.子程序 跨程序集调用, 整数型
返回 (全局辅助 ())
.子程序 同名对象调用, 逻辑型
.参数 本类, 本类
返回 (本类.写到文件 ())
'@
    '普通程序集.txt' = @'
.版本 2
.程序集 普通程序集, , , 空基类不是对象
.子程序 全局辅助, 整数型, 公开
返回 (7)
'@
    '本类.txt' = @'
.版本 2
.程序集 本类, <对象>, 公开, 显式根类
.子程序 写到文件, 逻辑型, 公开
.参数 路径, 文本型, 可空
返回 (真)
.子程序 本类调用, 逻辑型, 公开
返回 (写到文件 ())
.子程序 同名向库调用, 逻辑型, 公开
返回 (写到文件 ("", {}))
'@
    '无关类.txt' = @'
.版本 2
.程序集 无关类, <对象>
.子程序 写到文件, 逻辑型, 公开
.参数 路径, 文本型
.参数 数据, 字节集
返回 (假)
'@
}
foreach ($entry in $pages.GetEnumerator()) {
    [IO.File]::WriteAllText((Join-Path $workspace "src/$($entry.Key)"),
        ($entry.Value -replace '\r?\n', "`r`n"), [Text.UTF8Encoding]::new($true))
}
foreach ($arch in @('Win32', 'x64')) {
    $output = Join-Path $OutputRoot "$arch.e"
    & (Join-Path $repo "bin/$arch/Release/e-packager.exe") pack $workspace $output
    if ($LASTEXITCODE -ne 0) { throw "$arch pack failed" }
    $decoded = Join-Path $OutputRoot "$arch-decoded"
    & $decoder unpack $output $decoded --main-only
    if ($LASTEXITCODE -ne 0) { throw "$arch unpack failed" }
    $map = Get-Content -LiteralPath (Join-Path $decoded 'project/.native_source_map.json') -Raw | ConvertFrom-Json
    $main = $map | Where-Object { $_.methods.name -contains '全局调用' }
    $helper = $map | Where-Object { $_.methods.name -contains '全局辅助' }
    $own = $map | Where-Object { $_.methods.name -contains '本类调用' }
    foreach ($assembly in @($main, $helper)) {
        if (($assembly.classId -band 0xFF000000) -ne 0x09000000 -or $assembly.baseClass -ne 0) {
            throw "$arch static assembly encoded as class"
        }
    }
    if (($own.classId -band 0xFF000000) -ne 0x49000000 -or $own.baseClass -ne -1) {
        throw "$arch root class type lost"
    }
    $ownId = ($own.methods | Where-Object name -eq '写到文件').id
    $helperId = ($helper.methods | Where-Object name -eq '全局辅助').id
    $expected = @{
        '全局调用' = @(0, 155)
        '同名向库调用' = @(0, 155)
        '本类调用' = @(-2, $ownId)
        '跨程序集调用' = @(-2, $helperId)
    }
    foreach ($method in $map.methods) {
        if ($method.name -eq '同名对象调用') {
            $bytes = [Convert]::FromBase64String($method.expressionData)
            if ([BitConverter]::ToInt32($bytes, 19) -ne $ownId -or
                ([BitConverter]::ToInt16($bytes, 25) -band 0x10) -ne 0 -or $bytes[35] -ne 0x38) {
                throw "$arch object receiver shadowed by class name"
            }
        }
        if (-not $expected.ContainsKey($method.name)) { continue }
        $bytes = [Convert]::FromBase64String($method.expressionData)
        # These methods contain exactly Return(Call(...)); Return's empty header is 18 bytes.
        if ($bytes[18] -ne 0x21 -or
            [BitConverter]::ToInt16($bytes, 23) -ne $expected[$method.name][0] -or
            [BitConverter]::ToInt32($bytes, 19) -ne $expected[$method.name][1]) {
            throw "$arch incorrect callee: $($method.name)"
        }
    }
    Write-Host "PASS $arch static/root class identity and scoped native call targets"
}
