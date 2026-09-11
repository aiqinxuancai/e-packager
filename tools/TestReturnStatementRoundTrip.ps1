param(
    [string]$Packager = "$PSScriptRoot/../bin/Win32/Release/e-packager.exe",
    [string]$Decoder,
    [string]$OutputRoot = "$PSScriptRoot/../temp/return-roundtrip-$([guid]::NewGuid().ToString('N'))",
    [string]$Eide,
    [string]$AutoLinkerTest
)
$ErrorActionPreference = 'Stop'
$Packager = (Resolve-Path $Packager).Path
$Decoder = if ($Decoder) { (Resolve-Path $Decoder).Path } else { $Packager }
$template = (Resolve-Path "$PSScriptRoot/../eproj/e-console-exe-new-proj.e").Path
$encoding = [Text.UTF8Encoding]::new($true)
function Invoke-Packager([string[]]$Arguments) {
    $tool = if ($Arguments[0] -eq 'unpack') { $Decoder } else { $Packager }
    & $tool @Arguments
    if ($LASTEXITCODE -ne 0) { throw "e-packager failed: $Arguments" }
}
New-Item -ItemType Directory -Force $OutputRoot | Out-Null
$OutputRoot = (Resolve-Path $OutputRoot).Path
$workspace = "$OutputRoot/workspace"
Invoke-Packager @('unpack', $template, $workspace, '--main-only')
Invoke-Packager @('pack', $workspace, "$OutputRoot/unchanged.e")
if ((Get-FileHash $template).Hash -ne (Get-FileHash "$OutputRoot/unchanged.e").Hash) {
    throw 'Unchanged template no longer round-trips byte for byte'
}
$source = @'
.版本 2

.程序集 程序集1

.子程序 _启动子程序, 整数型, , 本子程序在程序启动后最先执行
.局部变量 结果, 长整数型
结果 ＝ 阶乘 (6)
返回 (0)  ' 可以根据您的需要返回任意数值

.子程序 阶乘, 长整数型, 公开, 递归计算 n 的阶乘
.参数 n, 整数型, , 整数n
.如果真 (n ≤ 1)
    返回 (1)
.如果真结束
返回 (n × 阶乘 (n － 1))
'@
$cases = [ordered]@{
    report = $source
    no_comment = $source.Replace("  ' 可以根据您的需要返回任意数值", '')
    edited_comment = $source.Replace('可以根据您的需要返回任意数值', '新的行尾注释')
    edited_return = $source.Replace('返回 (0)', '返回 (2)').Replace('可以根据您的需要返回任意数值', '新的返回值')
    only_return = ".版本 2`n.程序集 程序集1`n.子程序 _启动子程序, 整数型`n返回 (0)  ' 单独返回"
    quoted_comment = $source + @'

.子程序 文本返回, 文本型, 公开
返回 (“文本  ' 不是注释”)  ' 真正的注释

.子程序 分支返回, 整数型, 公开
.参数 n, 整数型
.如果 (n > 0)
    返回 (1)  ' 正数分支
.否则
    返回 (0)  ' 其他分支
.如果结束
'@
}
foreach ($case in $cases.GetEnumerator()) {
    [IO.File]::WriteAllText("$workspace/src/程序集1.txt", ($case.Value -replace '\r?\n', "`r`n") + "`r`n", $encoding)
    $packed = "$OutputRoot/$($case.Key).e"
    $unpacked = "$OutputRoot/$($case.Key)"
    Invoke-Packager @('pack', $workspace, $packed)
    Invoke-Packager @('unpack', $packed, $unpacked, '--main-only')
    $actual = [IO.File]::ReadAllText("$unpacked/src/程序集1.txt")
    # 忽略解包器补齐的空行，逐行检查所有代码和注释，不依赖 compare-bundle。
    $expectedLines = @($case.Value -split '\r?\n' | ForEach-Object { $_.Trim() } | Where-Object { $_ })
    $actualLines = @($actual -split '\r?\n' | ForEach-Object { $_.Trim() } | Where-Object { $_ })
    $normalize = { param($line) ([string]$line).Replace('＞', '>').Replace('＜', '<').Replace('＝', '=').Replace('＋', '+').Replace('－', '-').Replace('×', '*') }; $expectedNormalized = @($expectedLines | ForEach-Object { & $normalize $_ }); $actualNormalized = @($actualLines | ForEach-Object { & $normalize $_ })
    if (($expectedNormalized -join "`n") -cne ($actualNormalized -join "`n")) {
        $missing = @($expectedNormalized | Where-Object { $_ -notin $actualNormalized })
        $extra = @($actualNormalized | Where-Object { $_ -notin $expectedNormalized })
        throw "Source mismatch: $($case.Key); missing=[$($missing -join ' | ')]; extra=[$($extra -join ' | ')]"
    }
    if ($Eide -and $AutoLinkerTest) {
        Invoke-Packager @('compile-check', $packed, '--eide', $Eide, '--autolinker-test', $AutoLinkerTest, '--compile-static')
    }
    Write-Host "PASS $($case.Key)"
}
Write-Host "All return-statement cases passed: $OutputRoot"
