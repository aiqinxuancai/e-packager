param(
    [Parameter(Mandatory = $true)][string]$InputFile,
    [string]$OutputRoot = "$PSScriptRoot/../temp/edited-roundtrip-$([guid]::NewGuid().ToString('N'))",
    [string[]]$Architectures = @('Win32', 'x64'),
    [int]$Limit = 0,
    [ValidateRange(1, 2147483647)][int]$StartPage = 1,
    [switch]$AllPagesTogether,
    [string]$CompileIde,
    [string]$CompileLauncher
)
$ErrorActionPreference = 'Stop'
$PSNativeCommandUseErrorActionPreference = $false
$repo = Split-Path -Parent $PSScriptRoot
if ($CompileIde -or $CompileLauncher) {
    if (-not (Test-Path -LiteralPath $CompileIde -PathType Leaf) -or
        -not (Test-Path -LiteralPath $CompileLauncher -PathType Leaf)) {
        throw 'IDE verification requires existing CompileIde and CompileLauncher paths'
    }
}
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
$pages = @($pages | Select-Object -Skip ($StartPage - 1))
if ($Limit -gt 0) { $pages = @($pages | Select-Object -First $Limit) }
$cases = if ($AllPagesTogether) {
    @([pscustomobject]@{ logicalName = 'all-pages'; relativePath = '*'; sourcePages = $pages })
} else {
    @($pages | ForEach-Object { [pscustomobject]@{
        logicalName = $_.logicalName; relativePath = $_.relativePath; sourcePages = @($_)
    } })
}
$baselineFiles = @{}
foreach ($folder in @('src', 'image', 'audio')) {
    foreach ($file in Get-ChildItem -LiteralPath (Join-Path $workspace $folder) -File -Recurse) {
        $relative = [IO.Path]::GetRelativePath($workspace, $file.FullName)
        $baselineFiles[$relative] = [IO.File]::ReadAllBytes($file.FullName)
    }
}
function Normalize-Text([string]$Text) {
    $lines = $Text.Replace("`r`n", "`n")
    $trimmed = [regex]::Replace($lines, '(?m)^[^\S\n]+|[^\S\n]+$', '')
    return [regex]::Replace($trimmed, '(?m)^\n', '').TrimEnd("`n")
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
    $index = $StartPage - 1
    foreach ($page in $cases) {
        $index++
        $id = '{0:D3}' -f $index
        $caseRoot = Join-Path $archRoot $id
        [IO.Directory]::CreateDirectory($caseRoot) | Out-Null
        $originalBytes = @{}
        $marker = "' edited-roundtrip-$id"
        $output = Join-Path $caseRoot 'edited.e'
        $decoded = Join-Path $caseRoot 'decoded'
        $failure = ''
        $packCode = $null
        $compilePassed = $null
        try {
            foreach ($sourcePage in $page.sourcePages) {
                $path = Join-Path $workspace $sourcePage.relativePath
                $originalBytes[$path] = [IO.File]::ReadAllBytes($path)
                $edited = [IO.File]::ReadAllText($path).TrimEnd() + "`r`n`r`n$marker`r`n"
                [IO.File]::WriteAllText($path, $edited, [Text.UTF8Encoding]::new($true))
            }
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
            if ($CompileIde) {
                $compileResult = Join-Path $caseRoot 'compile-result.json'
                $artifact = Join-Path $caseRoot "jingyi-audit-$arch-$id.ec"
                $compile = Run-Tool $CompileLauncher @('headless-compile', $CompileIde, $output, $artifact,
                    '--target', 'ecom', '--result', $compileResult, '--timeout', '120') (Join-Path $caseRoot 'compile.log')
                $compilePassed = $false
                if ($compile.Code -ne 0 -or -not (Test-Path -LiteralPath $compileResult)) {
                    throw "IDE compile failed: $($compile.Text.Trim())"
                }
                $report = Get-Content -LiteralPath $compileResult -Raw | ConvertFrom-Json
                if (-not $report.ok -or -not $report.compile_result.artifact_verified -or
                    -not (Test-Path -LiteralPath $artifact) -or (Get-Item -LiteralPath $artifact).Length -eq 0) {
                    throw 'IDE did not verify a nonempty compiled artifact'
                }
                $compilePassed = $true
            }
        } catch { $failure = $_.Exception.Message }
        finally {
            foreach ($path in $originalBytes.Keys) { [IO.File]::WriteAllBytes($path, $originalBytes[$path]) }
        }
        $results.Add([pscustomobject]@{
            Architecture = $arch; Page = $page.relativePath; Marker = $marker
            ModifiedPages = $page.sourcePages.Count
            PackExit = $packCode; CompilePassed = $compilePassed; Passed = ($failure -eq ''); Failure = $failure; Directory = $caseRoot
        })
        $results | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath (Join-Path $OutputRoot 'results.json') -Encoding utf8
        Write-Host "$arch $id/$($cases.Count) $($page.logicalName): passed=$($failure -eq '')"
    }
}
$failed = @($results | Where-Object { -not $_.Passed }).Count
if ((Get-FileHash -LiteralPath $InputFile).Hash -ne $originalHash) { throw 'Original input changed during test' }
Write-Host "Edited roundtrip: total=$($results.Count), failed=$failed, results=$OutputRoot"
if ($failed -gt 0) { exit 1 }
