param(
    [string]$Compiler = "$PSScriptRoot/../temp/build/Win32/e-packager.exe",
    [string]$EDirectory = 'C:\Users\aiqin\OneDrive\e5.6',
    [string]$CoreDirectory = 'D:\git\BlackMoonKernelStaticLib\adapter',
    [string]$OutputRoot = "$PSScriptRoot/../temp/native-x86-abi-$([guid]::NewGuid().ToString('N'))"
)
$ErrorActionPreference = 'Stop'
$repo = Split-Path -Parent $PSScriptRoot
$OutputRoot = [IO.Path]::GetFullPath($OutputRoot)
$workspace = Join-Path $OutputRoot 'workspace'
& $Compiler unpack (Join-Path $repo 'eproj/e-console-exe-new-proj.e') $workspace
if ($LASTEXITCODE -ne 0) { throw 'ABI fixture unpack failed' }
Get-ChildItem -LiteralPath "$PSScriptRoot/fixtures/native-x86-abi" -Force -File |
    Copy-Item -Destination (Join-Path $workspace 'src')
$executable = Join-Path $OutputRoot 'native-abi.exe'
& $Compiler compile $workspace $executable --arch x86 --e-dir $EDirectory --blackmoon-x86-dir $CoreDirectory --diagnostics json
if ($LASTEXITCODE -ne 0) { throw 'ABI fixture compilation failed' }
$process = Start-Process -FilePath $executable -WorkingDirectory $OutputRoot -WindowStyle Hidden -PassThru
try {
    if (-not $process.WaitForExit(15000)) {
        Stop-Process -Id $process.Id
        throw 'ABI fixture timed out'
    }
    if ($process.ExitCode -ne 0) { throw "ABI fixture failed: exit=$($process.ExitCode). See numbered checks in fixtures/native-x86-abi/程序集1.txt" }
} finally { $process.Dispose() }
Write-Host 'PASS x86 native frames, references, optional arguments, class callbacks, static locals, binary data and DLL structures'
