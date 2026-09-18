param([string]$OutputRoot = "$PSScriptRoot/../temp/native-drop-target")
$ErrorActionPreference = 'Stop'
$OutputRoot = [IO.Path]::GetFullPath($OutputRoot)
New-Item -ItemType Directory -Force $OutputRoot | Out-Null
$header = Get-Content "$PSScriptRoot/../src/compiler/NativeDropTargetRuntime.h" -Raw
$runtime = [regex]::Match($header, '(?s)R"CPP\((.*?)\)CPP"').Groups[1].Value
$fixture = Get-Content "$PSScriptRoot/fixtures/NativeDropTargetTest.cpp" -Raw
[IO.File]::WriteAllText("$OutputRoot/test.cpp", $fixture.Replace('// INSERT_RUNTIME', $runtime), [Text.UTF8Encoding]::new($true))
$vswhere = "${env:ProgramFiles(x86)}/Microsoft Visual Studio/Installer/vswhere.exe"
$vs = & $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
foreach ($arch in @('x86','x64')) {
    $batch = "@call `"$vs/VC/Auxiliary/Build/vcvarsall.bat`" $arch >nul`r`n@cl /nologo /std:c++20 /EHsc /utf-8 /DUNICODE /D_UNICODE test.cpp /Fe:test-$arch.exe /Fo:test-$arch.obj user32.lib shell32.lib ole32.lib comctl32.lib`r`n"
    [IO.File]::WriteAllText("$OutputRoot/build.cmd",$batch)
    Push-Location $OutputRoot
    try {
        & cmd /c build.cmd
        if ($LASTEXITCODE -ne 0) { throw "$arch harness build failed" }
        & ".\test-$arch.exe"
        if ($LASTEXITCODE -ne 0) { throw "$arch harness failed" }
    } finally { Pop-Location }
}
