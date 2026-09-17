param(
    [string]$Packager = "$PSScriptRoot/../bin/Win32/Release/e-packager.exe",
    [string]$EDirectory = 'C:\Users\aiqin\OneDrive\e5.6',
    [string]$OutputRoot = "$PSScriptRoot/../temp/edit-borders-$([guid]::NewGuid().ToString('N'))"
)
$ErrorActionPreference = 'Stop'
$OutputRoot = [IO.Path]::GetFullPath($OutputRoot)
$workspace = Join-Path $OutputRoot 'workspace'
& $Packager unpack "$PSScriptRoot/../eproj/e-window-exe-new-proj.e" $workspace --main-only
if ($LASTEXITCODE -ne 0) { throw 'fixture unpack failed' }
function Write-Source([string]$Name, [string]$Content) {
    [IO.File]::WriteAllText((Join-Path $workspace "src/$Name"),
        ($Content -replace '\r?\n', "`r`n"), [Text.UTF8Encoding]::new($true))
}
Write-Source '窗口程序集_启动窗口.txt' @'
.版本 2
.程序集 窗口程序集_启动窗口
.子程序 _启动子程序, 整数型
载入 (_启动窗口, , 假)
返回 (0)
'@
Write-Source '_启动窗口.xml' @'
<?xml version="1.0" encoding="UTF-8"?>
<窗口 名称="_启动窗口" 左边="50" 顶边="50" 宽度="220" 高度="100" 标题="fixture" 边框="2">
  <分组框 名称="背景容器" 左边="5" 顶边="5" 宽度="200" 高度="80" 标题="group" 背景颜色="16777215">
  <编辑框 名称="测试编辑框" 左边="20" 顶边="20" 宽度="160" 高度="24" 内容="50" 边框="1" 背景颜色="16777215" />
  </分组框>
</窗口>
'@
$output = Join-Path $OutputRoot 'edit-borders.exe'
& $Packager compile $workspace $output --arch x86 --e-dir $EDirectory
if ($LASTEXITCODE -ne 0) { throw 'edit-border fixture compilation failed' }
if (-not ('EditBorderFixture' -as [type])) {
    Add-Type @'
using System;
using System.Text;
using System.Runtime.InteropServices;
public static class EditBorderFixture {
    public delegate bool EnumProc(IntPtr window, IntPtr data);
    [DllImport("user32.dll")] public static extern bool EnumWindows(EnumProc callback, IntPtr data);
    [DllImport("user32.dll")] public static extern bool EnumChildWindows(IntPtr parent, EnumProc callback, IntPtr data);
    [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr window, out uint pid);
    [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern int GetClassName(IntPtr window, StringBuilder text, int count);
    [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern bool SetWindowText(IntPtr window, string text);
    [DllImport("user32.dll")] public static extern int GetWindowLong(IntPtr window, int index);
    [DllImport("user32.dll")] public static extern bool GetClientRect(IntPtr window, out RECT rect);
    [DllImport("user32.dll")] public static extern bool RedrawWindow(IntPtr window, IntPtr updateRect, IntPtr updateRegion, uint flags);
    [DllImport("user32.dll")] public static extern IntPtr GetWindowDC(IntPtr window);
    [DllImport("user32.dll")] public static extern int ReleaseDC(IntPtr window, IntPtr dc);
    [DllImport("gdi32.dll")] public static extern uint GetPixel(IntPtr dc, int x, int y);
    [StructLayout(LayoutKind.Sequential)] public struct RECT { public int left, top, right, bottom; }
}
'@
}
$process = Start-Process -FilePath $output -WorkingDirectory $OutputRoot -PassThru
try {
    $main = [IntPtr]::Zero
    for ($attempt = 0; $attempt -lt 100 -and $main -eq [IntPtr]::Zero; $attempt++) {
        Start-Sleep -Milliseconds 50
        [void][EditBorderFixture]::EnumWindows({ param($window, $data)
            $owner = 0
            [void][EditBorderFixture]::GetWindowThreadProcessId($window, [ref]$owner)
            $className = [Text.StringBuilder]::new(80)
            [void][EditBorderFixture]::GetClassName($window, $className, $className.Capacity)
            if ($owner -eq $process.Id -and $className.ToString() -eq 'ecompiler_window_form') { $script:main = $window }
            return $true
        }, [IntPtr]::Zero)
    }
    if ($main -eq [IntPtr]::Zero) { throw 'fixture window was not created' }
    $edit = [IntPtr]::Zero
    [void][EditBorderFixture]::EnumChildWindows($main, { param($window, $data)
        $className = [Text.StringBuilder]::new(80)
        [void][EditBorderFixture]::GetClassName($window, $className, $className.Capacity)
        if ($className.ToString() -eq 'Edit') { $script:edit = $window }
        return $true
    }, [IntPtr]::Zero)
    if ($edit -eq [IntPtr]::Zero) { throw 'fixture edit control was not created' }
    for ($update = 0; $update -lt 100; $update++) {
        [void][EditBorderFixture]::SetWindowText($edit, [string]$update)
    }
    [void][EditBorderFixture]::SetWindowText($edit, "50")
    [void][EditBorderFixture]::RedrawWindow($edit, [IntPtr]::Zero, [IntPtr]::Zero, 0x0501)
    Start-Sleep -Milliseconds 100
    $style = [uint32][EditBorderFixture]::GetWindowLong($edit, -16)
    $extendedStyle = [uint32][EditBorderFixture]::GetWindowLong($edit, -20)
    $client = [EditBorderFixture+RECT]::new()
    [void][EditBorderFixture]::GetClientRect($edit, [ref]$client)
    $dc = [EditBorderFixture]::GetWindowDC($edit)
    try {
        $innerTopPixel = [EditBorderFixture]::GetPixel($dc, 120, 2)
        $bodyPixel = [EditBorderFixture]::GetPixel($dc, 120, 14)
        $cornerPixels = @(
            [EditBorderFixture]::GetPixel($dc, 0, 0),
            [EditBorderFixture]::GetPixel($dc, 159, 0),
            [EditBorderFixture]::GetPixel($dc, 0, 23),
            [EditBorderFixture]::GetPixel($dc, 159, 23)
        )
        $textTop = 100
        $textBottom = -1
        for ($y = 3; $y -lt 21; $y++) {
            for ($x = 3; $x -lt 157; $x++) {
                $color = [EditBorderFixture]::GetPixel($dc, $x, $y)
                if (($color -band 0xff) -lt 80 -and (($color -shr 8) -band 0xff) -lt 80 -and (($color -shr 16) -band 0xff) -lt 80) {
                    $textTop = [Math]::Min($textTop, $y)
                    $textBottom = [Math]::Max($textBottom, $y)
                }
            }
        }
    }
    finally { [void][EditBorderFixture]::ReleaseDC($edit, $dc) }
    if (($style -band 0x00800000) -ne 0) { throw "unexpected WS_BORDER style: 0x$($style.ToString('X8'))" }
    if (($extendedStyle -band 0x00000200) -eq 0) { throw "missing WS_EX_CLIENTEDGE: 0x$($extendedStyle.ToString('X8'))" }
    if (($client.bottom - $client.top) -ne 12) {
        throw "single-line edit client is not vertically centered: $($client.right - $client.left)x$($client.bottom - $client.top)"
    }
    if (@($cornerPixels | Where-Object { ($_ -band 0xff) -lt 200 -or (($_ -shr 8) -band 0xff) -lt 200 -or (($_ -shr 16) -band 0xff) -lt 200 }).Count -gt 0) { throw "edit contains a black corner pixel: $($cornerPixels -join ',')" }
    if ($textTop -ne 8 -or $textBottom -ne 15) {
        throw "single-line edit text is not vertically aligned like the native runtime: y=$textTop..$textBottom"
    }
    if ($innerTopPixel -ne $bodyPixel) {
        throw "edit inner edge differs from body: top=$innerTopPixel body=$bodyPixel"
    }
    Write-Host "PASS edit border is initialized without a dark inner frame (pixel=$bodyPixel corners=$($cornerPixels -join ','))"
}
finally {
    if (-not $process.HasExited) { $process.Kill(); $process.WaitForExit() }
}
