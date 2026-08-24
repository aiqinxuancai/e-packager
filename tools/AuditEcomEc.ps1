param(
    [Parameter(Mandatory = $true)]
    [string]$SourceRoot,
    [Parameter(Mandatory = $true)]
    [string]$AuditRoot,
    [Parameter(Mandatory = $true)]
    [string]$ExePath,
    [int]$ThrottleLimit = 4
)

$ErrorActionPreference = 'Stop'
$SourceRoot = [IO.Path]::GetFullPath($SourceRoot)
$AuditRoot = [IO.Path]::GetFullPath($AuditRoot)
$ExePath = [IO.Path]::GetFullPath($ExePath)
$workRoot = Join-Path $AuditRoot 'work'
$repackedRoot = Join-Path $AuditRoot 'repacked'
$logRoot = Join-Path $AuditRoot 'logs'
New-Item -ItemType Directory -Force -Path $workRoot, $repackedRoot, $logRoot | Out-Null

$allFiles = @(Get-ChildItem -LiteralPath $SourceRoot -Recurse -File -Filter '*.ec' | Sort-Object FullName)
$zeroFiles = @($allFiles | Where-Object Length -eq 0)
$files = @($allFiles | Where-Object Length -gt 0)

$manifest = for ($i = 0; $i -lt $files.Count; $i++) {
    $file = $files[$i]
    $relative = [IO.Path]::GetRelativePath($SourceRoot, $file.FullName)
    $id = '{0:D4}' -f ($i + 1)
    [pscustomobject]@{
        Id = $id
        Source = $file.FullName
        Relative = $relative
        Length = [int64]$file.Length
        WorkDir = Join-Path $workRoot $id
        Output = Join-Path $repackedRoot ($id + '.e')
        LogDir = Join-Path $logRoot $id
    }
}
$manifest | ConvertTo-Json -Depth 3 | Set-Content -LiteralPath (Join-Path $AuditRoot 'manifest.json') -Encoding utf8
$zeroFiles | Select-Object FullName, Length | ConvertTo-Json -Depth 2 | Set-Content -LiteralPath (Join-Path $AuditRoot 'skipped-zero.json') -Encoding utf8

$results = @($manifest | ForEach-Object -Parallel {
    $item = $_
    $ErrorActionPreference = 'Continue'
    New-Item -ItemType Directory -Force -Path $item.WorkDir, $item.LogDir | Out-Null

    function Invoke-Stage([string]$Name, [string[]]$Arguments) {
        $text = (& $using:ExePath @Arguments 2>&1 | Out-String)
        $code = $LASTEXITCODE
        Set-Content -LiteralPath (Join-Path $item.LogDir ($Name + '.log')) -Value $text -Encoding utf8
        [pscustomobject]@{ Code = $code; Text = $text }
    }

    $unpack = Invoke-Stage 'unpack' @('unpack', $item.Source, $item.WorkDir)
    $validate = $null
    $pack = $null
    $compare = $null
    if ($unpack.Code -eq 0) {
        $validate = Invoke-Stage 'validate' @('validate', $item.WorkDir)
        $pack = Invoke-Stage 'pack' @('pack', $item.WorkDir, $item.Output)
        $compare = Invoke-Stage 'compare-bundle' @('compare-bundle', $item.Source, $item.WorkDir)
    }

    $validateErrors = $null
    $validateWarnings = $null
    if ($validate) {
        $m = [regex]::Match($validate.Text, 'errors=(\d+), warnings=(\d+)')
        if ($m.Success) {
            $validateErrors = [int]$m.Groups[1].Value
            $validateWarnings = [int]$m.Groups[2].Value
        }
    }
    $match = $null
    if ($compare) {
        $m = [regex]::Match($compare.Text, 'match=(true|false)')
        if ($m.Success) { $match = [bool]::Parse($m.Groups[1].Value) }
    }

    [pscustomobject]@{
        Id = $item.Id
        Source = $item.Source
        Relative = $item.Relative
        Length = $item.Length
        UnpackCode = $unpack.Code
        ValidateCode = if ($validate) { $validate.Code } else { $null }
        ValidateErrors = $validateErrors
        ValidateWarnings = $validateWarnings
        PackCode = if ($pack) { $pack.Code } else { $null }
        CompareCode = if ($compare) { $compare.Code } else { $null }
        CompareMatch = $match
    }
} -ThrottleLimit $ThrottleLimit)

$results = @($results | Sort-Object Id)
$results | ConvertTo-Json -Depth 3 | Set-Content -LiteralPath (Join-Path $AuditRoot 'results.json') -Encoding utf8
$summary = [pscustomobject]@{
    Total = $allFiles.Count
    NonZero = $files.Count
    SkippedZero = $zeroFiles.Count
    UnpackSuccess = @($results | Where-Object UnpackCode -eq 0).Count
    ValidateSuccess = @($results | Where-Object { $_.ValidateCode -eq 0 -and $_.ValidateErrors -eq 0 }).Count
    ValidateFailures = @($results | Where-Object { $_.ValidateCode -ne 0 -or $_.ValidateErrors -gt 0 }).Count
    PackSuccess = @($results | Where-Object PackCode -eq 0).Count
    CompareSuccess = @($results | Where-Object { $_.CompareCode -eq 0 -and $_.CompareMatch -eq $true }).Count
    FailedItems = @($results | Where-Object { $_.UnpackCode -ne 0 -or $_.ValidateCode -ne 0 -or $_.ValidateErrors -gt 0 -or $_.PackCode -ne 0 -or $_.CompareCode -ne 0 -or $_.CompareMatch -ne $true })
}
$summary | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath (Join-Path $AuditRoot 'summary.json') -Encoding utf8
$summary | Select-Object Total, NonZero, SkippedZero, UnpackSuccess, ValidateSuccess, ValidateFailures, PackSuccess, CompareSuccess | Format-List
if ($summary.FailedItems.Count -gt 0) {
    'Failed items:'
    $summary.FailedItems | Select-Object Id, Relative, UnpackCode, ValidateCode, ValidateErrors, ValidateWarnings, PackCode, CompareCode, CompareMatch | Format-Table -AutoSize
    exit 2
}
