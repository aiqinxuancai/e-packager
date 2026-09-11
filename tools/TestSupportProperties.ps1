param([string]$OutputRoot = "$PSScriptRoot/../temp/support-properties-$([guid]::NewGuid().ToString('N'))")
$ErrorActionPreference = 'Stop'
$repo = Split-Path -Parent $PSScriptRoot
$OutputRoot = [IO.Path]::GetFullPath($OutputRoot)
if (Test-Path -LiteralPath $OutputRoot) { throw "Output already exists: $OutputRoot" }
$workspace = Join-Path $OutputRoot 'workspace'
$decoder = Join-Path $repo 'bin/Win32/Release/e-packager.exe'
& $decoder unpack (Join-Path $repo 'eproj/e-console-exe-new-proj.e') $workspace
if ($LASTEXITCODE -ne 0) { throw 'unpack failed' }
$resourcePath = Join-Path $OutputRoot 'payload.bin'
[IO.File]::WriteAllBytes($resourcePath, [byte[]]@(0, 1, 128, 255))
& $decoder update $workspace --add-image "测试资源=$resourcePath"
if ($LASTEXITCODE -ne 0) { throw 'add resource failed' }
$source = @'
.版本 2
.程序集 程序集1
.子程序 _启动子程序, 整数型
' 123 => 网络时间协议（NTP）
返回 (0)
.子程序 属性测试, 整数型
.参数 画布, 画板
.参数 画布组, 画板, 数组
.参数 数据源, 数据源
.局部变量 颜色, 整数型
.局部变量 高度, 整数型
.局部变量 字形, 字体
.局部变量 数据, 字节集
.局部变量 日期, 日期时间型
日期 ＝ [0100年01月01日]
日期 ＝ [2024年02月29日12时34分56秒]
日期 ＝ [1899年12月29日06时00分00秒]
数据 ＝ {0} ＋ 到字节集 (画布.取窗口句柄 ()) ＋ {255, 0}
数据 ＝ #测试资源
颜色 ＝ #红色
颜色 ＝ 画布.画板背景色
画布.画板背景色 ＝ -16777216
画布.左边 ＝ 1
高度 ＝ 画布.画板高度  ' 保存表项文本
高度 ＝ 画布.取窗口句柄 ()
高度 ＝ 颜色 ＋ 高度 × 2 － 2
高度 ＝ 颜色 × 高度 × 2 ÷ 8
高度 ＝ 高度 \ 4096 × 4096 ＋ 选择 (高度 % 4096 ≠ 0, 4096, 0)
画布.移动 (1, 2, 3, 4)
画布.清除 (, , , )
数据源.清除 (1, 1, , )
画布.可视 ＝ 真
画布.标记 ＝ “属性测试”
.如果真 (画布.标记 ?= “http” ＝ 假)
    画布.标记 ＝ “http:” ＋ 画布.标记
.如果真结束
画布.字体.字体大小 ＝ 12
字形.字体名称 ＝ 画布.字体.字体名称
画布组[1].画板背景色 ＝ 颜色
画布组[高度].画板背景色 ＝ 颜色
画布组[画布组[高度].高度].画板背景色 ＝ 颜色
.如果真 (-16777216 ＝ 画布.画板背景色)  ' 判断背景色
    返回 (画布组[1].字体.字体大小)
.如果真结束
返回 (高度)
.子程序 链式测试, 文本型
.参数 对象值, 对象
对象值.读对象型属性 (“Privileges”, ).方法 (“Add”, 1)
对象值.清除 ()
返回 (返回变体 ().取文本 ())
.子程序 限定调用, 变体型
返回 (程序集1.返回变体 ())
.子程序 返回变体, 变体型
.局部变量 值, 变体型
值.置类型 (#变体类型.数值型数组)
返回 (值)
'@
$sourcePath = Join-Path $workspace 'src/程序集1.txt'
[IO.File]::WriteAllText($sourcePath, ($source -replace '\r?\n', "`r`n"), [Text.UTF8Encoding]::new($true))
function Normalize([string]$Text) {
    return (($Text -split '\r?\n' | ForEach-Object { $_.Trim() } | Where-Object { $_ }) -join "`n")
}
foreach ($arch in @('Win32', 'x64')) {
    $tool = Join-Path $repo "bin/$arch/Release/e-packager.exe"
    $output = Join-Path $OutputRoot "$arch.e"
    & $tool pack $workspace $output
    if ($LASTEXITCODE -ne 0) { throw "$arch property rebuild failed" }
    $decoded = Join-Path $OutputRoot "$arch-decoded"
    & $decoder unpack $output $decoded --main-only
    if ($LASTEXITCODE -ne 0) { throw 're-unpack failed' }
    $nativeMap = Get-Content -LiteralPath (Join-Path $decoded 'project/.native_source_map.json') -Raw | ConvertFrom-Json
    $allowedMarkers = @{ methodReference = @(0x1E, 0x21, 0x6A); variableReference = @(0x1D, 0x21, 0x6A); constantReference = @(0x1B) }
    foreach ($class in $nativeMap) {
        foreach ($method in $class.methods) {
            $bytes = [Convert]::FromBase64String($method.expressionData)
            foreach ($table in $allowedMarkers.Keys) {
                $references = [Convert]::FromBase64String($method.$table)
                if ($references.Length % 4) { throw "$arch unaligned $table" }
                for ($offset = 0; $offset -lt $references.Length; $offset += 4) {
                    $position = [BitConverter]::ToInt32($references, $offset)
                    if ($position -lt 0 -or $position -ge $bytes.Length -or $bytes[$position] -notin $allowedMarkers[$table]) {
                        throw "$arch invalid $table entry: offset=$position"
                    }
                }
            }
        }
    }
    $resourceIndex = Get-Content -LiteralPath (Join-Path $decoded 'image/list.json') -Raw | ConvertFrom-Json
    $resource = @($resourceIndex.items | Where-Object { $_.logicalName -eq '测试资源' })
    if ($resource.Count -ne 1) { throw 'Resource index entry missing' }
    if ((Get-FileHash -LiteralPath (Join-Path $decoded $resource[0].relativePath)).Hash -ne
        (Get-FileHash -LiteralPath $resourcePath).Hash) { throw 'Resource content changed' }
    $actual = [IO.File]::ReadAllText((Join-Path $decoded 'src/程序集1.txt'))
    if ((Normalize $actual) -cne (Normalize $source)) { throw "$arch source mismatch: $actual" }
    $invalidCases = @{
        'invalid-date' = @{ Source = $source.Replace('[2024年02月29日12时34分56秒]', '[2023年02月29日12时34分56秒]'); Code = 'invalid_date_literal' }
        'unknown' = @{ Source = $source.Replace('画布.画板背景色', '画布.不存在的属性'); Code = 'member_not_found' }
        'readonly' = @{ Source = $source.Replace('画布.左边 ＝ 1', '画布.画板高度 ＝ 1'); Code = 'assignment_target_read_only' }
    }
    foreach ($entry in $invalidCases.GetEnumerator()) {
        [IO.File]::WriteAllText($sourcePath, $entry.Value.Source, [Text.UTF8Encoding]::new($true))
        try {
            $diagnostic = & $tool pack $workspace (Join-Path $OutputRoot "$arch-$($entry.Key).e") 2>&1 | Out-String
            if ($LASTEXITCODE -eq 0 -or -not $diagnostic.Contains($entry.Value.Code)) {
                throw "$arch invalid property test failed: $diagnostic"
            }
        } finally {
            [IO.File]::WriteAllText($sourcePath, ($source -replace '\r?\n', "`r`n"), [Text.UTF8Encoding]::new($true))
        }
    }
    Write-Host "PASS $arch property read/write, chained font, array access, unknown member and read-only rejection"
}
if ((Get-FileHash -LiteralPath (Join-Path $OutputRoot 'Win32.e')).Hash -ne
    (Get-FileHash -LiteralPath (Join-Path $OutputRoot 'x64.e')).Hash) {
    throw 'Native and exported support metadata produced different bytes'
}
