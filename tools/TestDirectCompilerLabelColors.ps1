param(
    [string]$Packager = "$PSScriptRoot/../bin/Win32/Release/e-packager.exe",
    [string]$EDirectory = 'C:\Users\aiqin\OneDrive\e5.6',
    [string]$OutputRoot = "$PSScriptRoot/../temp/label-colors-$([guid]::NewGuid().ToString('N'))"
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
.子程序 更新颜色
颜色标签.背景颜色 ＝ 3646696
颜色标签.数据源 ＝ “changed”
_启动窗口.标题 ＝ 到文本 (颜色标签.背景颜色) ＋ “|” ＋ 颜色标签.数据源
'@
Write-Source '_启动窗口.xml' @'
<?xml version="1.0" encoding="UTF-8"?>
<窗口 名称="_启动窗口" 左边="50" 顶边="50" 宽度="220" 高度="140" 标题="fixture" 边框="2">
  <标签 名称="颜色标签" 左边="10" 顶边="10" 宽度="80" 高度="40" 标题="" 背景颜色="16777215" />
  <按钮 名称="更新按钮" 左边="10" 顶边="65" 宽度="80" 高度="30" 标题="更新">
    <按钮.事件 索引="0" 名称="被单击" 处理器="窗口程序集_启动窗口::更新颜色" />
  </按钮>
</窗口>
'@
$output = Join-Path $OutputRoot 'label-colors.exe'
& $Packager compile $workspace $output --arch x86 --e-dir $EDirectory
if ($LASTEXITCODE -ne 0) { throw 'label fixture compilation failed' }
$eventLog = Join-Path $OutputRoot 'events.log'
if (-not ('LabelColorFixture' -as [type])) {
    Add-Type @'
using System;
using System.Text;
using System.Runtime.InteropServices;
public static class LabelColorFixture {
    public delegate bool EnumProc(IntPtr window, IntPtr data);
    [DllImport("user32.dll")] public static extern bool EnumWindows(EnumProc callback, IntPtr data);
    [DllImport("user32.dll")] public static extern bool EnumChildWindows(IntPtr parent, EnumProc callback, IntPtr data);
    [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr window, out uint pid);
    [DllImport("user32.dll")] public static extern int GetDlgCtrlID(IntPtr window);
    [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern int GetClassName(IntPtr window, StringBuilder text, int count);
    [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern int GetWindowText(IntPtr window, StringBuilder text, int count);
    [DllImport("user32.dll")] public static extern IntPtr SendMessage(IntPtr window, uint message, UIntPtr wParam, IntPtr lParam);
    [DllImport("user32.dll")] public static extern bool RedrawWindow(IntPtr window, IntPtr updateRect, IntPtr updateRegion, uint flags);
    [DllImport("user32.dll")] public static extern IntPtr GetDC(IntPtr window);
    [DllImport("user32.dll")] public static extern int ReleaseDC(IntPtr window, IntPtr dc);
    [DllImport("gdi32.dll")] public static extern uint GetPixel(IntPtr dc, int x, int y);
}
'@
}
$previousEventLog = $env:E_PACKAGER_EVENT_LOG
$env:E_PACKAGER_EVENT_LOG = $eventLog
$process = Start-Process -FilePath $output -WorkingDirectory $OutputRoot -PassThru
$env:E_PACKAGER_EVENT_LOG = $previousEventLog
try {
    $main = [IntPtr]::Zero
    for ($attempt = 0; $attempt -lt 100 -and $main -eq [IntPtr]::Zero; $attempt++) {
        Start-Sleep -Milliseconds 50
        [void][LabelColorFixture]::EnumWindows({ param($window, $data)
            $owner = 0
            [void][LabelColorFixture]::GetWindowThreadProcessId($window, [ref]$owner)
            $className = [Text.StringBuilder]::new(80)
            [void][LabelColorFixture]::GetClassName($window, $className, $className.Capacity)
            if ($owner -eq $process.Id -and $className.ToString() -eq 'ecompiler_window_form') { $script:main = $window }
            return $true
        }, [IntPtr]::Zero)
    }
    if ($main -eq [IntPtr]::Zero) { throw 'fixture window was not created' }
    Start-Sleep -Milliseconds 300
    $controls = @{}
    [void][LabelColorFixture]::EnumChildWindows($main, { param($window, $data)
        $script:controls[[LabelColorFixture]::GetDlgCtrlID($window)] = $window
        return $true
    }, [IntPtr]::Zero)
    if (-not $controls.ContainsKey(2) -or -not $controls.ContainsKey(3)) { throw 'fixture controls were not created' }
    [void][LabelColorFixture]::SendMessage($main, 0x0111, [UIntPtr]3, $controls[3])
    Start-Sleep -Milliseconds 100
    [void][LabelColorFixture]::RedrawWindow($controls[2], [IntPtr]::Zero, [IntPtr]::Zero, 0x0501)
    Start-Sleep -Milliseconds 50
    $title = [Text.StringBuilder]::new(80)
    [void][LabelColorFixture]::GetWindowText($main, $title, $title.Capacity)
    $dc = [LabelColorFixture]::GetDC($controls[2])
    try { $pixel = [LabelColorFixture]::GetPixel($dc, 40, 20) }
    finally { [void][LabelColorFixture]::ReleaseDC($controls[2], $dc) }
    if ($title.ToString() -ne '3646696|changed') { throw "background getter mismatch: $title" }
    if ($pixel -ne 3646696) { throw "label pixel mismatch: $pixel" }
    Write-Host 'PASS label background property and repaint'
}
finally {
    if (-not $process.HasExited) { $process.Kill(); $process.WaitForExit() }
}
