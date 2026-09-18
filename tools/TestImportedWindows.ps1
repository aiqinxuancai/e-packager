param([string]$OutputRoot="$PSScriptRoot/../temp/imported-windows",[string]$CoreDirectory='D:\git\BlackMoonKernelStaticLib\adapter')
$ErrorActionPreference='Stop'
$OutputRoot=[IO.Path]::GetFullPath($OutputRoot)
$x86=[IO.Path]::GetFullPath("$PSScriptRoot/../bin/Win32/Release/e-packager.exe")
$workspace=Join-Path $OutputRoot 'workspace'
$module=Join-Path $workspace 'ecom/fixture'
foreach($path in @($workspace,$module)){
    & $x86 unpack "$PSScriptRoot/../eproj/e-window-exe-new-proj.e" $path --main-only
    if($LASTEXITCODE -ne 0){throw 'unpack failed'}
}
function Write-Text($path,$text){[IO.File]::WriteAllText($path,($text -replace '\r?\n',"`r`n"),[Text.UTF8Encoding]::new($true))}
Write-Text "$workspace/src/窗口程序集_启动窗口.txt" @'
.版本 2
.程序集 窗口程序集_启动窗口
.子程序 _启动子程序, 整数型
载入 (_启动窗口, , 假)
返回 (0)
.子程序 开始
显示模块 ()
'@
Write-Text "$module/src/窗口程序集_启动窗口.txt" @'
.版本 2
.程序集 窗口程序集_启动窗口
.子程序 显示模块, , 公开
载入 (_启动窗口, , 假)
.子程序 模块创建
_启动窗口.标题 ＝ “module-ok”
'@
foreach($path in @($workspace,$module)){
    $handler=if($path -eq $workspace){'开始'}else{'模块创建'}
    $title=if($path -eq $workspace){'main'}else{'module-pending'}
    Write-Text "$path/src/_启动窗口.xml" "<窗口 名称=`"_启动窗口`" 标题=`"$title`" 宽度=`"300`" 高度=`"150`"><窗口.事件 索引=`"0`" 名称=`"创建完毕`" 处理器=`"窗口程序集_启动窗口::$handler`" /></窗口>"
    $meta=Get-Content "$path/project/_meta.json" -Raw | ConvertFrom-Json
    $meta.windowBindings=@(@{formName='_启动窗口';className='窗口程序集_启动窗口'})
    Write-Text "$path/project/_meta.json" ($meta | ConvertTo-Json -Depth 20)
}
$dependencies=Get-Content "$workspace/project/.module.json" -Raw | ConvertFrom-Json
$dependencies.dependencies+=@{kind='ecom';name='fixture';path='fixture.ec';localWorkspace='ecom/fixture'}
Write-Text "$workspace/project/.module.json" ($dependencies | ConvertTo-Json -Depth 20)
Add-Type @'
using System;
using System.Text;
using System.Collections.Generic;
using System.Runtime.InteropServices;
public static class ImportedWindowProbe {
    delegate bool Callback(IntPtr window,IntPtr data);
    [DllImport("user32.dll")] static extern bool EnumWindows(Callback callback,IntPtr data);
    [DllImport("user32.dll")] static extern uint GetWindowThreadProcessId(IntPtr window,out uint pid);
    [DllImport("user32.dll",CharSet=CharSet.Unicode)] static extern int GetWindowText(IntPtr window,StringBuilder text,int size);
    [DllImport("user32.dll")] public static extern bool PostMessageW(IntPtr window,uint message,IntPtr w,IntPtr l);
    public static Dictionary<string,IntPtr> Read(int pid){var result=new Dictionary<string,IntPtr>();EnumWindows((w,d)=>{uint owner;GetWindowThreadProcessId(w,out owner);if(owner==pid){var text=new StringBuilder(200);GetWindowText(w,text,200);result[text.ToString()]=w;}return true;},IntPtr.Zero);return result;}
}
'@
foreach($arch in @('x86','x64')){
    $tool=if($arch -eq 'x86'){$x86}else{[IO.Path]::GetFullPath("$PSScriptRoot/../bin/x64/Release/e-packager.exe")}
    $output=Join-Path $OutputRoot "$arch.exe"
    & $tool compile $workspace $output --arch $arch --blackmoon-core-dir $CoreDirectory *> "$OutputRoot/$arch.log"
    if($LASTEXITCODE -ne 0){Get-Content "$OutputRoot/$arch.log" -Tail 15;throw 'compile failed'}
    $process=Start-Process $output -WindowStyle Hidden -PassThru
    try{
        for($i=0;$i -lt 50;$i++){
            if($process.HasExited){throw "startup failed $($process.ExitCode)"}
            $windows=[ImportedWindowProbe]::Read($process.Id)
            if($windows.ContainsKey('main') -and $windows.ContainsKey('module-ok')){break}
            Start-Sleep -Milliseconds 100
        }
        if(-not $windows.ContainsKey('main') -or -not $windows.ContainsKey('module-ok')){throw 'missing imported window or incorrect event binding'}
        foreach($name in @('module-ok','main')){[void][ImportedWindowProbe]::PostMessageW($windows[$name],0x10,[IntPtr]::Zero,[IntPtr]::Zero)}
        if(-not $process.WaitForExit(5000) -or $process.ExitCode -ne 0){throw 'shutdown failed'}
        Write-Host "PASS ${arch}: imported form, colliding form/assembly names, module creation event, clean exit"
    }finally{if(-not $process.HasExited){Stop-Process -Id $process.Id};$process.Dispose()}
}
