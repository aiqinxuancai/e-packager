param(
    [string]$InputFile = "$PSScriptRoot/../eproj/e-window-exe-full+otherFne.e",
    [string]$OutputRoot = "$PSScriptRoot/../temp/window-properties-$([guid]::NewGuid().ToString('N'))",
    [string]$CompileIde,
    [string]$CompileLauncher
)
$ErrorActionPreference = 'Stop'
$PSNativeCommandUseErrorActionPreference = $false
$tool = [IO.Path]::GetFullPath("$PSScriptRoot/../bin/Win32/Release/e-packager.exe")
$OutputRoot = [IO.Path]::GetFullPath($OutputRoot)
if (Test-Path -LiteralPath $OutputRoot) { throw "Output already exists: $OutputRoot" }
[IO.Directory]::CreateDirectory($OutputRoot) | Out-Null
Copy-Item -LiteralPath $InputFile -Destination "$OutputRoot/original.e"
function Run-Tool([string[]]$Arguments, [string]$Log) {
    & $tool @Arguments *> "$OutputRoot/$Log"
    if ($LASTEXITCODE -ne 0) { throw (Get-Content "$OutputRoot/$Log" -Raw) }
}
function Save-Xml($Document, $Path) {
    $settings = [Xml.XmlWriterSettings]::new()
    $settings.Encoding = [Text.UTF8Encoding]::new($true)
    $settings.Indent = $true
    $settings.NewLineChars = "`r`n"
    $writer = [Xml.XmlWriter]::Create($Path, $settings)
    try { $Document.Save($writer) } finally { $writer.Dispose() }
}
Run-Tool @('unpack', "$OutputRoot/original.e", "$OutputRoot/workspace", '--main-only') 'unpack.log'
Run-Tool @('pack', "$OutputRoot/workspace", "$OutputRoot/unchanged.e") 'unchanged.log'
if ((Get-FileHash "$OutputRoot/original.e").Hash -ne (Get-FileHash "$OutputRoot/unchanged.e").Hash) {
    throw 'Unedited round trip changed bytes'
}
$expected = [Collections.Generic.List[object]]::new()
$events = @{}
$types = [Collections.Generic.HashSet[string]]::new()
foreach ($file in Get-ChildItem "$OutputRoot/workspace/src" -Filter '*.xml') {
    [xml]$xml = Get-Content $file.FullName -Raw
    $events[$file.Name] = @($xml.SelectNodes('//*[@处理器]') | ForEach-Object { $_.OuterXml }) -join "`n"
    foreach ($node in $xml.SelectNodes('//*[@名称 and @宽度]')) {
        [void]$types.Add($node.LocalName)
        $changes = @{ '宽度' = ([int]$node.GetAttribute('宽度') + 8).ToString() }
        if ($node.LocalName -ne '窗口') { $changes['左边'] = ([int]$node.GetAttribute('左边') + 4).ToString() }
        if ($node.HasAttribute('标题')) { $changes['标题'] = $node.GetAttribute('名称') + '已修改' }
        if ($node.HasAttribute('文本颜色')) { $changes['文本颜色'] = '255' }
        if ($node.HasAttribute('背景颜色')) { $changes['背景颜色'] = '65535' }
        if ($node.LocalName -eq '窗口') { $changes['底色'] = '15790320'; $changes['最大化按钮'] = '真' }
        if ($node.LocalName -eq '高级选择夹') { $changes['子夹头高度'] = '30'; $changes['表头方向'] = '1' }
        if ($node.LocalName -eq '进度条') { $changes['位置'] = '5' }
        foreach ($key in $changes.Keys) {
            $node.SetAttribute($key, $changes[$key])
            $expected.Add([pscustomobject]@{File=$file.Name; Name=$node.GetAttribute('名称'); Property=$key; Value=$changes[$key]})
        }
    }
    Save-Xml $xml $file.FullName
}
Run-Tool @('pack', "$OutputRoot/workspace", "$OutputRoot/edited.e") 'pack.log'
Run-Tool @('unpack', "$OutputRoot/edited.e", "$OutputRoot/reopened", '--main-only') 'reopen.log'
$documents = @{}
foreach ($file in Get-ChildItem "$OutputRoot/reopened/src" -Filter '*.xml') {
    [xml]$xml = Get-Content $file.FullName -Raw
    $documents[$file.Name] = $xml
    $actualEvents = @($xml.SelectNodes('//*[@处理器]') | ForEach-Object { $_.OuterXml }) -join "`n"
    if ($events[$file.Name] -ne $actualEvents) { throw "Event bindings changed: $($file.Name)" }
}
foreach ($item in $expected) {
    $node = @($documents[$item.File].SelectNodes('//*[@名称]') | Where-Object { $_.GetAttribute('名称') -eq $item.Name })
    if ($node.Count -ne 1 -or $node[0].GetAttribute($item.Property) -cne $item.Value) {
        throw "Property mismatch: $($item.File) / $($item.Name) / $($item.Property), expected $($item.Value)"
    }
}
# 窗口修改会触发完整语义编码，原有控件引用仍必须保留。
$source = Get-Content "$OutputRoot/reopened/src/窗口程序集_启动窗口.txt" -Raw
if (-not $source.Contains('标签1.标题 ＝ “HAHAHA”')) { throw 'Control reference was not preserved' }
if ($CompileIde -or $CompileLauncher) {
    if (-not (Test-Path -LiteralPath $CompileIde) -or -not (Test-Path -LiteralPath $CompileLauncher)) {
        throw 'Both CompileIde and CompileLauncher must exist'
    }
    & $CompileLauncher headless-compile $CompileIde "$OutputRoot/edited.e" "$OutputRoot/edited.exe" --result "$OutputRoot/ide-result.json" --timeout 60 *> "$OutputRoot/ide.log"
    if ($LASTEXITCODE -ne 0) { throw (Get-Content "$OutputRoot/ide.log" -Raw) }
    $result = Get-Content "$OutputRoot/ide-result.json" -Raw | ConvertFrom-Json
    if (-not $result.compile_result.artifact_verified) { throw 'IDE did not verify the compiled artifact' }
}
$expected | ConvertTo-Json -Depth 5 | Set-Content "$OutputRoot/expected.json" -Encoding utf8
Write-Host "PASS: $($expected.Count) property checks, $($types.Count) window/control types, unchanged bytes and event bindings; $OutputRoot"
