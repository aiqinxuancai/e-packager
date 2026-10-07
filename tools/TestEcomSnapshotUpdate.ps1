param(
    [string]$PackagerPath = "$PSScriptRoot/../bin/Win32/Release/e-packager.exe",
    [string]$DecoderPath = "$PSScriptRoot/../bin/Win32/Release/e-packager.exe",
    [ValidateSet('All', 'Dependency', 'Update')][string]$Case = 'All',
    [string]$OutputRoot = "$PSScriptRoot/../temp/issue9-$([guid]::NewGuid().ToString('N'))"
)
$ErrorActionPreference = 'Stop'
$PSNativeCommandUseErrorActionPreference = $false
$PackagerPath = [IO.Path]::GetFullPath($PackagerPath)
$OutputRoot = [IO.Path]::GetFullPath($OutputRoot)
if (Test-Path -LiteralPath $OutputRoot) { throw "Output already exists: $OutputRoot" }
[IO.Directory]::CreateDirectory($OutputRoot) | Out-Null
$template = "$PSScriptRoot/../eproj/e-console-exe-new-proj.e"

function Invoke-Tool([string[]]$Arguments, [string]$ExpectedError = '') {
    $tool = if ($Arguments[0] -eq 'unpack') { $DecoderPath } else { $PackagerPath }
    $output = & $tool @Arguments 2>&1 | Out-String
    if ($ExpectedError) {
        if ($LASTEXITCODE -eq 0 -or -not $output.Contains($ExpectedError)) { throw "Expected $ExpectedError`: $output" }
    } elseif ($LASTEXITCODE -ne 0) { throw $output }
}
function Write-Text([string]$Path, [string]$Text) {
    [IO.File]::WriteAllText($Path, ($Text -replace '\r?\n', "`r`n"), [Text.UTF8Encoding]::new($true))
}
function Assert-Source([string]$File, [string]$Marker, [string]$Name) {
    $decoded = Join-Path $OutputRoot $Name
    Invoke-Tool @('unpack', $File, $decoded, '--main-only')
    $text = (Get-ChildItem "$decoded/src" -Filter '*.txt' -Force | ForEach-Object { [IO.File]::ReadAllText($_.FullName) }) -join "`n"
    if (-not $text.Contains($Marker)) { throw "Repacked source lost: $Marker" }
}

if ($Case -in @('All', 'Dependency')) {
    $workspace = Join-Path $OutputRoot 'dependency'
    $module = Join-Path $workspace 'ecom/fixture'
    Invoke-Tool @('unpack', $template, $workspace)
    Invoke-Tool @('unpack', $template, $module, '--main-only')
    Write-Text "$module/src/程序集1.txt" @'
.版本 2
.程序集 公共库
.子程序 公开取值, 整数型, 公开
返回 (42)
.子程序 私有取值, 整数型
返回 (7)
'@
    $manifestPath = "$workspace/project/.module.json"
    $manifest = Get-Content -LiteralPath $manifestPath -Raw | ConvertFrom-Json
    $manifest.dependencies += [pscustomobject]@{ kind='ecom'; name='fixture'; path='fixture.ec'; localWorkspace='ecom/fixture' }
    Write-Text $manifestPath ($manifest | ConvertTo-Json -Depth 30)
    $source = @'
.版本 2
.程序集 程序集1
.子程序 _启动子程序, 整数型
返回 (公开取值 ())
'@
    Write-Text "$workspace/src/程序集1.txt" $source
    $packed = Join-Path $OutputRoot 'dependency.e'
    Invoke-Tool @('pack', $workspace, $packed)
    Assert-Source $packed '公开取值 ()' 'dependency-decoded'
    Write-Text "$workspace/src/程序集1.txt" ($source.Replace('公开取值', '私有取值'))
    Invoke-Tool @('pack', $workspace, "$OutputRoot/private.e") 'function_not_found'
    Write-Host 'PASS public functions in ordinary dependency assemblies; private functions rejected'
}

if ($Case -in @('All', 'Update')) {
    $workspace = Join-Path $OutputRoot 'update'
    Invoke-Tool @('unpack', $template, $workspace)
    $originalHash = (Get-FileHash -LiteralPath $template).Hash
    Invoke-Tool @('update', $workspace)
    Invoke-Tool @('pack', $workspace, "$OutputRoot/unchanged.e")
    if ((Get-FileHash "$OutputRoot/unchanged.e").Hash -ne $originalHash) { throw 'Unchanged update broke byte-identical roundtrip' }
    $sourcePath = "$workspace/src/程序集1.txt"
    Write-Text $sourcePath @'
.版本 2
.程序集 程序集1
.子程序 _启动子程序, 整数型
' issue9-edited
返回 (0)
'@
    Write-Text "$workspace/src/新增页.txt" @'
.版本 2
.程序集 新增页
.子程序 新增函数, 整数型
返回 (9)
'@
    foreach ($iteration in 1..2) {
        Invoke-Tool @('update', $workspace)
        $packed = "$OutputRoot/updated-$iteration.e"
        Invoke-Tool @('pack', $workspace, $packed)
        if ((Get-FileHash $packed).Hash -eq $originalHash) { throw 'update silently reused old native bytes' }
        Assert-Source $packed 'issue9-edited' "updated-$iteration-decoded"
        $newPage = "$OutputRoot/updated-$iteration-decoded/src/新增页.txt"
        if (-not (Test-Path -LiteralPath $newPage) -or -not ([IO.File]::ReadAllText($newPage)).Contains('新增函数')) { throw 'New program page was lost' }
    }
    Write-Host 'PASS unchanged roundtrip and repeated update preserve edits and new pages'

    $metaPath = "$workspace/project/_meta.json"
    $meta = Get-Content $metaPath -Raw | ConvertFrom-Json
    $meta.PSObject.Properties.Remove('nativeBundleDigest')
    Write-Text $metaPath ($meta | ConvertTo-Json -Depth 30)
    Invoke-Tool @('update', $workspace)
    Invoke-Tool @('pack', $workspace, "$OutputRoot/missing-digest.e")
    Assert-Source "$OutputRoot/missing-digest.e" 'issue9-edited' 'missing-digest-decoded'

    Write-Text $sourcePath ([IO.File]::ReadAllText($sourcePath).Replace('返回 (0)', '返回 (不存在的函数 ())'))
    Invoke-Tool @('update', $workspace)
    Invoke-Tool @('pack', $workspace, "$OutputRoot/invalid.e") 'function_not_found'
    Write-Host 'PASS missing digest cannot bless stale bytes; invalid edited code fails after update'
}
Write-Host "Artifacts: $OutputRoot"
