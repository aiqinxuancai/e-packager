param(
    [ValidateSet("speed","size")][string]$CodegenOpt = "speed",
    [ValidateSet("baseline","reachable","typed")][string]$SemanticOpt = "baseline",
    [string]$Packager = "$PSScriptRoot/../bin/Win32/Release/e-packager.exe",
    [string]$EDirectory = 'C:\Users\aiqin\OneDrive\e5.6',
    [string]$OutputRoot = "$PSScriptRoot/../temp/direct-semantics-$([guid]::NewGuid().ToString('N'))"
)
$ErrorActionPreference = 'Stop'
$Packager = (Resolve-Path $Packager).Path
$OutputRoot = [IO.Path]::GetFullPath($OutputRoot)
$workspace = Join-Path $OutputRoot 'workspace'
& $Packager unpack "$PSScriptRoot/../eproj/e-console-exe-new-proj.e" $workspace --main-only
if ($LASTEXITCODE -ne 0) { throw 'fixture unpack failed' }
function Write-Source([string]$Name, [string]$Content) {
    [IO.File]::WriteAllText((Join-Path $workspace "src/$Name.txt"),
        ($Content -replace '\r?\n', "`r`n"), [Text.UTF8Encoding]::new($true))
}
Write-Source '程序集1' @'
.版本 2
.程序集 程序集1, , , 普通程序集的注释不能使其成为类
.程序集变量 定时回调次数, 整数型
.子程序 _启动子程序, 整数型
.局部变量 返回_结果, 整数型
.局部变量 到循环尾_值, 整数型
.局部变量 跳出循环_值, 整数型
.局部变量 对象, 派生类
.局部变量 包装, 包装类型
.局部变量 数组, 整数型, , "2,3"
.局部变量 缓冲, 字节集
.局部变量 文本缓冲, 文本型
.局部变量 输出数组, 整数型, , "0"
.局部变量 字数组, 短整数型, , "5"
.局部变量 时间, 系统时间
返回_结果 ＝ 普通函数 ()
到循环尾_值 ＝ 4
跳出循环_值 ＝ 5
.如果真 (返回_结果 ≠ 7 或 到循环尾_值 ≠ 4 或 跳出循环_值 ≠ 5)
    返回 (1)
.如果真结束
.如果真 (对象.取值 () ≠ 8)
    返回 (2)
.如果真结束
.如果真 (包装.成员.取值 () ≠ 8)
    返回 (3)
.如果真结束
.如果真 (取数组下标 (数组) ≠ 2 或 取数组下标 (数组, 2) ≠ 3)
    返回 (4)
.如果真结束
.如果真 (取数组下标 (数组, 0) ≠ 0 或 取数组下标 (数组, 3) ≠ 0 或 取数组下标 (返回_结果) ≠ 0)
    返回 (5)
.如果真结束
.如果真 (资源机器码 () ≠ 42)
    返回 (6)
.如果真结束
.如果真 (通用机器码 () ≠ 43)
    返回 (7)
.如果真结束
.如果真 (对象.原生取值 () ≠ 37)
    返回 (8)
.如果真结束
.如果真 (通用文本机器码 () ≠ “原生文本”)
    返回 (9)
.如果真结束
.如果真 (长整数机器码 () ≠ 8589934635)
    返回 (10)
.如果真结束
缓冲 ＝ { 0, 0, 0, 0, 0, 0, 0, 0 }
.如果真 (转宽字符 (65001, 0, “abc”, 4, 缓冲, 4) ≠ 4)
    返回 (11)
.如果真结束
.如果真 (缓冲 [1] ≠ 97 或 缓冲 [3] ≠ 98 或 缓冲 [5] ≠ 99)
    返回 (12)
.如果真结束
文本缓冲 ＝ “........”
返回_结果 ＝ 转窄字符 (65001, 0, 缓冲, 4, 文本缓冲, 8, 0, 0)
.如果真 (返回_结果 ≠ 4 或 文本缓冲 ≠ “abc”)
    返回 (13)
.如果真结束
填充数组 (输出数组)
.如果真 (取数组成员数 (输出数组) ≠ 3)
    返回 (14)
.如果真结束
.如果真 (输出数组 [3] ≠ 73 或 到整数 (“”) ≠ 0)
    返回 (15)
.如果真结束
.如果真 (读取文本参数 (“abcd”) ≠ 1684234849)
    返回 (16)
.如果真结束
.如果真 (读取字节集参数 ({ 1, 2, 3 }) ≠ 3)
    返回 (17)
.如果真结束
字数组 [1] ＝ 77
.如果真 (数组转宽字符 (65001, 0, “abc”, 4, 字数组 [2], 4) ≠ 4)
    返回 (18)
.如果真结束
.如果真 (字数组 [1] ≠ 77 或 字数组 [2] ≠ 97 或 字数组 [4] ≠ 99 或 字数组 [5] ≠ 0)
    返回 (19)
.如果真结束
.如果真 ({ 1, 0, 2 } ＝ {} 或 { 1, 0, 2 } ＝ { 1, 0, 3 } 或 { 1, 0, 2 } ≠ { 1, 0, 2 })
    返回 (20)
.如果真结束
.如果真 (读取模块初始化值 () ≠ 42)
    返回 (21)
.如果真结束
读取系统时间 (时间)
.如果真 (时间.年 ＜ 2020 或 时间.月 ＜ 1 或 时间.月 ＞ 12)
    返回 (22)
.如果真结束
文本缓冲 ＝ “buffer must survive native pointer return”
.如果真 (指针文本长度 (原生文本指针 (文本缓冲)) ≠ 41)
    返回 (23)
.如果真结束
.如果真 (调用字节集回调 ({ 184, 123, 0, 0, 0, 194, 16, 0 }, 0, 0, 0, 0) ≠ 123)
    返回 (24)
.如果真结束
缓冲 ＝ { 97, 98, 99, 0 }
.如果真 (指针文本长度 (字节集取地址 (缓冲, 缓冲, 0)) ≠ 3)
    返回 (25)
.如果真结束
.如果真 (转发缺省字节集 () ≠ 1)
    返回 (26)
.如果真结束
.如果真 (转发缺省数组 () ≠ 1)
    返回 (27)
.如果真结束
.如果真 (验证事件派发 () ≠ 1)
    返回 (28)
.如果真结束
返回 (0)
.子程序 转发缺省数组, 整数型
.参数 数据, 字节集, 可空 数组
.如果真 (是否为空 (数据) ＝ 假 或 取数组成员数 (数据) ≠ 0)
    返回 (0)
.如果真结束
填充缺省数组 (数据)
.如果真 (取数组成员数 (数据) ≠ 2)
    返回 (0)
.如果真结束
返回 (选择 (数据 [2] ＝ { 65, 66 }, 1, 0))
.子程序 填充缺省数组
.参数 数据, 字节集, 参考 可空 数组
重定义数组 (数据, 假, 2)
数据 [2] ＝ { 65, 66 }
.子程序 验证事件派发, 整数型
.局部变量 定时器, 整数型
.局部变量 次数, 整数型
定时回调次数 ＝ 0
定时器 ＝ 创建测试定时器 (0, 0, 10, 到整数 (&测试定时回调))
.如果真 (定时器 ＝ 0)
    返回 (0)
.如果真结束
.计次循环首 (200, 次数)
    处理事件 ()
    .如果真 (定时回调次数 ＞ 0)
        跳出循环 ()
    .如果真结束
    测试休眠 (10)
.计次循环尾 ()
销毁测试定时器 (0, 定时器)
返回 (选择 (定时回调次数 ＞ 0, 1, 0))
.子程序 测试定时回调
.参数 窗口, 整数型
.参数 消息, 整数型
.参数 标识, 整数型
.参数 时刻, 整数型
定时回调次数 ＝ 定时回调次数 ＋ 1
.子程序 转发缺省字节集, 整数型
.参数 数据, 字节集, 可空
.如果真 (是否为空 (数据) ＝ 假)
    返回 (0)
.如果真结束
返回 (接收必需字节集 (数据))
.子程序 接收必需字节集, 整数型
.参数 数据, 字节集
.如果真 (取字节集右边 (数据, 2) ≠ {})
    返回 (0)
.如果真结束
返回 (1)
.子程序 原生文本指针, 整数型
.参数 文本, 文本型
置入代码 ({139, 69, 8, 139, 0, 201, 194, 4, 0})
返回 (0)
.子程序 读取文本参数, 整数型
.参数 文本, 文本型
置入代码 ({139, 69, 8, 139, 0, 139, 0, 201, 194, 4, 0})
返回 (0)
.子程序 读取字节集参数, 整数型
.参数 数据, 字节集
置入代码 ({139, 69, 8, 139, 0, 139, 64, 4, 201, 194, 4, 0})
返回 (0)
.子程序 填充数组
.参数 数据, 整数型, 数组
重定义数组 (数据, 假, 3)
数据 [3] ＝ 73
.子程序 通用文本机器码, 通用型
.局部变量 文本, 文本型
文本 ＝ “原生文本”
置入代码 ({185, 4, 0, 0, 128})
置入代码 ({139, 69, 252, 201, 194, 0, 0})
返回 (0)
.子程序 长整数机器码, 长整数型
置入代码 ({186, 2, 0, 0, 0})
置入代码 ({184, 43, 0, 0, 0, 201, 194, 0, 0})
返回 (0)
.子程序 通用机器码, 通用型
置入代码 ({185, 1, 3, 0, 128})
置入代码 ({184, 43, 0, 0, 0, 201, 194, 0, 0})
返回 (0)
.子程序 资源机器码, 整数型
置入代码 (#空指令)
返回 (42)
'@
Write-Source '辅助' @'
.版本 2
.程序集 辅助, , , 有注释的普通程序集
.子程序 普通函数, 整数型
返回 (7)
'@
Write-Source '基类' @'
.版本 2
.程序集 基类, <对象>
.子程序 取值, 整数型, 公开
返回 (7)
.子程序 取回调, 子程序指针, 公开
返回 (&普通函数)
.子程序 通用整数, 通用型, 公开
返回 (37)
.子程序 原生取值, 整数型, 公开
置入代码 ({139, 85, 8, 139, 2, 139, 0, 82, 255, 80, 8, 186, 0, 0, 0, 0, 129, 249, 1, 3, 0, 128, 15, 69, 194, 201, 194, 4, 0})
返回 (0)
.子程序 取结构, 系统时间, 公开
.局部变量 结果, 系统时间
结果.年 ＝ 2026
返回 (结果)
'@
Write-Source '派生类' @'
.版本 2
.程序集 派生类, 基类
.子程序 取值, 整数型, 公开
返回 (基类.取值 () ＋ 1)
'@
Write-Source '.数据类型' @'
.版本 2
.数据类型 包装类型
    .成员 成员, 派生类
.数据类型 系统时间
    .成员 年, 短整数型
    .成员 月, 短整数型
    .成员 星期, 短整数型
    .成员 日, 短整数型
    .成员 时, 短整数型
    .成员 分, 短整数型
    .成员 秒, 短整数型
    .成员 毫秒, 短整数型
'@
Write-Source '.DLL声明' @'
.版本 2
.DLL命令 读取系统时间, , , "GetSystemTime"
    .参数 时间, 系统时间, 传址
.DLL命令 指针文本长度, 整数型, "kernel32.dll", "lstrlenA"
    .参数 地址, 整数型
.DLL命令 创建测试定时器, 整数型, "user32.dll", "SetTimer"
    .参数 窗口, 整数型
    .参数 标识, 整数型
    .参数 间隔, 整数型
    .参数 回调, 整数型
.DLL命令 销毁测试定时器, 整数型, "user32.dll", "KillTimer"
    .参数 窗口, 整数型
    .参数 标识, 整数型
.DLL命令 测试休眠, , "kernel32.dll", "Sleep"
    .参数 毫秒, 整数型
.DLL命令 调用字节集回调, 整数型, "user32.dll", "CallWindowProcA"
    .参数 回调, 字节集
    .参数 窗口, 整数型
    .参数 消息, 整数型
    .参数 参数一, 整数型
    .参数 参数二, 整数型
.DLL命令 字节集取地址, 整数型, "kernel32.dll", "lstrcpynA"
    .参数 目标, 字节集
    .参数 源, 字节集
    .参数 长度, 整数型
.DLL命令 数组转宽字符, 整数型, "kernel32.dll", "MultiByteToWideChar"
    .参数 代码页, 整数型
    .参数 标志, 整数型
    .参数 输入, 文本型
    .参数 输入长度, 整数型
    .参数 输出, 短整数型, 传址
    .参数 输出长度, 整数型
.DLL命令 转宽字符, 整数型, "kernel32.dll", "MultiByteToWideChar"
    .参数 代码页, 整数型
    .参数 标志, 整数型
    .参数 输入, 文本型, 传址
    .参数 输入长度, 整数型
    .参数 输出, 字节集, 传址
    .参数 输出长度, 整数型
.DLL命令 转窄字符, 整数型, "kernel32.dll", "WideCharToMultiByte"
    .参数 代码页, 整数型
    .参数 标志, 整数型
    .参数 输入, 字节集, 传址
    .参数 输入长度, 整数型
    .参数 输出, 文本型, 传址
    .参数 输出长度, 整数型
    .参数 默认字符, 整数型
    .参数 使用默认, 整数型
'@
$resource = Join-Path $OutputRoot 'nop.bin'
[IO.File]::WriteAllBytes($resource, [byte[]]@(0x90))
& $Packager update $workspace --add-image "空指令=$resource"
if ($LASTEXITCODE -ne 0) { throw 'resource addition failed' }
$output = Join-Path $OutputRoot 'semantics-x86.exe'
$moduleWorkspace = Join-Path $workspace 'ecom/初始化模块'
& $Packager unpack "$PSScriptRoot/../eproj/e-console-exe-new-proj.e" $moduleWorkspace --main-only
if ($LASTEXITCODE -ne 0) { throw 'module fixture unpack failed' }
$moduleSource = @'
.版本 2
.程序集 模块程序集
.程序集变量 初始化值, 整数型
.子程序 _启动子程序, 整数型
初始化值 ＝ 40
_临时子程序 ()
返回 (0)
.子程序 _临时子程序
初始化值 ＝ 初始化值 ＋ 2
.子程序 读取模块初始化值, 整数型, 公开
返回 (初始化值)
'@
[IO.File]::WriteAllText((Join-Path $moduleWorkspace 'src/程序集1.txt'),
    ($moduleSource -replace '\r?\n', "`r`n"), [Text.UTF8Encoding]::new($true))
$moduleManifest = Join-Path $workspace 'project/.module.json'
$metadata = Get-Content -LiteralPath $moduleManifest -Raw | ConvertFrom-Json
$metadata.dependencies += [pscustomobject]@{ kind='ecom'; name='初始化模块'; path='初始化模块.ec'; localWorkspace='ecom/初始化模块' }
[IO.File]::WriteAllText($moduleManifest, ($metadata | ConvertTo-Json -Depth 30), [Text.UTF8Encoding]::new($true))
& $Packager compile $workspace $output --arch x86 --e-dir $EDirectory --codegen-opt $CodegenOpt --semantic-opt $SemanticOpt
if ($LASTEXITCODE -ne 0) { throw 'semantic fixture compilation failed' }
$process = Start-Process -FilePath $output -WorkingDirectory $OutputRoot -WindowStyle Hidden -PassThru
try {
    if (-not $process.WaitForExit(15000)) {
        Stop-Process -Id $process.Id
        throw 'semantic fixture timed out'
    }
    if ($process.ExitCode -ne 0) { throw "semantic fixture failed with check $($process.ExitCode)" }
} finally { $process.Dispose() }
Write-Host 'PASS ordinary assemblies, keyword boundaries, class members, qualified base calls, array bounds, resource machine code, native register preservation, generic returns and DLL text/binary buffers, implicit array references, binary equality, DLL array element buffers, DLL structs, pointer lifetime, module initialization, omitted binary/array parameters and timer dispatch'
