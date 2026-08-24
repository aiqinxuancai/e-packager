[CmdletBinding()]
param(
	[string]$PackagerPath = "",
	[string]$TemplatePath = ""
)

$ErrorActionPreference = "Stop"
$PSNativeCommandUseErrorActionPreference = $false

$repoRoot = Split-Path -Parent $PSScriptRoot
if ([string]::IsNullOrWhiteSpace($PackagerPath)) {
	$PackagerPath = Join-Path $repoRoot "bin\Win32\Release\e-packager.exe"
}
if ([string]::IsNullOrWhiteSpace($TemplatePath)) {
	$TemplatePath = Join-Path $repoRoot "eproj\e-console-exe-new-proj.e"
}

$PackagerPath = [System.IO.Path]::GetFullPath($PackagerPath)
$TemplatePath = [System.IO.Path]::GetFullPath($TemplatePath)
if (-not (Test-Path -LiteralPath $PackagerPath -PathType Leaf)) {
	throw "e-packager executable not found: $PackagerPath"
}
if (-not (Test-Path -LiteralPath $TemplatePath -PathType Leaf)) {
	throw "Easy Language template not found: $TemplatePath"
}

function Invoke-Packager {
	param([Parameter(Mandatory)][string[]]$Arguments)

	$output = & $PackagerPath @Arguments 2>&1 | Out-String
	if ($LASTEXITCODE -ne 0) {
		throw "e-packager failed with exit code $LASTEXITCODE`ncommand: $PackagerPath $($Arguments -join ' ')`n$output"
	}
	return $output.Trim()
}

function Invoke-PackagerExpectFailure {
	param([Parameter(Mandatory)][string[]]$Arguments)

	$output = & $PackagerPath @Arguments 2>&1 | Out-String
	if ($LASTEXITCODE -eq 0) {
		throw "e-packager unexpectedly succeeded`ncommand: $PackagerPath $($Arguments -join ' ')`n$output"
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

function Add-ProgramPage {
	param(
		[Parameter(Mandatory)][string]$Workspace,
		[Parameter(Mandatory)][string]$LogicalName,
		[Parameter(Mandatory)][string]$Content
	)

	$key = "class:$LogicalName"
	$relativePath = "src/$LogicalName.txt"
	Write-Utf8BomFile -Path (Join-Path $Workspace $relativePath) -Content $Content

	$metadataPath = Join-Path $Workspace "project\_meta.json"
	$metadata = Get-Content -Raw -Encoding UTF8 -LiteralPath $metadataPath | ConvertFrom-Json
	$metadata.sourceFiles = @($metadata.sourceFiles) + [pscustomobject]@{
		key = $key
		logicalName = $LogicalName
		relativePath = $relativePath
	}
	$metadata.rootChildKeys = @($metadata.rootChildKeys) + $key
	Write-Utf8BomFile -Path $metadataPath -Content ($metadata | ConvertTo-Json -Depth 20)
}

$cases = @(
	[pscustomobject]@{
		Name = "forward_call"
		Source = @"
.版本 2

.程序集 程序集1
.程序集变量 计数, 整数型

.子程序 _启动子程序, 整数型

前向目标 ()
返回 (0)

.子程序 前向目标

计数 ＝ 计数 ＋ 1
"@
		SecondPage = $null
	},
	[pscustomobject]@{
		Name = "backward_call"
		Source = @"
.版本 2

.程序集 程序集1

.子程序 后向目标


.子程序 _启动子程序, 整数型

后向目标 ()
返回 (0)
"@
		SecondPage = $null
	},
	[pscustomobject]@{
		Name = "self_recursion"
		Source = @"
.版本 2

.程序集 程序集1

.子程序 _启动子程序, 整数型

自递归 ()
返回 (0)

.子程序 自递归

自递归 ()
"@
		SecondPage = $null
	},
	[pscustomobject]@{
		Name = "mutual_recursion"
		Source = @"
.版本 2

.程序集 程序集1

.子程序 _启动子程序, 整数型

递归甲 ()
返回 (0)

.子程序 递归甲

递归乙 ()

.子程序 递归乙

递归甲 ()
"@
		SecondPage = $null
	},
	[pscustomobject]@{
		Name = "cross_page_call"
		Source = @"
.版本 2

.程序集 程序集1

.子程序 _启动子程序, 整数型

跨页目标 ()
返回 (0)
"@
		SecondPage = @"
.版本 2

.程序集 程序集2

.子程序 跨页目标

"@
	}
)

$tempRoot = Join-Path ([System.IO.Path]::GetTempPath()) ("e-packager-function-resolution-" + [guid]::NewGuid().ToString("N"))
[System.IO.Directory]::CreateDirectory($tempRoot) | Out-Null

try {
	foreach ($case in $cases) {
		$caseRoot = Join-Path $tempRoot $case.Name
		$workspace = Join-Path $caseRoot "workspace"
		$roundtripWorkspace = Join-Path $caseRoot "roundtrip"
		$outputFile = Join-Path $caseRoot "$($case.Name).e"

		Invoke-Packager -Arguments @("unpack", $TemplatePath, $workspace) | Out-Null
		Write-Utf8BomFile -Path (Join-Path $workspace "src\程序集1.txt") -Content $case.Source
		if ($null -ne $case.SecondPage) {
			Add-ProgramPage -Workspace $workspace -LogicalName "程序集2" -Content $case.SecondPage
		}

		Invoke-Packager -Arguments @("validate", $workspace) | Out-Null
		Invoke-Packager -Arguments @("pack", $workspace, $outputFile) | Out-Null
		Invoke-Packager -Arguments @("unpack", $outputFile, $roundtripWorkspace) | Out-Null
		Write-Host "PASS $($case.Name)"
	}

	$unknownRoot = Join-Path $tempRoot "unknown_function"
	$unknownWorkspace = Join-Path $unknownRoot "workspace"
	$unknownOutput = Join-Path $unknownRoot "unknown_function.e"
	Invoke-Packager -Arguments @("unpack", $TemplatePath, $unknownWorkspace) | Out-Null
	Write-Utf8BomFile -Path (Join-Path $unknownWorkspace "src\程序集1.txt") -Content @"
.版本 2

.程序集 程序集1

.子程序 _启动子程序, 整数型

确定不存在的函数 ()
返回 (0)
"@
	$unknownFailure = Invoke-PackagerExpectFailure -Arguments @("pack", $unknownWorkspace, $unknownOutput)
	if ($unknownFailure -notmatch "code=call_not_found") {
		throw "Unknown function did not produce the expected call_not_found preflight diagnostic:`n$unknownFailure"
	}
	Write-Host "PASS unknown_function_is_rejected"

	Write-Host "All $($cases.Count) local function resolution cases and the unknown-function negative case passed."
}
finally {
	$resolvedTempRoot = [System.IO.Path]::GetFullPath($tempRoot)
	$systemTempRoot = [System.IO.Path]::GetFullPath([System.IO.Path]::GetTempPath())
	if ($resolvedTempRoot.StartsWith($systemTempRoot, [System.StringComparison]::OrdinalIgnoreCase) -and
		[System.IO.Path]::GetFileName($resolvedTempRoot).StartsWith("e-packager-function-resolution-", [System.StringComparison]::Ordinal)) {
		Remove-Item -LiteralPath $resolvedTempRoot -Recurse -Force
	}
}
