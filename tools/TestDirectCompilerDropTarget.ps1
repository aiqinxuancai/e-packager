param(
    [string]$Packager = "$PSScriptRoot/../bin/Win32/Release/e-packager.exe",
    [string]$CoreDirectory = 'D:\git\BlackMoonKernelStaticLib\adapter',
    [string]$EDirectory = 'C:\Users\aiqin\OneDrive\e5.6',
    [string]$OutputRoot = "$PSScriptRoot/../temp/drop-target-integration"
)
# 使用进程内 WM_DROPFILES 验证生成代码的控件与事件绑定；OLE 数据格式另由 TestNativeDropTarget 覆盖。
$ErrorActionPreference='Stop'
$Packager=[IO.Path]::GetFullPath($Packager)
$OutputRoot=[IO.Path]::GetFullPath($OutputRoot)
$workspace=Join-Path $OutputRoot 'workspace'
& $Packager unpack "$PSScriptRoot/../eproj/e-window-exe-new-proj.e" $workspace --main-only
if($LASTEXITCODE -ne 0){throw 'unpack failed'}
function Write-Source($name,$content){[IO.File]::WriteAllText("$workspace/src/$name",($content -replace '\r?\n',"`r`n"),[Text.UTF8Encoding]::new($true))}
Write-Source '窗口程序集_启动窗口.txt' @'
.版本 2
.程序集 窗口程序集_启动窗口
.子程序 _启动子程序, 整数型
载入 (_启动窗口, , 假)
返回 (0)
.子程序 开始
.如果真 (拖放对象1.注册拖放控件 (图片框1.取窗口句柄 ()))
    _启动窗口.标题 ＝ “registered”
    测试拖入 (图片框1.取窗口句柄 ())
    拖放对象1.撤消拖放控件 (图片框1.取窗口句柄 ())
.如果真结束
.子程序 文件事件
.参数 路径, 文本型
_启动窗口.标题 ＝ _启动窗口.标题 ＋ “|” ＋ 路径
'@
Write-Source '_启动窗口.xml' @'
<?xml version="1.0" encoding="UTF-8"?>
<窗口 名称="_启动窗口" 左边="50" 顶边="50" 宽度="400" 高度="200" 标题="fixture" 边框="2">
  <窗口.事件 索引="0" 名称="创建完毕" 处理器="窗口程序集_启动窗口::开始" />
  <图片框 名称="图片框1" 左边="0" 顶边="0" 宽度="350" 高度="150" 可视="真">
    <拖放对象 名称="拖放对象1" 左边="0" 顶边="0" 宽度="32" 高度="32" 可视="假" 接收文本="假" 接收超文本="假" 接收URL="假" 接收文件="真">
      <拖放对象.事件 索引="3" 名称="得到文件" 处理器="窗口程序集_启动窗口::文件事件" />
    </拖放对象>
  </图片框>
</窗口>
'@
$helper=@'
#include <windows.h>
#include <shlobj.h>
extern "C" __declspec(dllexport) int __stdcall TestDrop(HWND target){
    const wchar_t files[]=L"C:\\图片\\甲.bmp\0D:\\乙.png\0";
    HGLOBAL block=GlobalAlloc(GHND,sizeof(DROPFILES)+sizeof(files));
    auto* data=static_cast<DROPFILES*>(GlobalLock(block));data->pFiles=sizeof(DROPFILES);data->fWide=TRUE;
    memcpy(reinterpret_cast<char*>(data)+sizeof(DROPFILES),files,sizeof(files));GlobalUnlock(block);
    SendMessageW(target,WM_DROPFILES,reinterpret_cast<WPARAM>(block),0);return 1;
}
'@
[IO.File]::WriteAllText("$OutputRoot/helper.cpp",$helper,[Text.UTF8Encoding]::new($true))
$vswhere="${env:ProgramFiles(x86)}/Microsoft Visual Studio/Installer/vswhere.exe"
$vs=& $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (-not ('DropEventFixture' -as [type])) {
    Add-Type @'
using System;
using System.Text;
using System.Runtime.InteropServices;
public static class DropEventFixture {
    public delegate bool EnumProc(IntPtr window, IntPtr data);
    [DllImport("user32.dll")] public static extern bool EnumWindows(EnumProc callback, IntPtr data);
    [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr window, out uint pid);
    [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern int GetWindowText(IntPtr window, StringBuilder text, int count);
    [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern int GetClassName(IntPtr window, StringBuilder text, int count);
    [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern IntPtr SendMessageTimeout(IntPtr window, uint message, UIntPtr wParam, IntPtr lParam, uint flags, uint timeout, out UIntPtr result);
    public static IntPtr Find(uint pid) {
        IntPtr result=IntPtr.Zero;
        EnumWindows((h,d)=> { uint owner; GetWindowThreadProcessId(h,out owner);var name=new StringBuilder(80);GetClassName(h,name,80);if(owner==pid&&name.ToString()=="ecompiler_window_form")result=h;return true; },IntPtr.Zero);
        return result;
    }
    public static void Close(IntPtr main) { UIntPtr result;SendMessageTimeout(main,0x10,UIntPtr.Zero,IntPtr.Zero,2,3000,out result); }
}
'@
}

foreach($arch in @('x86','x64')){
    $output=Join-Path $OutputRoot $arch
    New-Item -ItemType Directory -Force $output | Out-Null
    $export=if($arch -eq 'x86'){'/EXPORT:TestDrop=_TestDrop@4'}else{'/EXPORT:TestDrop'}
    $batch="@call `"$vs/VC/Auxiliary/Build/vcvarsall.bat`" $arch >nul`r`n@cl /nologo /EHsc /utf-8 /LD ../helper.cpp /Fe:drop-test.dll /Fo:helper.obj user32.lib /link $export`r`n"
    [IO.File]::WriteAllText("$output/build.cmd",$batch)
    Push-Location $output
    try{& cmd /c build.cmd;if($LASTEXITCODE -ne 0){throw 'helper compile failed'}}finally{Pop-Location}
    $pointer=if($arch -eq 'x86'){'整数型'}else{'长整数型'}
    Write-Source '.DLL声明.txt' ".版本 2`n.DLL命令 测试拖入, 整数型, `"drop-test.dll`", `"TestDrop`"`n    .参数 窗口, $pointer`n"
    $compiler=if($arch -eq 'x64'){[IO.Path]::GetFullPath("$PSScriptRoot/../bin/x64/Release/e-packager.exe")}else{$Packager}
    & $compiler compile $workspace "$output/drop-target.exe" --arch $arch --e-dir $EDirectory --blackmoon-core-dir $CoreDirectory *> "$output/compile.log"
    if($LASTEXITCODE -ne 0){Get-Content "$output/compile.log" -Tail 25;throw "$arch compile failed"}
    $process=Start-Process "$output/drop-target.exe" -WorkingDirectory $output -WindowStyle Hidden -PassThru
    try{
        $title=''
        for($i=0;$i -lt 100;$i++){
            Start-Sleep -Milliseconds 100;$process.Refresh()
            if($process.HasExited){throw "startup exited $($process.ExitCode)"}
            $main=[DropEventFixture]::Find($process.Id)
            $text=[Text.StringBuilder]::new(512)
            [void][DropEventFixture]::GetWindowText($main,$text,512)
            $title=$text.ToString()
            if($title -eq 'registered|C:\图片\甲.bmp|D:\乙.png'){break}
        }
        if($title -ne 'registered|C:\图片\甲.bmp|D:\乙.png'){throw "incorrect title: $title"}
        [DropEventFixture]::Close($main)
        if(-not $process.WaitForExit(5000)){throw 'shutdown timed out'}
        if($process.ExitCode -ne 0){throw "shutdown error $($process.ExitCode)"}
        Write-Host "$arch PASS: compile, registration, Unicode multi-file event binding, unregister, clean exit"
    }finally{if(-not $process.HasExited){Stop-Process -Id $process.Id -Force}}
}
