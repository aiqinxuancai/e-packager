param(
    [Parameter(Mandatory = $true)][string]$AuditRoot,
    [Parameter(Mandatory = $true)][string]$InputFile,
    [Parameter(Mandatory = $true)][string]$CombinedRoot
)
$ErrorActionPreference = 'Stop'
$AuditRoot = [IO.Path]::GetFullPath($AuditRoot)
$workers = @(Get-Content (Join-Path $AuditRoot 'workers.json') -Raw | ConvertFrom-Json)
$results = @(Get-Content (Join-Path $AuditRoot 'results.json') -Raw | ConvertFrom-Json)
$meta = Get-Content (Join-Path $workers[0].Root 'workspace/project/_meta.json') -Raw | ConvertFrom-Json
$pages = @($meta.sourceFiles.relativePath)
$originalHash = (Get-FileHash -LiteralPath $InputFile).Hash
function Assert-Artifact($Result) {
    if (-not $Result.Passed -or -not $Result.CompilePassed) { throw "Failed case: $($Result.Directory)" }
    $report = Get-Content (Join-Path $Result.Directory 'compile-result.json') -Raw | ConvertFrom-Json
    $artifact = $report.compile_result.output_path
    $caseRoot = [IO.Path]::GetFullPath($Result.Directory).TrimEnd('\', '/') + [IO.Path]::DirectorySeparatorChar
    if (-not $report.ok -or -not $report.compile_result.artifact_verified -or
        -not [IO.Path]::GetFullPath($artifact).StartsWith($caseRoot, [StringComparison]::OrdinalIgnoreCase) -or
        -not (Test-Path -LiteralPath $artifact -PathType Leaf) -or (Get-Item -LiteralPath $artifact).Length -eq 0) {
        throw "Invalid compile artifact: $($Result.Directory)"
    }
}
function Assert-ResourceIndexes([string]$Workspace, [string]$Decoded) {
    foreach ($kind in @('image', 'audio')) {
        $expected = Get-Content (Join-Path $Workspace "$kind/list.json") -Raw | ConvertFrom-Json
        $actual = Get-Content (Join-Path $Decoded "$kind/list.json") -Raw | ConvertFrom-Json
        if (($expected | ConvertTo-Json -Depth 10 -Compress) -cne ($actual | ConvertTo-Json -Depth 10 -Compress)) {
            throw "Resource index mismatch: $Decoded/$kind"
        }
    }
}
foreach ($worker in $workers) {
    if ((Get-FileHash (Join-Path $worker.Root 'original.e')).Hash -ne $originalHash) { throw 'Input copy changed' }
    $baseline = Join-Path $worker.Root "$($worker.Architecture)/baseline.e"
    if ((Get-FileHash $baseline).Hash -ne $originalHash) { throw 'Unchanged roundtrip mismatch' }
}
foreach ($arch in @('Win32', 'x64')) {
    $items = @($results | Where-Object Architecture -eq $arch)
    if ($items.Count -ne $pages.Count -or (Compare-Object ($items.Page | Sort-Object -Unique) ($pages | Sort-Object))) {
        throw "$arch page coverage incomplete or duplicated"
    }
    foreach ($item in $items) {
        if ($item.ModifiedPages -ne 1) { throw 'Independent page case modified an incorrect number of pages' }
        Assert-Artifact $item
        $workerRoot = Split-Path -Parent (Split-Path -Parent $item.Directory)
        Assert-ResourceIndexes (Join-Path $workerRoot 'workspace') (Join-Path $item.Directory 'decoded')
        $sourceFile = Join-Path $item.Directory "decoded/$($item.Page)"
        if (-not ([IO.File]::ReadAllText($sourceFile)).Contains($item.Marker)) { throw 'Edited marker missing' }
        if ((Get-FileHash (Join-Path $item.Directory 'edited.e')).Hash -eq $originalHash) { throw 'Edited output reused original bytes' }
    }
}
foreach ($group in ($results | Group-Object Page)) {
    if ($group.Count -ne 2) { throw "Missing architecture pair: $($group.Name)" }
    $hashes = @($group.Group | ForEach-Object { (Get-FileHash (Join-Path $_.Directory 'edited.e')).Hash } | Sort-Object -Unique)
    if ($hashes.Count -ne 1) { throw "Architecture output differs: $($group.Name)" }
}
$combined = @(Get-Content (Join-Path $CombinedRoot 'results.json') -Raw | ConvertFrom-Json)
if ($combined.Count -ne 2 -or @($combined.Architecture | Sort-Object -Unique).Count -ne 2) { throw 'Combined audit incomplete' }
foreach ($item in $combined) {
    Assert-Artifact $item
    if ($item.ModifiedPages -ne $pages.Count) { throw 'Combined audit did not modify every page' }
    Assert-ResourceIndexes (Join-Path $CombinedRoot 'workspace') (Join-Path $item.Directory 'decoded')
    foreach ($page in $pages) {
        if (-not ([IO.File]::ReadAllText((Join-Path $item.Directory "decoded/$page"))).Contains($item.Marker)) {
            throw "Combined edit missing: $page"
        }
    }
}
$combinedHashes = @($combined | ForEach-Object { (Get-FileHash (Join-Path $_.Directory 'edited.e')).Hash } | Sort-Object -Unique)
if ($combinedHashes.Count -ne 1) { throw 'Combined architecture output differs' }
$summary = [pscustomobject]@{
    OriginalSHA256 = $originalHash; PagesPerArchitecture = $pages.Count; IndependentCases = $results.Count
    CompiledCases = $results.Count + $combined.Count; ArchitecturePairsIdentical = $pages.Count
    ResourceIndexesVerified = $true; CombinedSHA256 = $combinedHashes[0]; VerifiedAt = (Get-Date).ToString('o')
}
$summary | ConvertTo-Json | Set-Content (Join-Path $AuditRoot 'verification.json') -Encoding utf8
$summary | Format-List
