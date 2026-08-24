[CmdletBinding()]
param(
	[string]$Win32PackagerPath = "",
	[string]$X64PackagerPath = "",
	[string]$TemplatePath = ""
)

$ErrorActionPreference = "Stop"
$PSNativeCommandUseErrorActionPreference = $false

$repoRoot = Split-Path -Parent $PSScriptRoot
if ([string]::IsNullOrWhiteSpace($Win32PackagerPath)) {
	$Win32PackagerPath = Join-Path $repoRoot "bin\Win32\Release\e-packager.exe"
}
if ([string]::IsNullOrWhiteSpace($X64PackagerPath)) {
	$X64PackagerPath = Join-Path $repoRoot "bin\x64\Release\e-packager.exe"
}
if ([string]::IsNullOrWhiteSpace($TemplatePath)) {
	$TemplatePath = Join-Path $repoRoot "eproj\e-console-exe-new-proj.e"
}

$Win32PackagerPath = [System.IO.Path]::GetFullPath($Win32PackagerPath)
$X64PackagerPath = [System.IO.Path]::GetFullPath($X64PackagerPath)
$TemplatePath = [System.IO.Path]::GetFullPath($TemplatePath)
foreach ($path in @($Win32PackagerPath, $X64PackagerPath, $TemplatePath)) {
	if (-not (Test-Path -LiteralPath $path -PathType Leaf)) {
		throw "Required file not found: $path"
	}
}

function Invoke-Packager {
	param(
		[Parameter(Mandatory)][string]$Executable,
		[Parameter(Mandatory)][string[]]$Arguments
	)

	$output = & $Executable @Arguments 2>&1 | Out-String
	if ($LASTEXITCODE -ne 0) {
		throw "e-packager failed with exit code $LASTEXITCODE`ncommand: $Executable $($Arguments -join ' ')`n$output"
	}
	return $output.Trim()
}

function Write-Utf8BomFile {
	param(
		[Parameter(Mandatory)][string]$Path,
		[Parameter(Mandatory)][string]$Content
	)

	$normalized = $Content -replace "`r?`n", "`r`n"
	[System.IO.File]::WriteAllText($Path, $normalized, [System.Text.UTF8Encoding]::new($true))
}

$source = @"
.版本 2

.程序集 程序集1

.子程序 _启动子程序, 整数型
.局部变量 文本数组, 文本型, , "0"
.局部变量 数量, 整数型

文本数组 ＝ 分割文本 (“甲,乙”, “,”, )
数量 ＝ 取数组成员数 (文本数组)
返回 (0)
"@

$tempRoot = Join-Path ([System.IO.Path]::GetTempPath()) ("e-packager-support-command-resolution-" + [guid]::NewGuid().ToString("N"))
$workspace = Join-Path $tempRoot "workspace"
$win32Output = Join-Path $tempRoot "win32.e"
$x64Output = Join-Path $tempRoot "x64.e"
[System.IO.Directory]::CreateDirectory($tempRoot) | Out-Null

try {
	# Win32 reads the native .fne metadata; x64 must fall back to this exported text workspace.
	Invoke-Packager -Executable $Win32PackagerPath -Arguments @("unpack", $TemplatePath, $workspace) | Out-Null
	Write-Utf8BomFile -Path (Join-Path $workspace "src\程序集1.txt") -Content $source

	Invoke-Packager -Executable $Win32PackagerPath -Arguments @("pack", $workspace, $win32Output) | Out-Null
	Invoke-Packager -Executable $X64PackagerPath -Arguments @("pack", $workspace, $x64Output) | Out-Null

	$win32Hash = (Get-FileHash -LiteralPath $win32Output -Algorithm SHA256).Hash
	$x64Hash = (Get-FileHash -LiteralPath $x64Output -Algorithm SHA256).Hash
	if ($win32Hash -ne $x64Hash) {
		throw "Support command resolution differs between native and text metadata paths: Win32=$win32Hash x64=$x64Hash"
	}

	Invoke-Packager -Executable $Win32PackagerPath -Arguments @("unpack", $win32Output, (Join-Path $tempRoot "win32-roundtrip"), "--main-only") | Out-Null
	Invoke-Packager -Executable $X64PackagerPath -Arguments @("unpack", $x64Output, (Join-Path $tempRoot "x64-roundtrip"), "--main-only") | Out-Null
	Write-Host "PASS support command resolution parity SHA256=$win32Hash"
}
finally {
	$resolvedTempRoot = [System.IO.Path]::GetFullPath($tempRoot)
	$systemTempRoot = [System.IO.Path]::GetFullPath([System.IO.Path]::GetTempPath())
	if ($resolvedTempRoot.StartsWith($systemTempRoot, [System.StringComparison]::OrdinalIgnoreCase) -and
		[System.IO.Path]::GetFileName($resolvedTempRoot).StartsWith("e-packager-support-command-resolution-", [System.StringComparison]::Ordinal)) {
		Remove-Item -LiteralPath $resolvedTempRoot -Recurse -Force
	}
}
