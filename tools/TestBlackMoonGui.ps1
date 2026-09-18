param(
    [Parameter(Mandatory=$true)][string]$Executable,
    [ValidateSet('simple','workbench')][string]$Mode='simple',
    [int]$MaxStartupMilliseconds=0,
    [int]$MaxResizeMilliseconds=0,
    [string]$OutputRoot = "$PSScriptRoot/../temp/blackmoon-gui-$([guid]::NewGuid().ToString('N'))"
)
$ErrorActionPreference='Stop'
$Executable=(Resolve-Path $Executable).Path
$OutputRoot=[IO.Path]::GetFullPath($OutputRoot)
New-Item -ItemType Directory $OutputRoot -Force | Out-Null
Add-Type -AssemblyName System.Drawing
Add-Type @'
using System;
using System.Text;
using System.Collections.Generic;
using System.Runtime.InteropServices;
public static class BlackMoonGuiTest {
 public delegate bool Callback(IntPtr h,IntPtr l);
 [StructLayout(LayoutKind.Sequential)] public struct Rect {public int Left,Top,Right,Bottom;}
 [StructLayout(LayoutKind.Sequential)] public struct Point {public int X,Y;}
 public class Window {public long Handle;public string Title,Class;public int Left,Top,Width,Height;}
 [DllImport("user32.dll")] static extern bool EnumWindows(Callback f,IntPtr l);
 [DllImport("user32.dll")] static extern bool EnumChildWindows(IntPtr h,Callback f,IntPtr l);
 [DllImport("user32.dll")] static extern uint GetWindowThreadProcessId(IntPtr h,out uint p);
 [DllImport("user32.dll",CharSet=CharSet.Unicode)] static extern int GetWindowText(IntPtr h,StringBuilder s,int n);
 [DllImport("user32.dll",CharSet=CharSet.Unicode)] static extern int GetClassName(IntPtr h,StringBuilder s,int n);
 [DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr h);
 [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h,out Rect r);
 [DllImport("user32.dll")] public static extern bool GetClientRect(IntPtr h,out Rect r);
 [DllImport("user32.dll")] public static extern bool PrintWindow(IntPtr h,IntPtr dc,uint flags);
 [DllImport("user32.dll")] static extern bool ScreenToClient(IntPtr h,ref Point p);
 [DllImport("user32.dll")] static extern IntPtr ChildWindowFromPointEx(IntPtr h,Point p,uint flags);
 public static void CheckHit(long parent,Window child) {var p=new Point{X=child.Left+child.Width/2,Y=child.Top+child.Height/2};ScreenToClient((IntPtr)parent,ref p);var hit=ChildWindowFromPointEx((IntPtr)parent,p,1);if(hit.ToInt64()!=child.Handle)throw new Exception("Control is covered: "+child.Title);}
 [DllImport("user32.dll")] public static extern bool SetWindowPos(IntPtr h,IntPtr after,int x,int y,int w,int height,uint flags);
 [DllImport("user32.dll")] public static extern bool PostMessageW(IntPtr h,uint m,IntPtr w,IntPtr l);
 [DllImport("user32.dll")] static extern IntPtr SendMessageTimeoutW(IntPtr h,uint m,IntPtr w,IntPtr l,uint flags,uint timeout,out IntPtr result);
 public static void Send(long h,uint m,uint timeout=10000) {IntPtr result;if(SendMessageTimeoutW((IntPtr)h,m,IntPtr.Zero,IntPtr.Zero,2,timeout,out result)==IntPtr.Zero)throw new Exception("Window message timed out: "+m);}
 public static long GetCheck(long h) {IntPtr result;if(SendMessageTimeoutW((IntPtr)h,0xF0,IntPtr.Zero,IntPtr.Zero,2,10000,out result)==IntPtr.Zero)throw new Exception("Checkbox timed out");return result.ToInt64();}
 public static string Text(long h,string text=null) {
  IntPtr result;IntPtr buffer=text==null?Marshal.AllocHGlobal(2048):Marshal.StringToHGlobalUni(text);
  try {if(SendMessageTimeoutW((IntPtr)h,text==null?13u:12u,(IntPtr)1024,buffer,2,10000,out result)==IntPtr.Zero)throw new Exception("Edit timed out");return text??Marshal.PtrToStringUni(buffer);}finally{Marshal.FreeHGlobal(buffer);}
 }
 public static Window Read(IntPtr h) {var s=new StringBuilder(512);var c=new StringBuilder(256);GetWindowText(h,s,512);GetClassName(h,c,256);Rect r;GetWindowRect(h,out r);return new Window{Handle=h.ToInt64(),Title=s.ToString(),Class=c.ToString(),Left=r.Left,Top=r.Top,Width=r.Right-r.Left,Height=r.Bottom-r.Top};}
 public static Window[] Windows(int pid) {var a=new List<Window>();EnumWindows((h,l)=>{uint p;GetWindowThreadProcessId(h,out p);if(p==pid&&IsWindowVisible(h)){a.Add(Read(h));EnumChildWindows(h,(ch,cl)=>{if(IsWindowVisible(ch))a.Add(Read(ch));return true;},IntPtr.Zero);}return true;},IntPtr.Zero);return a.ToArray();}
}
'@
function Assert-Layout($Windows){
    if($Mode -eq 'workbench'){
        $equal=@($Windows | Where-Object Title -match '^等宽 [123]$' | Sort-Object Left)
        $ratio=@($Windows | Where-Object Title -match '^横向比例 [37]$' | Sort-Object Left)
        if($equal.Count -ne 3 -or $ratio.Count -ne 2){throw 'Workbench controls are missing'}
        if($equal[0].Width -lt 200 -or [math]::Abs($equal[0].Width-$equal[2].Width) -gt 2){throw 'Equal-width constraints failed'}
        for($i=1;$i -lt 3;$i++){if($equal[$i].Left -lt $equal[$i-1].Left+$equal[$i-1].Width+4){throw 'Workbench buttons overlap'}}
        if($ratio[1].Width -lt 2*$ratio[0].Width -or $ratio[1].Left -lt $ratio[0].Left+$ratio[0].Width+4){throw 'Proportional constraints failed'}
        $edits=@($Windows | Where-Object Class -eq 'Edit')
        if($edits.Count -lt 5 -or @($edits | Where-Object {$_.Width -gt 150 -and $_.Height -gt 60}).Count -lt 4){throw 'Editor panels have incorrect bounds'}
        $parent=$Windows | Where-Object Class -eq 'zyWindow' | Select-Object -First 1
        foreach($edit in $edits){[BlackMoonGuiTest]::CheckHit($parent.Handle,$edit)}
        return $equal[0].Width
    }
    $one=$Windows | Where-Object Title -eq '按钮一' | Select-Object -First 1
    $two=$Windows | Where-Object Title -eq '按钮二' | Select-Object -First 1
    $prompt=$Windows | Where-Object Title -eq '点击下面的按钮，看看反馈标签的变化' | Select-Object -First 1
    $feedback=$Windows | Where-Object { $_.Title -eq '还没有点击任何按钮' -or $_.Title -like '你点击了*' } | Select-Object -First 1
    if(-not $one -or -not $two -or -not $prompt -or -not $feedback){throw 'Expected controls are missing'}
    if($one.Width -lt 120 -or $two.Width -lt 120 -or [math]::Abs($one.Width-$two.Width) -gt 2){throw 'Button widths are incorrect'}
    if($two.Left -lt $one.Left+$one.Width+10 -or $one.Top -ne $two.Top){throw 'Buttons overlap or are not aligned'}
    if($one.Top -lt $prompt.Top+$prompt.Height+10 -or $feedback.Top -lt $one.Top+$one.Height+10){throw 'Vertical constraints are incorrect'}
    return $one.Width
}
function Save-Screenshot($Window,[string]$Name){
    $r=New-Object BlackMoonGuiTest+Rect
    [BlackMoonGuiTest]::GetWindowRect([IntPtr]$Window.Handle,[ref]$r)|Out-Null
    $bitmap=New-Object Drawing.Bitmap ($r.Right-$r.Left),($r.Bottom-$r.Top)
    $graphics=[Drawing.Graphics]::FromImage($bitmap)
    try{
        $dc=$graphics.GetHdc()
        try{if(-not [BlackMoonGuiTest]::PrintWindow([IntPtr]$Window.Handle,$dc,2)){throw 'Window capture failed'}}finally{$graphics.ReleaseHdc($dc)}
        $bitmap.Save((Join-Path $OutputRoot "$Name.png"))
    }finally{$graphics.Dispose();$bitmap.Dispose()}
}
$startup=[Diagnostics.Stopwatch]::StartNew()
$process=Start-Process $Executable -WorkingDirectory (Split-Path $Executable) -PassThru -RedirectStandardError (Join-Path $OutputRoot 'stderr.log')
$null=$process.Handle
$result=[ordered]@{Executable=$Executable;Passed=$false}
try{
    do {
        if($process.HasExited){throw "Exited during startup: $($process.ExitCode)"}
        $windows=@([BlackMoonGuiTest]::Windows($process.Id))
        $main=$windows | Where-Object Class -eq 'zyWindow' | Select-Object -First 1
        $ready=$false
        if($main){
            try { $null=Assert-Layout $windows; $ready=$true } catch { $layoutError=$_.Exception.Message }
        }
        if($ready){
            try {
                [BlackMoonGuiTest]::Send($main.Handle,0,100)
                $ready=$process.WaitForInputIdle(100)
            } catch { $ready=$false }
        }
        if(-not $ready){Start-Sleep -Milliseconds 20}
    } while(-not $ready -and $startup.ElapsedMilliseconds -lt 60000)
    if(-not $ready){throw "Startup layout not ready: $layoutError"}
    $result.StartupIdleMilliseconds=$startup.ElapsedMilliseconds
    $result.Initial=$windows
    [BlackMoonGuiTest]::SetWindowPos([IntPtr]$main.Handle,[IntPtr](-1),80,80,0,0,0x41)|Out-Null
    Start-Sleep -Milliseconds 500
    Save-Screenshot $main 'initial'
    $windows=@([BlackMoonGuiTest]::Windows($process.Id))
    $initialWidth=Assert-Layout $windows
    if($Mode -eq 'simple'){foreach($caption in @('按钮一','按钮二')){
        $button=$windows | Where-Object Title -eq $caption | Select-Object -First 1
        [BlackMoonGuiTest]::Send($button.Handle,0xF5)
        $windows=@([BlackMoonGuiTest]::Windows($process.Id))
        if(-not ($windows | Where-Object Title -eq "你点击了 $caption")){throw "Click callback failed: $caption"}
    }}else{
        $search=$windows | Where-Object Class -eq 'Edit' | Sort-Object Height | Select-Object -First 1
        [BlackMoonGuiTest]::Text($search.Handle,'GUI 输入测试')|Out-Null
        if([BlackMoonGuiTest]::Text($search.Handle) -ne 'GUI 输入测试'){throw 'Text entry failed'}
        $check=$windows | Where-Object Title -eq '自动应用约束' | Select-Object -First 1
        if(-not $check){throw 'Checkbox missing'}
        $before=[BlackMoonGuiTest]::GetCheck($check.Handle)
        [BlackMoonGuiTest]::Send($check.Handle,0xF5)
        if([BlackMoonGuiTest]::GetCheck($check.Handle) -eq $before){throw 'Checkbox did not toggle'}
        $button=$windows | Where-Object Title -eq '新建' | Select-Object -First 1
        [BlackMoonGuiTest]::Send($button.Handle,0xF5)
    }
    $width=if($Mode -eq 'workbench'){1280}else{640}
    $height=if($Mode -eq 'workbench'){800}else{400}
    $resize=[Diagnostics.Stopwatch]::StartNew()
    [BlackMoonGuiTest]::SetWindowPos([IntPtr]$main.Handle,[IntPtr]::Zero,80,80,$width,$height,0x44)|Out-Null
    [BlackMoonGuiTest]::Send($main.Handle,0)
    $result.ResizeMilliseconds=$resize.ElapsedMilliseconds
    $windows=@([BlackMoonGuiTest]::Windows($process.Id))
    $result.Resized=$windows
    if((Assert-Layout $windows) -le $initialWidth){throw 'Resizing did not expand the controls'}
    Save-Screenshot $main 'resized'
    [BlackMoonGuiTest]::PostMessageW([IntPtr]$main.Handle,0x10,[IntPtr]::Zero,[IntPtr]::Zero)|Out-Null
    if(-not $process.WaitForExit(10000)){throw 'WM_CLOSE did not terminate the application'}
    if($process.ExitCode -ne 0){throw "Exit failed: $($process.ExitCode)"}
    if($MaxStartupMilliseconds -gt 0 -and $result.StartupIdleMilliseconds -gt $MaxStartupMilliseconds){throw "Startup exceeded budget: $($result.StartupIdleMilliseconds) ms"}
    if($MaxResizeMilliseconds -gt 0 -and $result.ResizeMilliseconds -gt $MaxResizeMilliseconds){throw "Resize exceeded budget: $($result.ResizeMilliseconds) ms"}
    $result.Passed=$true
    Write-Host "Startup idle: $($result.StartupIdleMilliseconds) ms; resize: $($result.ResizeMilliseconds) ms"
    Write-Host "PASS $Mode window, layout, control interaction, resizing and clean close"
}catch{
    $result.Error=$_.Exception.Message
    throw
}finally{
    $result | ConvertTo-Json -Depth 6 | Set-Content (Join-Path $OutputRoot 'result.json') -Encoding utf8
    if(-not $process.HasExited){Stop-Process -Id $process.Id}
    $process.Dispose()
}
