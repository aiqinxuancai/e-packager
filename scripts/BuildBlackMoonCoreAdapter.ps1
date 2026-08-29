param(
    [string]$SourceRoot = '',
    [string]$ModernCoreRoot = '',
    [string]$FallbackLibrary = '',
    [string]$MetadataFne = '',
    [string]$OutputRoot = '',
    [string]$CompilerPath = ''
)

$ErrorActionPreference = 'Stop'

try {
    [Text.Encoding]::RegisterProvider([Text.CodePagesEncodingProvider]::Instance)
} catch {
    # Windows PowerShell already exposes the system code pages.
}

$repositoryRoot = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
if ([string]::IsNullOrWhiteSpace($SourceRoot)) {
    $SourceRoot = Join-Path $repositoryRoot 'krnln'
}
if ([string]::IsNullOrWhiteSpace($OutputRoot)) {
    $OutputRoot = Join-Path $repositoryRoot 'adapter\static_lib\x64'
}
if ([string]::IsNullOrWhiteSpace($ModernCoreRoot)) {
    $siblingModernCore = Join-Path (Split-Path -Parent $repositoryRoot) 'ycIDE-electron'
    if (Test-Path -LiteralPath $siblingModernCore -PathType Container) {
        $ModernCoreRoot = $siblingModernCore
    }
}
if ([string]::IsNullOrWhiteSpace($FallbackLibrary) -and -not [string]::IsNullOrWhiteSpace($ModernCoreRoot)) {
    $FallbackLibrary = Join-Path $ModernCoreRoot 'static_lib\x64\krnln_static.lib'
}
if ([string]::IsNullOrWhiteSpace($MetadataFne) -and -not [string]::IsNullOrWhiteSpace($ModernCoreRoot)) {
    $MetadataFne = Join-Path $ModernCoreRoot 'lib\x64\krnln.fne'
}

function Find-X64Compiler {
    param([string]$RequestedPath)

    if (-not [string]::IsNullOrWhiteSpace($RequestedPath)) {
        if (-not (Test-Path -LiteralPath $RequestedPath -PathType Leaf)) {
            throw "指定的 x64 编译器不存在: $RequestedPath"
        }
        return (Resolve-Path -LiteralPath $RequestedPath).Path
    }

    if (-not [string]::IsNullOrWhiteSpace($env:VCToolsInstallDir)) {
        $candidate = Join-Path $env:VCToolsInstallDir 'bin\Hostx64\x64\cl.exe'
        if (Test-Path -LiteralPath $candidate -PathType Leaf) {
            return $candidate
        }
    }

    $vsRoot = 'C:\Program Files\Microsoft Visual Studio'
    $candidates = Get-ChildItem -Path $vsRoot -Recurse -Filter 'cl.exe' -File -ErrorAction SilentlyContinue |
        Where-Object { $_.FullName -match '\\VC\\Tools\\MSVC\\[^\\]+\\bin\\Hostx64\\x64\\cl\.exe$' } |
        Sort-Object FullName -Descending
    if ($candidates.Count -eq 0) {
        throw '未找到 MSVC Hostx64/x64 cl.exe。请安装 C++ x64 工具集或传入 -CompilerPath。'
    }
    return $candidates[0].FullName
}

function Find-WindowsKitVersion {
    $includeRoot = 'C:\Program Files (x86)\Windows Kits\10\Include'
    $versions = Get-ChildItem -Path $includeRoot -Directory -ErrorAction SilentlyContinue |
        Where-Object { Test-Path -LiteralPath (Join-Path $_.FullName 'um\Windows.h') -PathType Leaf } |
        Sort-Object Name -Descending
    if ($versions.Count -eq 0) {
        throw '未找到 Windows SDK Include 目录。'
    }
    return $versions[0].Name
}

function Invoke-AdapterCompile {
    param(
        [string]$Compiler,
        [string]$Source,
        [string]$Object,
        [string[]]$IncludeDirectories,
        [string[]]$ExtraArguments = @(),
        [switch]$RejectPointerNarrowing
    )

    $arguments = @(
        '/nologo', '/c', '/O2', '/MT', '/std:c++20', '/EHsc',
        '/DWIN32', '/D_CRT_SECURE_NO_WARNINGS',
        '/source-charset:.936', '/execution-charset:.936'
    )
    foreach ($includeDirectory in $IncludeDirectories) {
        if (-not [string]::IsNullOrWhiteSpace($includeDirectory)) {
            $arguments += ('/I' + $includeDirectory)
        }
    }
    $arguments += $ExtraArguments
    $arguments += ('/Fo' + $Object)
    $arguments += $Source
    if ($RejectPointerNarrowing) {
        # C4311/C4312 identify conversions between a pointer and a 32-bit
        # integer. They are correctness errors in an x64 support library.
        $arguments = @('/we4311', '/we4312') + $arguments
    }
    $output = & $Compiler @arguments 2>&1
    return [PSCustomObject]@{
        Success = $LASTEXITCODE -eq 0
        Output = @($output)
    }
}

function Find-CppFunctionBodyEnd {
    param(
        [string]$Text,
        [int]$StartOffset
    )

    $state = 'code'
    $braceDepth = 0
    $bodyStarted = $false
    for ($index = $StartOffset; $index -lt $Text.Length; ++$index) {
        $current = $Text[$index]
        $next = if ($index + 1 -lt $Text.Length) { $Text[$index + 1] } else { [char]0 }
        if ($state -eq 'line-comment') {
            if ($current -eq "`n") { $state = 'code' }
            continue
        }
        if ($state -eq 'block-comment') {
            if ($current -eq '*' -and $next -eq '/') { $state = 'code'; ++$index }
            continue
        }
        if ($state -eq 'string') {
            if ($current -eq [char]92) { ++$index; continue }
            if ($current -eq '"') { $state = 'code' }
            continue
        }
        if ($state -eq 'character') {
            if ($current -eq [char]92) { ++$index; continue }
            if ($current -eq [char]39) { $state = 'code' }
            continue
        }
        if ($current -eq '/' -and $next -eq '/') { $state = 'line-comment'; ++$index; continue }
        if ($current -eq '/' -and $next -eq '*') { $state = 'block-comment'; ++$index; continue }
        if ($current -eq '"') { $state = 'string'; continue }
        if ($current -eq [char]39) { $state = 'character'; continue }
        if ($current -eq '{') {
            $bodyStarted = $true
            ++$braceDepth
            continue
        }
        if ($current -eq '}' -and $bodyStarted) {
            --$braceDepth
            if ($braceDepth -eq 0) { return $index + 1 }
        }
    }
    throw "无法定位 C++ 函数结束位置: offset=$StartOffset"
}

function Remove-AdapterCommandsFromModernSource {
    param(
        [string]$SourceText,
        [System.Collections.Generic.HashSet[int]]$CommandIndexes
    )

    $matches = [regex]::Matches(
        $SourceText,
        '(?m)^\s*EXTERN_C\s+void\s+KRNLN_NAME\s*\(\s*(\d+)\s*,')
    for ($matchIndex = $matches.Count - 1; $matchIndex -ge 0; --$matchIndex) {
        $match = $matches[$matchIndex]
        $commandIndex = [int]$match.Groups[1].Value
        if (-not $CommandIndexes.Contains($commandIndex)) { continue }
        $endOffset = Find-CppFunctionBodyEnd -Text $SourceText -StartOffset $match.Index
        $SourceText = $SourceText.Remove($match.Index, $endOffset - $match.Index).
            Insert($match.Index, "`r`n/* BlackMoon adapter owns command $commandIndex. */`r`n")
    }
    return $SourceText
}

if (-not (Test-Path -LiteralPath $SourceRoot -PathType Container)) {
    throw "BlackMoonKernelStaticLib 核心源码目录不存在: $SourceRoot"
}
if (-not (Test-Path -LiteralPath $FallbackLibrary -PathType Leaf)) {
    throw "标准 ABI 兼容核心不存在: $FallbackLibrary"
}
if (-not (Test-Path -LiteralPath $MetadataFne -PathType Leaf)) {
    throw "匹配的 x64 核心 FNE 不存在: $MetadataFne"
}

$compiler = Find-X64Compiler $CompilerPath
$compilerDirectory = Split-Path -Parent $compiler
$libraryManager = Join-Path $compilerDirectory 'lib.exe'
if (-not (Test-Path -LiteralPath $libraryManager -PathType Leaf)) {
    throw "未找到 lib.exe: $libraryManager"
}
$coffDumper = Join-Path $compilerDirectory 'dumpbin.exe'
if (-not (Test-Path -LiteralPath $coffDumper -PathType Leaf)) {
    throw "未找到 dumpbin.exe: $coffDumper"
}

$vcTools = Split-Path -Parent (Split-Path -Parent (Split-Path -Parent (Split-Path -Parent $compiler)))
$kitVersion = Find-WindowsKitVersion
$kitRoot = 'C:\Program Files (x86)\Windows Kits\10'
$includeDirectories = @(
    (Join-Path $vcTools 'include'),
    (Join-Path $kitRoot "Include\$kitVersion\ucrt"),
    (Join-Path $kitRoot "Include\$kitVersion\shared"),
    (Join-Path $kitRoot "Include\$kitVersion\um")
)
$libraryDirectories = @(
    (Join-Path $vcTools 'lib\x64'),
    (Join-Path $kitRoot "Lib\$kitVersion\ucrt\x64"),
    (Join-Path $kitRoot "Lib\$kitVersion\um\x64")
)
foreach ($path in $includeDirectories + $libraryDirectories) {
    if (-not (Test-Path -LiteralPath $path -PathType Container)) {
        throw "x64 工具链目录不存在: $path"
    }
}

New-Item -ItemType Directory -Path $OutputRoot -Force | Out-Null
$adapterProductRoot = Split-Path -Parent (Split-Path -Parent $OutputRoot)
$metadataDirectory = Join-Path $adapterProductRoot 'lib\x64'
New-Item -ItemType Directory -Path $metadataDirectory -Force | Out-Null
Copy-Item -LiteralPath $MetadataFne -Destination (Join-Path $metadataDirectory 'krnln.fne') -Force
$workRoot = Join-Path $OutputRoot '.adapter-build'
if (Test-Path -LiteralPath $workRoot) {
    # This directory is wholly generated by this script. Removing it avoids
    # stale fallback objects being accidentally archived as adapter objects.
    Remove-Item -LiteralPath $workRoot -Recurse -Force
}
New-Item -ItemType Directory -Path $workRoot -Force | Out-Null
$generator = Join-Path $PSScriptRoot 'BuildBlackMoonAdapterProto.ps1'
if (-not (Test-Path -LiteralPath $generator -PathType Leaf)) {
    throw "未找到适配源码生成器: $generator"
}

$savedInclude = $env:INCLUDE
$savedLib = $env:LIB
try {
    $env:INCLUDE = $includeDirectories -join ';'
    $env:LIB = $libraryDirectories -join ';'

    $targetSymbolMap = Join-Path $workRoot 'target-core-symbols.txt'
    $targetSymbolOutput = @(& $coffDumper '/linkermember:1' $FallbackLibrary 2>&1 | ForEach-Object { [string]$_ })
    if ($LASTEXITCODE -ne 0) {
        throw "读取目标核心的 COFF 符号表失败:`n$($targetSymbolOutput -join "`n")"
    }
    [IO.File]::WriteAllLines($targetSymbolMap, $targetSymbolOutput, [Text.UTF8Encoding]::new($true))

    # First generation discovers every source whose command name can be
    # matched to the target FNE metadata. The compiler then decides which
    # implementations are x64-safe; no command-specific behavior is coded
    # into this script.
    & $generator -SourceRoot $SourceRoot -OutputRoot $workRoot -ModernCoreRoot $ModernCoreRoot -TargetSymbolMapPath $targetSymbolMap | Out-Null
    if (-not $?) {
        throw '生成 BlackMoon 核心适配源码失败。'
    }

    $probeDirectory = Join-Path $workRoot 'probe'
    New-Item -ItemType Directory -Path $probeDirectory -Force | Out-Null
    $candidateSources = @(
        Get-Content -LiteralPath (Join-Path $workRoot 'adapter-source-files.txt') -Encoding UTF8 |
            Where-Object { $_ -and $_ -ne 'bm_runtime.cpp' } |
            ForEach-Object { [string]$_ }
    )
    $eligibleSources = [System.Collections.Generic.List[string]]::new()
    $rejectedSources = [System.Collections.Generic.List[object]]::new()
    foreach ($sourceName in $candidateSources) {
        $sourcePath = Join-Path $workRoot $sourceName
        $objectPath = Join-Path $probeDirectory (($sourceName -replace '\.cpp$', '.obj'))
        $probe = Invoke-AdapterCompile -Compiler $compiler -Source $sourcePath -Object $objectPath -IncludeDirectories @($workRoot) -RejectPointerNarrowing
        if ($probe.Success) {
            $eligibleSources.Add($sourceName)
            continue
        }
        $diagnostic = $probe.Output | Where-Object { $_ -match 'error C|fatal error' } | Select-Object -First 1
        $rejectedSources.Add([PSCustomObject]@{
            source = $sourceName
            diagnostic = if ($diagnostic) { [string]$diagnostic } else { ($probe.Output -join "`n") }
        })
    }
    if ($eligibleSources.Count -eq 0) {
        throw '没有可用于 x64 的 BlackMoon 核心实现。'
    }

    $eligiblePath = Join-Path $workRoot 'adapter-eligible-sources.txt'
    [IO.File]::WriteAllLines($eligiblePath, $eligibleSources, [Text.UTF8Encoding]::new($true))
    & $generator -SourceRoot $SourceRoot -OutputRoot $workRoot -ModernCoreRoot $ModernCoreRoot -EligibleSourcesPath $eligiblePath -TargetSymbolMapPath $targetSymbolMap | Out-Null
    if (-not $?) {
        throw '根据 x64 能力检查重新生成适配源码失败。'
    }

    $legacyObjectDirectory = Join-Path $workRoot 'objects\legacy'
    $wrapperObjectDirectory = Join-Path $workRoot 'objects\wrappers'
    New-Item -ItemType Directory -Path $legacyObjectDirectory -Force | Out-Null
    New-Item -ItemType Directory -Path $wrapperObjectDirectory -Force | Out-Null

    $selectedSources = @(
        Get-Content -LiteralPath (Join-Path $workRoot 'adapter-source-files.txt') -Encoding UTF8 |
            ForEach-Object { [string]$_ }
    )
    foreach ($sourceName in $selectedSources) {
        $sourcePath = Join-Path $workRoot $sourceName
        $objectPath = Join-Path $legacyObjectDirectory (($sourceName -replace '\.cpp$', '.obj'))
        $result = Invoke-AdapterCompile -Compiler $compiler -Source $sourcePath -Object $objectPath -IncludeDirectories @($workRoot) -RejectPointerNarrowing
        if (-not $result.Success) {
            throw "编译 BlackMoon 适配源码失败: $sourceName`n$($result.Output -join "`n")"
        }
    }

    foreach ($wrapper in Get-ChildItem -LiteralPath (Join-Path $workRoot 'wrappers') -Filter '*.cpp') {
        $objectPath = Join-Path $wrapperObjectDirectory (($wrapper.Name -replace '\.cpp$', '.obj'))
        $result = Invoke-AdapterCompile -Compiler $compiler -Source $wrapper.FullName -Object $objectPath -IncludeDirectories @($workRoot)
        if (-not $result.Success) {
            throw "编译 BlackMoon ABI 包装失败: $($wrapper.Name)`n$($result.Output -join "`n")"
        }
    }

    $objects = Get-ChildItem -LiteralPath (Join-Path $workRoot 'objects') -Recurse -Filter '*.obj' |
        Select-Object -ExpandProperty FullName
    if ($objects.Count -eq 0) {
        throw 'BlackMoon ABI 适配对象为空。'
    }
    $archive = Join-Path $OutputRoot 'krnln_static.lib'
    # The adapter contains one wrapper object per legacy implementation; pass
    # the archive members through a response file to stay below CreateProcess'
    # command-line limit on Windows.
    $archiveResponse = Join-Path $workRoot 'archive.rsp'
    $archiveArguments = @('/NOLOGO', '/MACHINE:X64', ('/OUT:"' + $archive + '"')) + @($objects | ForEach-Object { '"' + $_ + '"' })
    [IO.File]::WriteAllLines($archiveResponse, $archiveArguments, [Text.Encoding]::Unicode)
    $archiveOutput = & $libraryManager ('@' + $archiveResponse) 2>&1
    if ($LASTEXITCODE -ne 0 -or -not (Test-Path -LiteralPath $archive -PathType Leaf)) {
        throw "生成 BlackMoon x64 核心静态库失败:`n$($archiveOutput -join "`n")"
    }

    # The modern compatibility archive keeps every core command in one object
    # file. Reusing it unchanged would pull all modern definitions as soon as
    # one command is needed, causing duplicate definitions with the adapter.
    # Rebuild only that object after removing the command indices exported by
    # the adapter; all other compatibility-runtime members are retained.
    $adapterCommandIndexes = [System.Collections.Generic.HashSet[int]]::new()
    foreach ($wrapper in Get-ChildItem -LiteralPath (Join-Path $workRoot 'wrappers') -Filter '*.cpp') {
        $wrapperText = [IO.File]::ReadAllText($wrapper.FullName, [Text.Encoding]::UTF8)
        foreach ($match in [regex]::Matches($wrapperText, '(?m)^extern\s+"C"\s+void\s+krnln_.*?_(\d+)_krnln\s*\(')) {
            [void]$adapterCommandIndexes.Add([int]$match.Groups[1].Value)
        }
    }
    # Some adapter features emit FNE ABI bridges directly instead of going
    # through a legacy wrapper. Treat the primary archive's exported command
    # symbols as authoritative so the filtered fallback cannot reintroduce
    # duplicate command definitions.
    $archiveSymbolOutput = @(& $coffDumper '/linkermember:1' $archive 2>&1 | ForEach-Object { [string]$_ })
    if ($LASTEXITCODE -ne 0) {
        throw "读取 BlackMoon x64 主归档符号失败:`n$($archiveSymbolOutput -join "`n")"
    }
    foreach ($match in [regex]::Matches(($archiveSymbolOutput -join "`n"), '(?m)^\s*[0-9A-F]+\s+krnln_[A-Za-z0-9_]+_(\d+)_krnln\s*$')) {
        [void]$adapterCommandIndexes.Add([int]$match.Groups[1].Value)
    }
    if ($adapterCommandIndexes.Count -eq 0) {
        throw '未能从 BlackMoon ABI 包装中提取核心命令索引。'
    }
    $modernCommandSource = Get-ChildItem -Path $ModernCoreRoot -Recurse -Filter 'krnln_cmd_impl.cpp' -File |
        Select-Object -First 1 -ExpandProperty FullName
    if ([string]::IsNullOrEmpty($modernCommandSource)) {
        throw "未在兼容核心源码中找到 krnln_cmd_impl.cpp: $ModernCoreRoot"
    }
    $modernCoreDirectory = Split-Path -Parent (Split-Path -Parent $modernCommandSource)
    $filteredCommandSource = Join-Path $workRoot 'fallback_cmd_impl.cpp'
    $filteredCommandText = Remove-AdapterCommandsFromModernSource `
        -SourceText ([IO.File]::ReadAllText($modernCommandSource, [Text.Encoding]::UTF8)) `
        -CommandIndexes $adapterCommandIndexes
    # The generated companion core allocates arrays with the standard
    # two-int header (dimension count + first dimension), but several array
    # writers advanced by three ints before writing pointer elements. Normalize
    # the array payload offset once for the whole compatibility source.
    $filteredCommandText = [regex]::Replace(
        $filteredCommandText,
        'sizeof\s*\(\s*INT\s*\)\s*\*\s*3',
        'sizeof(INT) * 2')
    [IO.File]::WriteAllText($filteredCommandSource, $filteredCommandText, [Text.UTF8Encoding]::new($true))
    $filteredCommandObject = Join-Path $workRoot 'objects\fallback_cmd_impl.obj'
    $filteredCommandBuild = Invoke-AdapterCompile -Compiler $compiler -Source $filteredCommandSource `
        -Object $filteredCommandObject -IncludeDirectories @($workRoot, $modernCoreDirectory) `
        -ExtraArguments @('/D__E_STATIC_LIB', '/utf-8')
    if (-not $filteredCommandBuild.Success) {
        throw "编译过滤后的兼容核心命令对象失败:`n$($filteredCommandBuild.Output -join "`n")"
    }
    $fallbackMembers = @(& $libraryManager '/nologo' '/list' $FallbackLibrary 2>&1 | ForEach-Object { [string]$_ })
    if ($LASTEXITCODE -ne 0) {
        throw "读取兼容核心归档成员失败:`n$($fallbackMembers -join "`n")"
    }
    $commandMember = $fallbackMembers | Where-Object { $_ -match '(?i)krnln_cmd_impl\.obj\s*$' } |
        Select-Object -First 1
    if ([string]::IsNullOrWhiteSpace($commandMember)) {
        throw "兼容核心归档中未找到 krnln_cmd_impl.obj: $FallbackLibrary"
    }
    $fallbackBase = Join-Path $workRoot 'krnln_fallback_without_commands.lib'
    $removeOutput = & $libraryManager '/nologo' ('/REMOVE:' + $commandMember.Trim()) ('/OUT:' + $fallbackBase) $FallbackLibrary 2>&1
    if ($LASTEXITCODE -ne 0 -or -not (Test-Path -LiteralPath $fallbackBase -PathType Leaf)) {
        throw "从兼容核心归档移除原命令对象失败:`n$($removeOutput -join "`n")"
    }
    $fallbackOutput = Join-Path $OutputRoot 'krnln_fallback.lib'
    $fallbackOutputLog = & $libraryManager '/nologo' ('/OUT:' + $fallbackOutput) $fallbackBase $filteredCommandObject 2>&1
    if ($LASTEXITCODE -ne 0 -or -not (Test-Path -LiteralPath $fallbackOutput -PathType Leaf)) {
        throw "生成过滤后的兼容核心归档失败:`n$($fallbackOutputLog -join "`n")"
    }
    $manifest = [ordered]@{
        formatVersion = 1
        architecture = 'x64'
        abi = 'ecompiler-fne-execute-v1'
        primaryArchive = 'krnln_static.lib'
        fallbackArchive = 'krnln_fallback.lib'
		fallbackRequired = $true
		metadataFile = '../../lib/x64/krnln.fne'
        source = 'BlackMoonKernelStaticLib'
        sourceRoot = 'krnln'
        adapterCommandCount = $adapterCommandIndexes.Count
        eligibleSourceCount = $eligibleSources.Count
        rejectedSourceCount = $rejectedSources.Count
        generatedAtUtc = [DateTime]::UtcNow.ToString('o')
    }
    $jsonEncoding = [Text.UTF8Encoding]::new($true)
    [IO.File]::WriteAllText((Join-Path $OutputRoot 'krnln_adapter.json'), ($manifest | ConvertTo-Json -Depth 4), $jsonEncoding)
    # ConvertTo-Json recursively expands PowerShell's generic List internals
    # on some hosts. Materialize plain arrays before writing the report.
    $report = [ordered]@{
        manifest = $manifest
        eligibleSources = @($eligibleSources.ToArray() | Sort-Object)
        rejectedSources = @($rejectedSources.ToArray() | Sort-Object source | ForEach-Object {
            [ordered]@{ source = $_.source; diagnostic = $_.diagnostic }
        })
    }
    [IO.File]::WriteAllText((Join-Path $OutputRoot 'krnln_adapter_report.json'), ($report | ConvertTo-Json -Depth 5), $jsonEncoding)

    Write-Output ("adapter-built: archive={0};eligible={1};rejected={2};fallback={3}" -f $archive, $eligibleSources.Count, $rejectedSources.Count, $fallbackOutput)
}
finally {
    $env:INCLUDE = $savedInclude
    $env:LIB = $savedLib
}
