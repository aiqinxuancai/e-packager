# e-packager v1.2.6：x86 MFC 库探测失败复现

本文记录 V2 基准使用 e-packager 独立编译时遇到的 `NAFXCW.LIB` 链接失败，供 e-packager 修复使用。

## 环境

- Windows
- e-packager `v1.2.6`
- 目标架构：`x86`
- 工程底座：`e-console-exe-new-proj.e`
- Visual Studio 18 Professional
- MSVC 工具集：`14.51.36231`

当前机器的 e-packager：

```text
D:\git\e-language-bench\work\toolchain-v1.2.6\e-packager.exe
```

版本确认：

```powershell
& D:\git\e-language-bench\work\toolchain-v1.2.6\e-packager.exe --version
```

输出：

```text
e-packager v1.2.6
```

## 复现步骤

先将模板拆包到临时目录：

```powershell
$root = Join-Path $env:TEMP ("e-packager-mfc-repro-" + [guid]::NewGuid())
New-Item -ItemType Directory $root | Out-Null

& D:\git\e-language-bench\work\toolchain-v1.2.6\e-packager.exe `
  unpack D:\git\e-packager\eproj\e-console-exe-new-proj.e `
  "$root\workspace" --main-only
```

直接编译拆包工作区：

```powershell
& D:\git\e-language-bench\work\toolchain-v1.2.6\e-packager.exe `
  compile "$root\workspace" "$root\output.exe" `
  --arch x86 --subsystem console
```

实际结果：

```text
e-packager v1.2.6
compile failed: mfc_runtime_file_not_found:C:\Program Files\Microsoft Visual Studio\18\Professional\VC\Tools\MSVC\14.51.36231\lib\x86\NAFXCW.LIB
```

编译过程已经生成了 C++ 和 OBJ 文件，失败发生在链接阶段。典型临时目录内容包括：

```text
output.generated.cpp
output.obj
output.resources.rc
output.resources.res
```

## 依赖实际位置

报错路径不存在，但同版本 MFC 库实际位于 MSVC 的 `atlmfc` 目录：

```text
C:\Program Files\Microsoft Visual Studio\18\Professional\VC\Tools\MSVC\14.51.36231\atlmfc\lib\x86\nafxcw.lib
```

验证：

```powershell
Get-ChildItem `
  'C:\Program Files\Microsoft Visual Studio' `
  -Recurse -Filter NAFXCW.LIB |
  Select-Object FullName
```

当前输出显示 e-packager 查找的是：

```text
...\VC\Tools\MSVC\14.51.36231\lib\x86\NAFXCW.LIB
```

而正确目录是：

```text
...\VC\Tools\MSVC\14.51.36231\atlmfc\lib\x86\nafxcw.lib
```

Windows 文件系统通常不区分大小写，因此问题不是文件名大小写，而是缺少 `atlmfc\lib\x86` 搜索路径。

## 期望行为

当语义编译判断工程需要 MFC 时，e-packager 应将对应架构的 MFC 库目录加入链接搜索路径，至少包括：

```text
<vc-tools-dir>\atlmfc\lib\x86
```

对于 x64，应使用：

```text
<vc-tools-dir>\atlmfc\lib\x64
```

随后应能找到 `nafxcw.lib` 并继续完成链接，生成目标 EXE。若库确实不存在，错误应明确指出缺失的是 `atlmfc` 组件，而不是只报告普通 `lib\x86` 路径。

## 基准调用方式

e-language-bench V2 使用以下命令调用 e-packager：

```text
e-packager compile <workspace> <project-root>\.temp\<case>.exe --arch x86 --subsystem console
```

编译结果放在项目根目录 `.temp/`，不启动 AutoLinker 或 EIDE。修复后可直接重新运行 V2 的工具链集成测试验证。
