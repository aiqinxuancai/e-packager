[CmdletBinding()]
param([string]$AdapterRoot = 'D:\git\BlackMoonKernelStaticLib\adapter')

$ErrorActionPreference = 'Stop'
$PSNativeCommandUseErrorActionPreference = $false
$repo = Split-Path -Parent $PSScriptRoot
$root = Join-Path $repo ('temp\executable-resources-' + [guid]::NewGuid().ToString('N'))
[IO.Directory]::CreateDirectory($root) | Out-Null
$x86 = Join-Path $repo 'bin\Win32\Release\e-packager.exe'
$x64 = Join-Path $repo 'bin\x64\Release\e-packager.exe'

function Invoke-Tool([string]$Tool, [string[]]$Arguments, [string]$ExpectedError = '') {
    $output = & $Tool @Arguments 2>&1 | Out-String
    if ($ExpectedError) {
        if ($LASTEXITCODE -eq 0 -or -not $output.Contains($ExpectedError)) { throw "Expected $ExpectedError`n$output" }
    } elseif ($LASTEXITCODE -ne 0) { throw $output }
}
function Write-Text([string]$Path, [string]$Text) {
    [IO.File]::WriteAllText($Path, ($Text -replace '\r?\n', "`r`n"), [Text.UTF8Encoding]::new($true))
}
function Write-Json([string]$Path, $Value) { Write-Text $Path ($Value | ConvertTo-Json -Depth 40) }
function Assert($Condition, [string]$Message) { if (-not $Condition) { throw $Message } }

Add-Type -AssemblyName System.Drawing
if (-not ('ExecutableResourceProbe' -as [type])) {
    Add-Type @'
using System;
using System.Runtime.InteropServices;
public static class ExecutableResourceProbe {
    [DllImport("kernel32.dll", CharSet=CharSet.Unicode, SetLastError=true)] static extern IntPtr LoadLibraryEx(string p, IntPtr f, uint flags);
    [DllImport("kernel32.dll")] static extern bool FreeLibrary(IntPtr m);
    [DllImport("kernel32.dll")] static extern IntPtr FindResource(IntPtr m, IntPtr n, IntPtr t);
    [DllImport("kernel32.dll")] static extern uint SizeofResource(IntPtr m, IntPtr r);
    [DllImport("kernel32.dll")] static extern IntPtr LoadResource(IntPtr m, IntPtr r);
    [DllImport("kernel32.dll")] static extern IntPtr LockResource(IntPtr r);
    [DllImport("version.dll", CharSet=CharSet.Unicode)] static extern uint GetFileVersionInfoSize(string p, out uint h);
    [DllImport("version.dll", CharSet=CharSet.Unicode)] static extern bool GetFileVersionInfo(string p, uint h, uint n, byte[] b);
    [DllImport("version.dll", CharSet=CharSet.Unicode)] static extern bool VerQueryValue(byte[] b, string k, out IntPtr p, out uint n);
    [DllImport("user32.dll", EntryPoint="GetClassLongPtrW")] static extern IntPtr GetClassLongPtr(IntPtr h, int i);
    [DllImport("user32.dll", EntryPoint="SendMessageW")] static extern IntPtr SendMessage(IntPtr h, uint m, IntPtr w, IntPtr l);
    [DllImport("user32.dll")] static extern bool GetIconInfo(IntPtr h, out IconInfo info);
    [DllImport("gdi32.dll")] static extern bool DeleteObject(IntPtr h);
    [StructLayout(LayoutKind.Sequential)] struct IconInfo { public int icon; public uint x,y; public IntPtr mask,color; }
    public static byte[] Resource(string path, int type, int id) {
        var m=LoadLibraryEx(path,IntPtr.Zero,0x22);
        if(m==IntPtr.Zero)throw new Exception("LoadLibraryEx failed");
        try {
            var r=FindResource(m,(IntPtr)id,(IntPtr)type);
            if(r==IntPtr.Zero)return new byte[0];
            var b=new byte[SizeofResource(m,r)];
            Marshal.Copy(LockResource(LoadResource(m,r)),b,0,b.Length);return b;
        } finally {FreeLibrary(m);}
    }
    public static string VersionString(string path,string field) {
        uint h;var b=new byte[GetFileVersionInfoSize(path,out h)];
        if(!GetFileVersionInfo(path,0,(uint)b.Length,b))throw new Exception("Missing version");
        IntPtr p;uint n;
        if(!VerQueryValue(b,"\\StringFileInfo\\080404b0\\"+field,out p,out n))return null;
        return Marshal.PtrToStringUni(p);
    }
    public static uint FileType(string path) {
        uint h;var b=new byte[GetFileVersionInfoSize(path,out h)];
        GetFileVersionInfo(path,0,(uint)b.Length,b);IntPtr p;uint n;
        if(!VerQueryValue(b,"\\",out p,out n))throw new Exception("Missing fixed version");
        return (uint)Marshal.ReadInt32(p,36);
    }
    public static bool WindowIcons(IntPtr h) {
        foreach(int slot in new[]{-14,-34}) {
            var icon=GetClassLongPtr(h,slot);IconInfo info;
            if(icon==IntPtr.Zero || !GetIconInfo(icon,out info))return false;
            if(info.mask!=IntPtr.Zero)DeleteObject(info.mask);
            if(info.color!=IntPtr.Zero)DeleteObject(info.color);
        }
        // An explicit per-window icon must still take precedence over the class default.
        var big=GetClassLongPtr(h,-14);var small=GetClassLongPtr(h,-34);
        SendMessage(h,0x80,(IntPtr)1,small);
        bool explicitIcon=SendMessage(h,0x7f,(IntPtr)1,IntPtr.Zero)==small;
        SendMessage(h,0x80,(IntPtr)1,big);
        return explicitIcon;
    }
}
'@
}

function New-TestIcon([string]$Path, [int[]]$Sizes) {
    $images = @()
    foreach ($size in $Sizes) {
        $bitmap = [Drawing.Bitmap]::new($size, $size)
        $graphics = [Drawing.Graphics]::FromImage($bitmap)
        $stream = [IO.MemoryStream]::new()
        try {
            $graphics.Clear([Drawing.Color]::FromArgb(255, 40, 160, 90))
            $bitmap.Save($stream, [Drawing.Imaging.ImageFormat]::Png)
            $images += ,$stream.ToArray()
        } finally { $stream.Dispose(); $graphics.Dispose(); $bitmap.Dispose() }
    }
    $file = [IO.File]::Create($Path)
    $writer = [IO.BinaryWriter]::new($file)
    try {
        $writer.Write([uint16]0); $writer.Write([uint16]1); $writer.Write([uint16]$Sizes.Count)
        $offset = 6 + 16 * $Sizes.Count
        for ($i = 0; $i -lt $Sizes.Count; $i++) {
            $dimension = $Sizes[$i] % 256
            $writer.Write([byte]$dimension); $writer.Write([byte]$dimension)
            $writer.Write([uint16]0); $writer.Write([uint16]1); $writer.Write([uint16]32)
            $writer.Write([uint32]$images[$i].Length); $writer.Write([uint32]$offset)
            $offset += $images[$i].Length
        }
        foreach ($image in $images) { $writer.Write([byte[]]$image) }
    } finally { $writer.Dispose() }
}
function Assert-Icon([string]$Path, [int]$Count) {
    $group = [ExecutableResourceProbe]::Resource($Path, 14, 101)
    Assert ($group.Length -ge 6) "Missing icon in $Path"
    Assert ([BitConverter]::ToUInt16($group, 4) -eq $Count) "Wrong image count in $Path"
    for ($i = 0; $i -lt $Count; $i++) {
        $id = [BitConverter]::ToUInt16($group, 6 + 14 * $i + 12)
        $bytes = [ExecutableResourceProbe]::Resource($Path, 3, $id)
        Assert ($bytes.Length -eq [BitConverter]::ToUInt32($group, 6 + 14 * $i + 8)) 'Icon payload size mismatch'
    }
}

$workspace = Join-Path $root '工程 space'
Invoke-Tool $x86 @('unpack', (Join-Path $repo 'eproj\e-console-exe-new-proj.e'), $workspace)
foreach ($tool in @($x86,$x64)) {
    $roundtrip = Join-Path $root 'unchanged.e'
    Invoke-Tool $tool @('pack',$workspace,$roundtrip)
    Assert ((Get-FileHash $roundtrip).Hash -eq (Get-FileHash (Join-Path $repo 'eproj\e-console-exe-new-proj.e')).Hash) 'Unchanged source roundtrip differs'
}
$configPath = Join-Path $workspace 'project\executable.json'
$iconPath = Join-Path $workspace '图标 space.ico'
$overrideIcon = Join-Path $root 'override.ico'
New-TestIcon $iconPath @(16,32,48,256)
New-TestIcon $overrideIcon @(32)
$config = [ordered]@{
    icon='../图标 space.ico'; fileDescription='中文说明 "quoted" \ path'; productName='产品名称';
    companyName='开发者'; author='作者'; legalCopyright='Copyright (C) 2026 作者';
    fileVersion='2.3'; productVersion='4.5.6.7'
}

foreach ($arch in @('x86','x64')) {
    $tool = if ($arch -eq 'x86') { $x86 } else { $x64 }
    $common = @('--arch', $arch, '--blackmoon-core-dir', $AdapterRoot, '--x86-decoder', $x86)
    $default = Join-Path $root "$arch-default.exe"
    Invoke-Tool $tool (@('compile', $workspace, $default) + $common)
    Invoke-Tool $default @()
    Assert ([ExecutableResourceProbe]::FileType($default) -eq 1) 'Wrong EXE file type'
    Write-Json $configPath $config
    $output = Join-Path $root "$arch-configured.exe"
    Invoke-Tool $tool (@('compile', $workspace, $output) + $common)
    Assert-Icon $output 4
    foreach ($pair in @(@('Author','author'),@('CompanyName','companyName'),@('FileDescription','fileDescription'),@('LegalCopyright','legalCopyright'))) {
        Assert ([ExecutableResourceProbe]::VersionString($output,$pair[0]) -ceq $config[$pair[1]]) "Wrong $($pair[0])"
    }
    $version = [Diagnostics.FileVersionInfo]::GetVersionInfo($output)
    Assert ($version.FileVersion -eq '2.3.0.0' -and $version.ProductVersion -eq '4.5.6.7') 'Wrong versions'
    Assert ($version.FileMajorPart -eq 2 -and $version.FileMinorPart -eq 3 -and $version.FileBuildPart -eq 0) 'Wrong fixed version'
    $replacement = Join-Path $root 'replacement.json'
    Write-Json $replacement @{ companyName='Replacement' }
    $replaced = Join-Path $root "$arch-replaced.exe"
    Invoke-Tool $tool (@('compile',$workspace,$replaced,'--exe-config',$replacement,'--icon',$overrideIcon) + $common)
    Assert-Icon $replaced 1
    Assert ([ExecutableResourceProbe]::VersionString($replaced,'Author') -eq $null) 'Config replacement merged defaults'
    $dll = Join-Path $root "$arch-version.dll"
    Invoke-Tool $tool (@('compile',$workspace,$dll,'--exe-config',$replacement) + $common)
    Assert ([ExecutableResourceProbe]::FileType($dll) -eq 2) 'Wrong DLL file type'
    Assert ([ExecutableResourceProbe]::Resource($dll,14,101).Length -eq 0) 'Unexpected DLL icon'
    $before = (Get-FileHash -LiteralPath $configPath).Hash
    Invoke-Tool $x86 @('update',$workspace)
    Assert ((Get-FileHash -LiteralPath $configPath).Hash -eq $before) 'Update changed executable config'
    # Rename only our own generated configuration to exercise absence on the next pass.
    Move-Item -LiteralPath $configPath -Destination (Join-Path $root "$arch-saved-config.json")
    Write-Output "${arch}: resource, version, override, DLL and update checks passed"
}

$common = @('--arch','x86','--blackmoon-core-dir',$AdapterRoot,'--x86-decoder',$x86)
foreach ($case in @(
    @{ value=@{fileVersion='65536'}; error='invalid_version:fileVersion' },
    @{ value=@{fileVersion='1..2'}; error='invalid_version:fileVersion' },
    @{ value=@{fileVersion='1.2.3.4.5'}; error='invalid_version:fileVersion' },
    @{ value=@{author=5}; error='field_must_be_string:author' },
    @{ value=@{unknown='x'}; error='unknown_field:unknown' },
    @{ value=@{icon='missing.ico'}; error='read_failed:' }
)) {
    Write-Json $configPath $case.value
    Invoke-Tool $x86 (@('compile',$workspace,(Join-Path $root 'invalid.exe')) + $common) $case.error
}
Write-Text $overrideIcon 'bad ico'
Write-Json $configPath @{}
Invoke-Tool $x86 (@('compile',$workspace,(Join-Path $root 'bad-icon.exe'),'--icon',$overrideIcon) + $common) 'invalid_ico_header'
Invoke-Tool $x86 @('compile',$workspace,(Join-Path $root 'legacy.exe'),'--compile-mode','legacy-blackmoon','--exe-config',$configPath) 'legacy_blackmoon_does_not_support'

$sourcePath = Join-Path $workspace 'src\程序集1.txt'
$originalSource = [IO.File]::ReadAllText($sourcePath)
try {
    Write-Text $sourcePath ".版本 2`n.程序集 程序集1`n.子程序 普通方法`n"
    Invoke-Tool $x86 (@('compile',$workspace,(Join-Path $root 'missing-startup.exe')) + $common) 'startup_method_not_found'
} finally { Write-Text $sourcePath $originalSource }

# The icon is carried by the native program header, including projects without global symbols.
$symbolMap = Get-ChildItem (Join-Path $workspace 'project') -Force -Filter '*.json' | Where-Object {
    (Get-Content -LiteralPath $_.FullName -Raw | ConvertFrom-Json).programHeader
} | Select-Object -First 1
Assert ($null -ne $symbolMap) 'Native program header was not persisted'
$symbols = Get-Content -LiteralPath $symbolMap.FullName -Raw | ConvertFrom-Json
$symbols.programHeader.icon = [Convert]::ToBase64String([IO.File]::ReadAllBytes($iconPath))
Write-Json $symbolMap.FullName $symbols
$nativeExe = Join-Path $root 'native-icon.exe'
Invoke-Tool $x86 (@('compile',$workspace,$nativeExe) + $common)
Assert-Icon $nativeExe 4
$nativeSource = Join-Path $root 'native-icon.e'
# Rebuild the synthetic native fixture semantically; unchanged source intentionally reuses its original bytes.
Write-Text $sourcePath ($originalSource + "`n.子程序 资源测试标记`n")
Invoke-Tool $x86 @('pack',$workspace,$nativeSource)
Invoke-Tool $x86 (@('compile',$nativeSource,(Join-Path $root 'from-e.exe'),'--exe-config',$configPath) + $common)
Assert-Icon (Join-Path $root 'from-e.exe') 4

foreach ($arch in @('x86','x64')) {
    $tool = if ($arch -eq 'x86') { $x86 } else { $x64 }
    $gui = Join-Path $root "$arch-window.exe"
    Invoke-Tool $tool @('compile',(Join-Path $repo 'eproj\e-window-exe-new-proj.e'),$gui,'--icon',$iconPath,'--arch',$arch,'--blackmoon-core-dir',$AdapterRoot,'--x86-decoder',$x86)
    Assert-Icon $gui 4
}
Write-Output "Executable resource verification passed. Artifacts: $root"
