param(
    [Parameter(Mandatory = $true)][string]$InputFile,
    [Parameter(Mandatory = $true)][string]$OutputRoot,
    [Parameter(Mandatory = $true)][string]$CompileIde,
    [Parameter(Mandatory = $true)][string]$CompileLauncher,
    [int]$PageCount = 105,
    [ValidateRange(1, 16)][int]$Partitions = 4
)
$ErrorActionPreference = 'Stop'
$OutputRoot = [IO.Path]::GetFullPath($OutputRoot)
if (Test-Path -LiteralPath $OutputRoot) { throw "Output already exists: $OutputRoot" }
[IO.Directory]::CreateDirectory($OutputRoot) | Out-Null
$script = Join-Path $PSScriptRoot 'TestEditedSourceRoundTrip.ps1'
$workers = [Collections.Generic.List[object]]::new()
$size = [int][Math]::Ceiling($PageCount / $Partitions)
foreach ($arch in @('Win32', 'x64')) {
    for ($part = 0; $part -lt $Partitions; $part++) {
        $start = $part * $size + 1
        if ($start -gt $PageCount) { break }
        $count = [Math]::Min($size, $PageCount - $start + 1)
        $root = Join-Path $OutputRoot "$arch-$start"
        $arguments = @('-NoProfile', '-File', $script, '-InputFile', $InputFile,
            '-OutputRoot', $root, '-Architectures', $arch, '-StartPage', "$start", '-Limit', "$count",
            '-CompileIde', $CompileIde, '-CompileLauncher', $CompileLauncher)
        $quoted = @($arguments | ForEach-Object { '"' + $_.Replace('"', '\"') + '"' })
        $process = Start-Process -FilePath (Join-Path $PSHOME 'pwsh.exe') -ArgumentList $quoted `
            -WindowStyle Hidden -PassThru -RedirectStandardOutput "$root.stdout.log" -RedirectStandardError "$root.stderr.log"
        $workers.Add([pscustomobject]@{ Process = $process; Root = $root; Architecture = $arch; Start = $start; Count = $count })
    }
}
$workers | Select-Object Root,Architecture,Start,Count,@{n='ProcessId';e={$_.Process.Id}} |
    ConvertTo-Json | Set-Content (Join-Path $OutputRoot 'workers.json') -Encoding utf8
do {
    $live = @($workers | Where-Object { -not $_.Process.HasExited })
    $results = @($workers | ForEach-Object {
        $file = Join-Path $_.Root 'results.json'
        if (Test-Path -LiteralPath $file) {
            try { Get-Content -LiteralPath $file -Raw | ConvertFrom-Json } catch { }
        }
    })
    Write-Host "Completed=$($results.Count)/$($PageCount * 2) failed=$(@($results | Where-Object { -not $_.Passed }).Count) live=$($live.Count)"
    if ($live.Count) { Start-Sleep -Seconds 20 }
} while ($live.Count)
$results = @($workers | ForEach-Object {
    if ($_.Process.ExitCode -ne 0) { throw "Partition failed: $($_.Root), exit=$($_.Process.ExitCode)" }
    $items = @(Get-Content -LiteralPath (Join-Path $_.Root 'results.json') -Raw | ConvertFrom-Json)
    if ($items.Count -ne $_.Count) { throw "Partition incomplete: $($_.Root)" }
    $items
})
if ($results.Count -ne $PageCount * 2 -or @($results | Where-Object { -not $_.Passed -or -not $_.CompilePassed }).Count) {
    throw 'Full audit failed or incomplete'
}
$results | ConvertTo-Json -Depth 6 | Set-Content (Join-Path $OutputRoot 'results.json') -Encoding utf8
Write-Host "PASS all $($results.Count) independently edited pages including IDE compilation"
