param(
    [string]$Executable = "$PSScriptRoot/../temp/wfui-runtime/wfui-semantic.exe",
    [string]$OutputDirectory = "$PSScriptRoot/../temp/wfui-runtime/verified"
)
$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.Drawing
Add-Type @'
using System;
using System.Text;
using System.Runtime.InteropServices;
public static class WfuiRuntimeTest {
    public delegate bool Callback(IntPtr hwnd, IntPtr data);
    [StructLayout(LayoutKind.Sequential)] public struct Rect { public int Left, Top, Right, Bottom; }
    [DllImport("user32.dll")] static extern bool EnumWindows(Callback callback, IntPtr data);
    [DllImport("user32.dll")] static extern uint GetWindowThreadProcessId(IntPtr hwnd, out uint process);
    [DllImport("user32.dll", CharSet=CharSet.Unicode)] static extern int GetClassName(IntPtr hwnd, StringBuilder name, int size);
    [DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr hwnd);
    [DllImport("user32.dll")] public static extern bool IsWindowEnabled(IntPtr hwnd);
    [DllImport("user32.dll")] public static extern bool IsIconic(IntPtr hwnd);
    [DllImport("user32.dll")] public static extern bool IsZoomed(IntPtr hwnd);
    [DllImport("user32.dll")] public static extern bool ShowWindowAsync(IntPtr hwnd, int command);
    [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr hwnd, out Rect rect);
    [DllImport("user32.dll", SetLastError=true)] public static extern bool SetWindowPos(IntPtr hwnd, IntPtr after, int x, int y, int width, int height, uint flags);
    [DllImport("user32.dll", SetLastError=true)] public static extern bool PostMessageW(IntPtr hwnd, uint message, IntPtr w, IntPtr l);
    [DllImport("user32.dll", SetLastError=true)] static extern IntPtr SendMessageTimeoutW(IntPtr hwnd, uint message, IntPtr w, IntPtr l, uint flags, uint timeout, out IntPtr result);
    [DllImport("user32.dll")] public static extern IntPtr GetDC(IntPtr hwnd);
    [DllImport("user32.dll")] public static extern int ReleaseDC(IntPtr hwnd, IntPtr dc);
    [DllImport("gdi32.dll")] public static extern bool BitBlt(IntPtr target, int x, int y, int width, int height, IntPtr source, int sx, int sy, uint operation);
    public static IntPtr Find(int pid, string classname) {
        IntPtr found=IntPtr.Zero;
        EnumWindows((h,l)=>{uint p;GetWindowThreadProcessId(h,out p);if(p==pid && IsWindowVisible(h)){var name=new StringBuilder(128);GetClassName(h,name,128);if(name.ToString()==classname){found=h;return false;}}return true;},IntPtr.Zero);
        return found;
    }
    public static void Send(IntPtr hwnd, uint message, int w, int l) {
        IntPtr result;
        if(SendMessageTimeoutW(hwnd,message,(IntPtr)w,(IntPtr)l,2,5000,out result)==IntPtr.Zero)
            throw new Exception("Window message failed: " + message + ", error=" + Marshal.GetLastWin32Error());
    }
}
'@
$Executable = [IO.Path]::GetFullPath($Executable)
$OutputDirectory = [IO.Path]::GetFullPath($OutputDirectory)
New-Item -ItemType Directory -Path $OutputDirectory -Force | Out-Null
function Wait-Window([string]$Class, [bool]$Present = $true) {
    $timer = [Diagnostics.Stopwatch]::StartNew()
    do {
        if ($process.HasExited) { throw "Process exited prematurely: $($process.ExitCode)" }
        $window = [WfuiRuntimeTest]::Find($process.Id, $Class)
        if (($window -ne [IntPtr]::Zero) -eq $Present) { return $window }
        Start-Sleep -Milliseconds 50
    } while ($timer.ElapsedMilliseconds -lt 10000)
    throw "Window state timed out: $Class, present=$Present"
}
function Click-Window([IntPtr]$Window, [int]$X, [int]$Y) {
    $point = ($Y -shl 16) -bor $X
    [WfuiRuntimeTest]::Send($Window, 512, 0, $point)
    [WfuiRuntimeTest]::Send($Window, 513, 1, $point)
    [WfuiRuntimeTest]::Send($Window, 514, 0, $point)
}
function Capture-Window([IntPtr]$Window, [string]$Name) {
    if (-not [WfuiRuntimeTest]::SetWindowPos($Window, [IntPtr](-1), 0, 0, 0, 0, 0x13)) { throw 'Cannot raise test window' }
    Start-Sleep -Milliseconds 200
    $rect = New-Object WfuiRuntimeTest+Rect
    if (-not [WfuiRuntimeTest]::GetWindowRect($Window, [ref]$rect)) { throw 'Cannot read test window bounds' }
    $bitmap = New-Object Drawing.Bitmap ($rect.Right-$rect.Left), ($rect.Bottom-$rect.Top)
    $graphics = [Drawing.Graphics]::FromImage($bitmap)
    $target = $graphics.GetHdc()
    $screen = [WfuiRuntimeTest]::GetDC([IntPtr]::Zero)
    try {
        # CAPTUREBLT includes the per-pixel layered windows used by WFUI.
        if (-not [WfuiRuntimeTest]::BitBlt($target, 0, 0, $bitmap.Width, $bitmap.Height, $screen, $rect.Left, $rect.Top, 0x40CC0020)) { throw 'Window capture failed' }
    } finally {
        [WfuiRuntimeTest]::ReleaseDC([IntPtr]::Zero, $screen) | Out-Null
        $graphics.ReleaseHdc($target)
        $graphics.Dispose()
        [WfuiRuntimeTest]::SetWindowPos($Window, [IntPtr](-2), 0, 0, 0, 0, 0x13) | Out-Null
    }
    $bitmap.Save((Join-Path $OutputDirectory $Name), [Drawing.Imaging.ImageFormat]::Png)
    return $bitmap
}
$process = Start-Process -FilePath $Executable -WorkingDirectory (Split-Path $Executable) -PassThru
try {
    $main = Wait-Window 'ecompiler_window_form'
    # Keep captures inside the primary desktop, independent of the current cursor.
    [WfuiRuntimeTest]::SetWindowPos($main, [IntPtr](-1), 80, 80, 0, 0, 0x11) | Out-Null
    $initial = Capture-Window $main 'wfui-main.png'
    try {
        if ($initial.Width -ne 500 -or $initial.Height -ne 350) { throw 'Unexpected main window dimensions' }
        if ($initial.GetPixel(20, 40).R -gt 40 -or $initial.GetPixel(240, 270).R -lt 230) { throw 'Self-drawn button or edit background is missing' }
        Click-Window $main 40 270
        [WfuiRuntimeTest]::Send($main, 258, 65, 0)
        $edited = Capture-Window $main 'wfui-input.png'
        try {
            $changed = 0
            for ($y=255; $y -lt 285; $y++) { for ($x=12; $x -lt 240; $x++) {
                if ($initial.GetPixel($x,$y).ToArgb() -ne $edited.GetPixel($x,$y).ToArgb()) { $changed++ }
            } }
            if ($changed -lt 10) { throw 'Text input did not change the rendered edit field' }
        } finally { $edited.Dispose() }
    } finally { $initial.Dispose() }
    Click-Window $main 389 18
    $menu = Wait-Window 'WF_Menu'
    $picture = Capture-Window $menu 'wfui-menu.png'
    $picture.Dispose()
    # About starts a modal loop before the menu's mouse-up handler returns.
    [WfuiRuntimeTest]::Send($menu, 512, 0, (76 -shl 16) -bor 55)
    [WfuiRuntimeTest]::Send($menu, 513, 1, (76 -shl 16) -bor 55)
    [WfuiRuntimeTest]::PostMessageW($menu, 514, [IntPtr]::Zero, [IntPtr]((76 -shl 16) -bor 55)) | Out-Null
    $about = Wait-Window 'WF_MsgBox'
    Start-Sleep -Milliseconds 600
    [WfuiRuntimeTest]::SetWindowPos($about, [IntPtr](-1), 300, 300, 0, 0, 0x11) | Out-Null
    $picture = Capture-Window $about 'wfui-about.png'
    $picture.Dispose()
    Start-Sleep -Milliseconds 400
    [WfuiRuntimeTest]::PostMessageW($about, 16, [IntPtr]::Zero, [IntPtr]::Zero) | Out-Null
    $null = Wait-Window 'WF_MsgBox' $false
    $null = Wait-Window 'WF_Menu' $false
    if (-not [WfuiRuntimeTest]::IsWindowEnabled($main)) { throw 'About did not re-enable its owner' }
    Click-Window $main 419 18
    Start-Sleep -Milliseconds 600
    if ($process.HasExited -or -not [WfuiRuntimeTest]::IsIconic($main)) { throw 'Minimize failed' }
    [WfuiRuntimeTest]::ShowWindowAsync($main, 9) | Out-Null
    Start-Sleep -Milliseconds 600
    if ($process.HasExited -or [WfuiRuntimeTest]::IsIconic($main)) { throw 'Restore after minimize failed' }
    Click-Window $main 449 18
    Start-Sleep -Milliseconds 600
    if ($process.HasExited -or -not [WfuiRuntimeTest]::IsZoomed($main)) { throw 'Maximize failed' }
    $picture = Capture-Window $main 'wfui-maximized.png'
    $restoreX = $picture.Width - 51
    $picture.Dispose()
    Click-Window $main $restoreX 18
    Start-Sleep -Milliseconds 600
    if ($process.HasExited -or [WfuiRuntimeTest]::IsZoomed($main)) { throw 'Restore after maximize failed' }
    $picture = Capture-Window $main 'wfui-restored.png'
    try { if ($picture.Width -ne 500 -or $picture.Height -ne 350) { throw 'Restored bounds differ from initial bounds' } } finally { $picture.Dispose() }
    [WfuiRuntimeTest]::PostMessageW($main, 16, [IntPtr]::Zero, [IntPtr]::Zero) | Out-Null
    $confirm = Wait-Window 'WF_MsgBox'
    $picture = Capture-Window $confirm 'wfui-confirm.png'
    $picture.Dispose()
    Click-Window $confirm 270 124
    $null = Wait-Window 'WF_MsgBox' $false
    if (-not [WfuiRuntimeTest]::IsWindowEnabled($main)) { throw 'Cancel did not re-enable the owner window' }
    [WfuiRuntimeTest]::PostMessageW($main, 16, [IntPtr]::Zero, [IntPtr]::Zero) | Out-Null
    $confirm = Wait-Window 'WF_MsgBox'
    Start-Sleep -Milliseconds 400
    Click-Window $confirm 195 124
    if (-not $process.WaitForExit(10000)) { throw 'Confirmed close did not terminate the program' }
    if ($process.ExitCode -ne 0) { throw "Runtime exit code: $($process.ExitCode)" }
    Write-Host "PASS WFUI x86 rendering, movement, text input, About, minimize/restore, maximize/restore, cancel and confirmed exit; screenshots: $OutputDirectory"
} finally {
    if (-not $process.HasExited) { $process.Kill(); $process.WaitForExit() }
    $process.Dispose()
}
