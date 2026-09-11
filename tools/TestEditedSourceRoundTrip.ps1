param(
    [Parameter(Mandatory = $true)][string]$InputFile,
    [string]$OutputRoot = "$PSScriptRoot/../temp/edited-roundtrip-$([guid]::NewGuid().ToString('N'))",
    [string[]]$Architectures = @('Win32', 'x64'),
    [int]$Limit = 0
)
$ErrorActionPreference = 'Stop'
$PSNativeCommandUseErrorActionPreference = $false
$repo = Split-Path -Parent $PSScriptRoot
$OutputRoot = [IO.Path]::GetFullPath($OutputRoot)
if (Test-Path -LiteralPath $OutputRoot) { throw "Output root already exists: $OutputRoot" }
[IO.Directory]::CreateDirectory($OutputRoot) | Out-Null
$inputCopy = Join-Path $OutputRoot 'original.e'
Copy-Item -LiteralPath $InputFile -Destination $inputCopy
$originalHash = (Get-FileHash -LiteralPath $inputCopy).Hash
$decoder = Join-Path $repo 'bin/Win32/Release/e-packager.exe'
$workspace = Join-Path $OutputRoot 'workspace'
function Run-Tool([string]$Tool, [string[]]$Arguments, [string]$Log) {
    $output = & $Tool @Arguments 2>&1 | Out-String
    $exitCode = $LASTEXITCODE
    [IO.File]::WriteAllText($Log, $output, [Text.UTF8Encoding]::new($true))
    return [pscustomobject]@{ Code = $exitCode; Text = $output }
}
$unpack = Run-Tool $decoder @('unpack', $inputCopy, $workspace) (Join-Path $OutputRoot 'unpack.log')
if ($unpack.Code -ne 0) { throw $unpack.Text }
$meta = Get-Content -LiteralPath (Join-Path $workspace 'project/_meta.json') -Raw | ConvertFrom-Json
$pages = @($meta.sourceFiles)
if ($Limit -gt 0) { $pages = @($pages | Select-Object -First $Limit) }
$baselineFiles = @{}
foreach ($folder in @('src', 'image', 'audio')) {
    foreach ($file in Get-ChildItem -LiteralPath (Join-Path $workspace $folder) -File -Recurse) {
        $relative = [IO.Path]::GetRelativePath($workspace, $file.FullName)
        $baselineFiles[$relative] = [IO.File]::ReadAllBytes($file.FullName)
    }
}
function Normalize-Text([string]$Text) {
    return (($Text -split '\r?\n' | ForEach-Object { $_.Trim() } | Where-Object { $_ }) -join "`n")
}
$results = [Collections.Generic.List[object]]::new()
foreach ($arch in $Architectures) {
    $tool = Join-Path $repo "bin/$arch/Release/e-packager.exe"
    $archRoot = Join-Path $OutputRoot $arch
    [IO.Directory]::CreateDirectory($archRoot) | Out-Null
    $baselineOutput = Join-Path $archRoot 'baseline.e'
    $baseline = Run-Tool $tool @('pack', $workspace, $baselineOutput) (Join-Path $archRoot 'baseline.log')
    if ($baseline.Code -ne 0 -or (Get-FileHash -LiteralPath $baselineOutput).Hash -ne $originalHash) {
        throw "Baseline roundtrip failed: $($baseline.Text)"
    }
    $index = 0
    foreach ($page in $pages) {
        $index++
        $id = '{0:D3}' -f $index
        $caseRoot = Join-Path $archRoot $id
        [IO.Directory]::CreateDirectory($caseRoot) | Out-Null
        $path = Join-Path $workspace $page.relativePath
        $originalBytes = [IO.File]::ReadAllBytes($path)
        $marker = "' edited-roundtrip-$id"
        $edited = [IO.File]::ReadAllText($path).TrimEnd() + "`r`n`r`n$marker`r`n"
        $output = Join-Path $caseRoot 'edited.e'
        $decoded = Join-Path $caseRoot 'decoded'
        $failure = ''
        $packCode = $null
        try {
            [IO.File]::WriteAllText($path, $edited, [Text.UTF8Encoding]::new($true))
            $pack = Run-Tool $tool @('pack', $workspace, $output) (Join-Path $caseRoot 'pack.log')
            $packCode = $pack.Code
            if ($pack.Code -ne 0) { throw $pack.Text.Trim() }
            if ((Get-FileHash -LiteralPath $output).Hash -eq $originalHash) { throw 'Edited source reused original bytes' }
            $decode = Run-Tool $decoder @('unpack', $output, $decoded, '--main-only') (Join-Path $caseRoot 'decode.log')
            if ($decode.Code -ne 0) { throw $decode.Text.Trim() }
            foreach ($folder in @('src', 'image', 'audio')) {
                foreach ($file in Get-ChildItem -LiteralPath (Join-Path $decoded $folder) -File -Recurse) {
                    $relative = [IO.Path]::GetRelativePath($decoded, $file.FullName)
                    if (-not $baselineFiles.ContainsKey($relative)) { throw "Unexpected file: $relative" }
                }
            }
            foreach ($relative in $baselineFiles.Keys) {
                $actualPath = Join-Path $decoded $relative
                if (-not (Test-Path -LiteralPath $actualPath)) { throw "Missing file: $relative" }
                if ($relative -match '^src[\\/]') {
                    $expected = [IO.File]::ReadAllText((Join-Path $workspace $relative))
                    $actual = [IO.File]::ReadAllText($actualPath)
                    if ((Normalize-Text $expected) -cne (Normalize-Text $actual)) { throw "Source mismatch: $relative" }
                } elseif ($relative -notmatch '^(image|audio)[\\/]list\.json$') {
                    if ((Get-FileHash -LiteralPath (Join-Path $workspace $relative)).Hash -ne
                        (Get-FileHash -LiteralPath $actualPath).Hash) { throw "Resource mismatch: $relative" }
                }
            }
        } catch { $failure = $_.Exception.Message }
        finally { [IO.File]::WriteAllBytes($path, $originalBytes) }
        $results.Add([pscustomobject]@{
            Architecture = $arch; Page = $page.relativePath; Marker = $marker
            PackExit = $packCode; Passed = ($failure -eq ''); Failure = $failure; Directory = $caseRoot
        })
        $results | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath (Join-Path $OutputRoot 'results.json') -Encoding utf8
        Write-Host "$arch $id/$($pages.Count) $($page.logicalName): passed=$($failure -eq '')"
    }
}
$failed = @($results | Where-Object { -not $_.Passed }).Count
if ((Get-FileHash -LiteralPath $InputFile).Hash -ne $originalHash) { throw 'Original input changed during test' }
Write-Host "Edited roundtrip: total=$($results.Count), failed=$failed, results=$OutputRoot"
if ($failed -gt 0) { exit 1 }
