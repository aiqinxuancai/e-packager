param(
    [string]$Packager = "$PSScriptRoot/../bin/Win32/Release/e-packager.exe",
    [string]$EDirectory = 'C:\Users\aiqin\OneDrive\e5.6',
    [string]$OutputRoot = "$PSScriptRoot/../temp/mouse-events-$([guid]::NewGuid().ToString('N'))"
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
.子程序 开始, 逻辑型
.参数 x, 整数型
.参数 y, 整数型
.参数 flags, 整数型
_启动窗口.标题 ＝ “pressed”
捕获 (拖拽源.取窗口句柄 ())
返回 (假)
.子程序 结束, 逻辑型
.参数 x, 整数型
.参数 y, 整数型
.参数 flags, 整数型
释放 ()
_启动窗口.标题 ＝ “released”
返回 (假)
'@
Write-Source '.DLL声明.txt' @'
.版本 2
.DLL命令 捕获, 整数型, "user32.dll", "SetCapture"
    .参数 窗口, 整数型
.DLL命令 释放, 整数型, "user32.dll", "ReleaseCapture"
'@
Write-Source '_启动窗口.xml' @'
<?xml version="1.0" encoding="UTF-8"?>
<窗口 名称="_启动窗口" 左边="50" 顶边="50" 宽度="200" 高度="120" 标题="fixture" 边框="2">
  <图片框 名称="拖拽源" 左边="10" 顶边="10" 宽度="40" 高度="40" 可视="真">
    <图片框.事件 索引="-1" 名称="_Lib0Type5Event-1" 处理器="窗口程序集_启动窗口::开始" />
    <图片框.事件 索引="-2" 名称="_Lib0Type5Event-2" 处理器="窗口程序集_启动窗口::结束" />
  </图片框>
</窗口>
'@
$output = Join-Path $OutputRoot 'mouse-events.exe'
& $Packager compile $workspace $output --arch x86 --e-dir $EDirectory
if ($LASTEXITCODE -ne 0) { throw 'mouse fixture compilation failed' }
if (-not ('MouseEventFixture' -as [type])) {
    Add-Type @'
using System;
using System.Text;
using System.Runtime.InteropServices;
public static class MouseEventFixture {
    public delegate bool EnumProc(IntPtr window, IntPtr data);
    [DllImport("user32.dll")] public static extern bool EnumWindows(EnumProc callback, IntPtr data);
    [DllImport("user32.dll")] public static extern bool EnumChildWindows(IntPtr parent, EnumProc callback, IntPtr data);
    [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr window, out uint pid);
    [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern int GetWindowText(IntPtr window, StringBuilder text, int count);
    [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern int GetClassName(IntPtr window, StringBuilder text, int count);
    [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern IntPtr SendMessageTimeout(IntPtr window, uint message, UIntPtr wParam, IntPtr lParam, uint flags, uint timeout, out UIntPtr result);
    [StructLayout(LayoutKind.Sequential)] public struct GUI {
        public uint size, flags;
        public IntPtr active, focus, capture, menuOwner, moveSize, caret;
        public int left, top, right, bottom;
    }
    [DllImport("user32.dll")] public static extern bool GetGUIThreadInfo(uint thread, ref GUI info);
    public static IntPtr Find(uint pid) {
        IntPtr result=IntPtr.Zero;
        EnumWindows((h,d)=> { uint owner; GetWindowThreadProcessId(h,out owner);var name=new StringBuilder(80);GetClassName(h,name,80);if(owner==pid&&name.ToString()=="ecompiler_window_form")result=h;return true; },IntPtr.Zero);
        return result;
    }
    public static void Check(IntPtr main, uint message, string title, bool captured) {
        IntPtr child=IntPtr.Zero;
        EnumChildWindows(main,(h,d)=>{child=h;return false;},IntPtr.Zero);
        if(child==IntPtr.Zero)throw new Exception("missing image control");
        UIntPtr result;
        if(SendMessageTimeout(child,message,UIntPtr.Zero,new IntPtr(0x20002),2,3000,out result)==IntPtr.Zero)throw new Exception("mouse event timed out");
        var text=new StringBuilder(80);GetWindowText(main,text,80);
        uint pid;uint thread=GetWindowThreadProcessId(child,out pid);
        var info=new GUI();info.size=(uint)Marshal.SizeOf(info);
        if(!GetGUIThreadInfo(thread,ref info)||text.ToString()!=title||info.capture!=(captured?child:IntPtr.Zero))throw new Exception("wrong event or mouse capture state");
    }
    public static void Close(IntPtr main) { UIntPtr result;SendMessageTimeout(main,0x10,UIntPtr.Zero,IntPtr.Zero,2,3000,out result); }
}
'@
}
$process = Start-Process -FilePath $output -WorkingDirectory $OutputRoot -WindowStyle Hidden -PassThru
try {
    $main = [IntPtr]::Zero
    for ($attempt = 0; $attempt -lt 50 -and $main -eq [IntPtr]::Zero; $attempt++) {
        Start-Sleep -Milliseconds 100
        $main = [MouseEventFixture]::Find($process.Id)
        if ($process.HasExited) { throw "window startup failed: $($process.ExitCode)" }
    }
    if ($main -eq [IntPtr]::Zero) { throw 'window startup timed out' }
    [MouseEventFixture]::Check($main, 0x201, 'pressed', $true)
    [MouseEventFixture]::Check($main, 0x202, 'released', $false)
    [MouseEventFixture]::Close($main)
    if (-not $process.WaitForExit(3000) -or $process.ExitCode -ne 0) { throw 'window shutdown failed' }
} finally {
    if (-not $process.HasExited) { Stop-Process -Id $process.Id }
    $process.Dispose()
}
Write-Host 'PASS window startup, opaque negative mouse events, capture, release and clean shutdown'
