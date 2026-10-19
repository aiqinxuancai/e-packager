# e-packager

让易语言项目享有现代开发体验：Git 版本管理、代码 Diff、AI 辅助编辑、命令行编译。

```bash
# 解包：.e → 可读目录
e-packager unpack MyApp.e MyApp\

# 回包：目录 → .e
e-packager pack MyApp\ MyApp.e

# 直接编译：.e → .exe（无需打开 IDE）
e-packager compile MyApp.e MyApp.exe
```

> 📖 参考应用：[易语言 × AI Agent 实践白皮书](https://github.com/aiqinxuancai/Awesome-E-Agent)  
> 📊 AI 编辑源码前，可参考 [易语言大模型基准评分](https://e-language-bench.apptest.dev) 选择模型  
> 🔧 [AutoLinker 支持库](https://github.com/aiqinxuancai/AutoLinker) — 提供无头编译，用于 AI 编辑后的验证

---

## 📖 目录

- [快速开始](#-快速开始)
- [核心功能](#-核心功能)
  - [解包 / 回包](#1-解包--回包)
  - [源码直接编译](#2-源码直接编译试验性)
  - [回包前预检](#3-回包前预检)
  - [权威编译检查](#4-权威编译检查)
- [工作区目录结构](#-工作区目录结构)
- [其他命令](#-其他命令)
- [高级用法](#-高级用法)

---

## 🚀 快速开始

### 三步上手

```bash
# 1. 解包
e-packager unpack MyApp.e MyApp\

# 2. 编辑（用任意编辑器修改 MyApp\src\*.txt）
# ...

# 3. 回包
e-packager pack MyApp\ MyApp.e
```

### 更快的方式：拖拽解包

直接将 `.e` / `.ec` 文件拖放到 `e-packager.exe` 上，自动在源文件所在目录创建同名子目录并解包。

### 在目录内直接回包

在工作区根目录（或 `tool/` 子目录）内直接运行，自动输出到 `pack/` 目录：

```bash
e-packager
```

---

## 💎 核心功能

### 1. 解包 / 回包

#### 基本用法

```bash
# 解包
e-packager unpack <input.e|input.ec> <output-dir>

# 回包
e-packager pack <input-dir> <output.e|output.ec>
```

未修改的 `.ec` 拆包目录可以回包为原始 `.ec`，保持字节一致。显式指定 `.e` 输出时转换为易语言源工程；修改源码或资源后，请回包为 `.e` 并通过易语言编译为模块。工具不会把 `.e` 数据写入 `.ec` 文件，也不会在修改后复用旧模块快照。

EC 拆包保留内嵌的匿名类、隐藏函数及其依赖声明，统一按原生 ID 消歧。x64 处理经典 x86 支持库时，需要同目录的 `e-packager-x86.exe`（Release x64 构建会从已有 Win32 产物复制），或保留仓库的双架构 bin 目录布局。

#### 常用选项

```bash
# 快速解包（不刷新依赖模块和支持库接口）
e-packager unpack MyApp.e MyApp\ --main-only

# 带密码的文件
e-packager unpack MyApp.e MyApp\ --password 123456
e-packager pack MyApp\ MyApp.e --password 123456

# 导出支持库公开接口（仅 Win32 版）
e-packager decrypt-fne MyLib.fne MyLib.txt
```

---

### 2. 源码直接编译（试验性）

无需打开 IDE，一条命令把 `.e` 编译成 `.exe` 或 `.dll`。

> ⚠️ **实验性功能，不保证稳定性或与 IDE 的兼容性。只有用户明确要求使用无 IDE 编译 / `compile` 时才可调用**；不要自动用于日常源码编辑、回包或验证，也不要作为其它检查失败后的自动替代。需要完整的 VC/MSVC 工具链和 Windows SDK，建议通过 Visual Studio 安装“使用 C++ 的桌面开发”。`compile-check` 是依赖 IDE / AutoLinker 的另一条检查链路。

#### 🚀 从零上手教程

**完整教程**：[`docs/compile-from-source-guide.md`](docs/compile-from-source-guide.md)（含实测编译尺寸、GUI 子系统、DLL 导出与失败排查）

**基本用法**：

```bash
# 编译 EXE
e-packager compile MyApp.e MyApp.exe

# 编译 DLL
e-packager compile MyLib.e MyLib.dll

# 编译 x64 版本
e-packager compile MyApp.e MyApp-x64.exe --arch x64

# 编译 GUI 程序（无控制台窗口）
e-packager compile MyApp.e MyApp-gui.exe --subsystem windows
```

#### 环境要求

无 IDE 编译会自动检查 VS2022 / VS2026（含 Build Tools、自定义安装目录）的 VC 编译器、链接器、静态运行库和 Windows SDK。缺失时询问是否自动下载安装 **VS2026 Build Tools**：只选择 VC x86/x64 工具与 Windows SDK，不安装完整 IDE。仅输入 `y` 或 `Y` 确认，默认 `N`；空行、`n` 或标准输入 EOF 取消；此提示仅在用户主动使用实验性 `compile` 时触发。

安装需要联网，可能请求 Windows 管理员权限；下载后校验微软签名及 VS2026 版本。安装成功后重新检测并重试编译，随后仍可按原逻辑询问下载缺失的现代黑月依赖。安装失败、取消或要求重启时停止并显示原因，不会自动重启或循环安装。`.e` 与目录输入均支持工具链安装提示（目录输入仍需提前准备现代黑月 adapter）。

显式传入 `--vc-tools-dir`、`--windows-sdk-dir`、`--compiler` 或 `--linker` 时，以用户配置为准，不触发自动安装。CI 应预装依赖并关闭标准输入。已有 VS2026 Build Tools 会补充所需组件；已有完整 VS2022 工具链可直接复用。


1. **Visual Studio C++ 工具链**（编译器会自动探测）
   - 下载地址：https://visualstudio.microsoft.com/zh-hans/downloads/
   - 选择 **Visual Studio Community**（免费）
   - 安装时勾选：**使用 C++ 的桌面开发**
   - 详见：[完整安装指南](docs/compile-from-source-guide.md#-环境要求)

2. **核心库实现**（首次编译时自动下载）
   - 来源：[BlackMoonModernCore Release](https://github.com/aiqinxuancai/BlackMoonModernCore/releases)
   - 自动缓存到：`%LOCALAPPDATA%\e-packager\dependencies`

#### 编译模式

| 模式 | 架构 | 推荐场景 |
|------|------|----------|
| `semantic`（默认） | x86 / x64 | **推荐** — 现代工具链，支持 x64 |
| `legacy-blackmoon` | 仅 x86 | 仅当需要与旧编译产物保持逐字节一致 |

#### 常用选项

```bash
# 指定架构
--arch x86|x64

# 指定子系统
--subsystem auto|console|windows

# 指定核心库路径（CI 场景）
--blackmoon-x86-dir <路径>
--blackmoon-x64-dir <路径>

# JSON 格式诊断（供脚本/IDE 解析）
--diagnostics json
```

#### 实测编译尺寸

| 产物类型 | 架构 | 尺寸 |
|---------|------|------|
| 空白控制台工程 | x86 | ~114 KB |
| 完整测试工程（1606 行，137 个命令） | x86 | ~675 KB |
| 完整测试工程 | x64 | ~777 KB |
| 纯代码 GUI（一个信息框） | x86 | ~173 KB |

---

### 3. 回包前预检

`validate` 支持三档验证，通过 `--level` 选择；省略时默认为 `semantic`。

| 级别 | 检查内容 | 是否调用 IDE |
| --- | --- | --- |
| `editable` | 检查目录、声明和流程结构，并实际构造回包字节；允许部分源码错误以警告形式保留 | 否 |
| `semantic`（默认） | 严格检查语法、符号、调用签名和可确定的类型；仅做静态检查，不构造回包字节 | 否 |
| `compile` | 可编辑回包后，通过 AutoLinker 调用真实易语言 IDE 编译 | 是 |

```powershell
# 默认：静态语义预检，文本输出
e-packager validate MyApp\

# 可编辑回包验证
e-packager validate MyApp\ --level editable --diagnostics json

# 显式选择静态语义预检
e-packager validate MyApp\ --level semantic --diagnostics json

# 实际 IDE 编译验证（IDE 的 lib 目录需启用 AutoLinker.fne）
e-packager validate MyApp\ --level compile --diagnostics json --eide "C:\path\to\IDE.exe"

# 按易模块目标进行编译验证
e-packager validate MyModule\ --level compile --compile-target ecom --eide "C:\path\to\IDE.exe"
```

这三档并非逐级叠加：`compile` 执行的是“`editable` → IDE 实际编译”，不要求先通过 `semantic`，由真实编译器判断代码是否可编译。`validate --level compile` 使用 IDE / AutoLinker，与上节试验性的 `compile` 命令是不同链路。

`editable` 通过只表示能生成回包字节，不代表已实际验证 IDE 打开，也不代表源码可编译。JSON 中的 `rebuildVerified=true` 表示回包构造成功，`ideOpenVerified=false` 表示未独立验证 IDE 打开。`compile` 会生成临时源文件和编译产物进行验证，不运行生成的程序。

**静态语义预检覆盖范围**：

- ✅ 声明槽位、属性、数组维数
- ✅ 声明顺序、重复名称、类型冲突
- ✅ 智能引号/括号配对、赋值左右值
- ✅ 窗体 XML、控件成员、事件处理器
- ✅ 符号、成员、调用签名、类型错误

`pack` 和无参回包默认以可继续在 IDE 中编辑为目标，执行可编辑级别预检。可保留的表达式、未知调用等源码问题会产生警告，并以原生未检查语句保存；不会自动注释、修正或删除源码。无法安全构造的流程块、错误声明、缺失资源等问题仍会阻止回包。

**脚本集成**：使用 `--diagnostics json` 获取结构化结果。

| 退出码 | JSON `status` | 含义 |
| --- | --- | --- |
| `0` | `passed` | 所选级别验证通过 |
| `1` | `failed` | 验证失败，包括 IDE 明确报告编译失败 |
| `2` | `unavailable` | 无法完成编译验证，例如 IDE 不可用、超时或缺少可靠结果 |

JSON 包含 `level`、`status`、`ok`、`diagnostics` 和 `runtimeWarnings`；实际编译结果保存在 `ideResult` 中。请结合所选级别解释结果，不要将 `editable` 或 `semantic` 通过当作编译成功。

编译级别支持 `--eide`、`--compile-target auto|win_exe|win_console_exe|win_dll|ecom`、`--compile-static` 和 `--compile-timeout <seconds>`；这些选项仅可用于 `--level compile`。

> 💡 交付前可用 `validate --level compile` 检查工作区，或用 `compile-check` 检查已有 `.e` / `.ec` 文件。完整边界及测试方法见 [可编辑回包与分级验证](docs/editable-repack-validation.md)。

---

### 4. 权威编译检查

使用真实易语言 IDE 和 AutoLinker 执行编译检查：

```bash
# 对已有 .e 文件检查
e-packager compile-check MyApp.e

# 回包时同时检查
e-packager pack MyApp\ MyApp.e --compile-check

# 显式指定 IDE 路径（IDE 的 lib 目录需启用 AutoLinker.fne）
e-packager compile-check MyApp.e \
  --eide "C:\path\to\IDE.exe"
```

编译失败会输出 IDE 页面、行号和错误内容，且不会覆盖已有文件。

---

## 📁 工作区目录结构

解包后的目录结构：

| 路径 | 内容 |
|------|------|
| `src/` | 源码文件（`.txt`）及窗口界面定义（`.xml`） |
| `project/` | 封包所需元数据；`.e` 解包后还可能含原生快照，**请勿删除** |
| `ecom/` | 已解包的易模块工作区（`.e` 项目） |
| `elib/` | 依赖支持库的公开接口导出（仅供查阅） |
| `header/` | 公开接口头文件（`.ec` 项目） |
| `image/` | 图片资源，元数据在 `image/list.json` |
| `audio/` | 音频资源，元数据在 `audio/list.json` |
| `tool/e-packager.exe` | 随目录自带的封包工具 |
| `info.json` | 来源文件的类型、路径、修改时间、MD5 |
| `README.md` | 自动生成的项目结构、工具来源与回包方法说明 |
| `AGENTS.md` | 供 AI Agent 阅读的项目结构说明 |

---

回包时，普通常量按 `src/.常量.txt` 的行序排列；图片和音频按资源索引的 `order` 顺序排列。调整或重命名普通常量不会改变资源自身的顺序。原生常量资源表中的交错位置仅作为布局参考，发生冲突时优先保持常量页与资源索引各自的顺序。

`project/_meta.json` 的 `systemInfo` 保存工程类型、编译器版本和系统保留数据。修改源码或执行 `update` 不会改变这些工程属性；旧目录会从 `.native_source.bin` 自动提取，原生文件缺失时无法恢复已丢失的信息。

`systemInfo.compileType` 的已知值为 `0`（窗口 EXE）、`1`（控制台 EXE）、`2`（DLL）、`1000`（易模块），未知整数值也会保留。`projectSubsystem` 仅用于 EXE，显式设置 `windows` / `console` 时会更新 EXE 类型；DLL、易模块及未知类型应为 `unknown`，冲突配置会报错。主动转换工程种类时需同时设置这两个字段，并自行调整对应的启动代码和公开接口。

## 🔧 其他命令

### 刷新派生内容

解包后，若依赖的易模块或支持库发生变化，或需要增删依赖、资源，可用 `update` 刷新：

```bash
# 刷新所有派生内容
e-packager update MyApp\

# 新增易模块
e-packager update MyApp\ --add-ecom D:\modules\MyLib.ec

# 新增支持库（仅 Win32 版）
e-packager update MyApp\ --add-elib 互联网支持库

# 新增图片资源（代码中写 #logo）
e-packager update MyApp\ --add-image D:\res\logo.png

# 新增音频资源（代码中写 #notify）
e-packager update MyApp\ --add-audio D:\res\notify.wav

# 显式指定资源常量名（代码中写 #启动画面）
e-packager update MyApp\ --add-image 启动画面=D:\res\splash.bin

# 删除易模块（名称、文件名或依赖记录中的路径）
e-packager update MyApp\ --remove-ecom MyLib.ec

# 删除支持库（名称、文件名、不含扩展名的文件名或 GUID；两种架构均支持）
e-packager update MyApp\ --remove-elib spec

# 删除图片、音频资源（常量名，可带 #；也支持索引 key 或 relativePath）
e-packager update MyApp\ --remove-image 启动画面 --remove-audio notify
```

目录可省略，`update` 子命令也可在增删操作时省略：

```powershell
.\tool\e-packager.exe --add-image logo.png
.\tool\e-packager.exe --remove-image logo
.\tool\e-packager.exe update --add-ecom D:\modules\MyLib.ec
.\tool\e-packager.exe update --remove-elib spec
.\tool\e-packager.exe update
```

所有 `--add-*` / `--remove-*` 均支持这两种写法。省略目录时，优先选择当前工作区（或当前 `tool/` 的父目录），否则选择工具自身所在的工作区；找不到工作区会报错。相对资源文件路径仍相对于当前工作目录解析。显式指定目录的写法继续有效。

每次解包都会将当前工具复制为 `tool/e-packager.exe` 并覆盖已有副本；源和目标为同一个文件时直接保留，不执行自复制。

增删参数均可重复，也可组合使用；同一次 `update` 先删除再新增，可用来替换同名资源。删除选择器必须唯一匹配对应类型，找不到或存在歧义会报错，参数验证失败不会写回工作区；同一次命令重复指定同一删除目标只计一次。

删除资源会更新 `list.json`、工程树，并删除资源目录中不再被其它资源引用的对应文件。删除依赖会刷新 `ecom/`、`elib/` 派生内容，不会删除原始 `.ec` 或支持库文件。增删均使旧原生字节快照失效，后续封包按语义重建；源码中对已删项的引用需自行修改，可用 `validate <目录> --level semantic` 检查。普通可编辑回包会警告并保留待修源码文本，不会恢复已删除的资源或依赖。

### 直接编译的图标与属性

`semantic` 编译支持 x86/x64 的 EXE 图标、文件属性，以及 DLL 版本资源。在拆包目录中创建 `project/executable.json`：

```json
{
  "icon": "../assets/app.ico",
  "fileDescription": "程序说明",
  "productName": "产品名称",
  "companyName": "开发者或公司名称",
  "author": "作者",
  "legalCopyright": "Copyright (C) 2026 作者",
  "fileVersion": "1.0.0.0",
  "productVersion": "1.0.0.0"
}
```

所有字段可省略。JSON 使用 UTF-8（允许 BOM），`icon` 相对于配置文件解析。ICO 可包含 16、32、48、256 像素等多个尺寸。版本接受一至四段数字，补齐为四段，每段范围为 `0–65535`。

```powershell
# 自动读取 MyApp/project/executable.json
e-packager compile MyApp MyApp.exe --arch x64

# 替换默认配置，并覆盖配置内的图标；也适用于直接编译 .e
e-packager compile MyApp.e MyApp.exe --exe-config release.json --icon assets/app.ico
```

`--exe-config` 替换整个默认配置，不与其合并；`--icon` 优先于配置文件。命令行路径相对于当前工作目录。未配置图标时继承原工程已保存的图标；未配置产品名称、文件描述及版本时继承工程名称和版本，缺少工程版本时使用 `1.0.0.0`。公司、作者和版权不自动推断。`OriginalFilename` 自动使用输出文件名。

`author` 写入自定义 `Author` 字段，Windows 资源管理器不保证显示；建议用 `companyName` 展示开发者。此配置不提供数字签名，也不改变签名中的发布者。窗口默认加载 EXE 的大小图标，窗口自身显式设置的图标优先。

配置只用于直接编译，不写回 `.e`，目录 `update` 会保留此文件。传统 `legacy-blackmoon` 不读取该配置，并拒绝新命令行参数。无配置的工程仍可编译。非法字段、版本、图标或资源编译错误会导致编译失败；资源编译日志保存在输出目录的 `<名称>.resources.log` 中。

### 实用工具

```bash
# 自动更新到最新版本
e-packager /update

# 查看当前版本
e-packager version

# 比较原文件与目录内容是否一致
e-packager compare-bundle MyApp.e MyApp\

# 往返验证（解包后立即回包并校验）
e-packager verify-roundtrip MyApp.e temp\ verified.e
```

---

## ⚙️ 高级用法

### 运行日志

`e2txt_trace.log` 也会在每次主命令开始记录日志时清空，只保留本次运行的解包跟踪。辅助进程不执行清空；仅查询版本不改动日志。没有跟踪日志时不会仅为清空而创建文件；清空失败会在控制台及 `e-packager.log` 中记录 `trace_log_reset_failed` 和系统错误。

运行时优先在**启动时的当前工作目录**写入 `e-packager.log`（不一定是 EXE 或输入工程所在目录）。主命令首次写入前清空旧日志，只保留本次运行记录；x86 辅助进程继续追加，不会清空主进程的日志。首次写入成功后，控制台会显示 `日志文件: <完整路径>`；此提示输出到 stderr，不影响 JSON 标准输出。仅查询版本不改动日志。

如果该位置无法创建或写入日志，会依次尝试 EXE 所在目录、`%LOCALAPPDATA%\e-packager\logs\e-packager.log`、系统临时目录下的 `e-packager\logs\e-packager.log`，并显示实际位置及前面位置失败的系统错误。全部失败时会明确提示 `日志写入失败`，不会静默丢弃。日志采用 UTF-8 BOM 和 CRLF，每条记录包含时间、级别、进程 ID 和进程位数，便于区分 x64 主进程与 x86 辅助进程。

日志记录易语言安装目录的探测来源、选定路径和缺失的必要子目录、支持库候选搜索路径及实际加载路径，以及找不到文件、DLL 加载失败、`GetNewInf` 缺失或异常、x86 辅助进程失败等原因。Win32 加载错误同时记录错误码和系统说明，运行警告也会同步保存。目录探测结果在每个进程内记录一次。

出现 `_Lib0Cmd203` 等未解码命令时，可在日志中搜索该占位名或 `support_command_unresolved`。记录包含源文件、所在子程序、库名和索引、命令编号、实际库路径、工程要求和实际加载的 GUID/版本，以及命令表声明数量和解码数量。`reason` 区分库未找到、元数据加载失败、库索引越界、命令表不可读、命令编号越界、命令名为空或不可读；`detail` 保留加载器或辅助进程返回的具体错误。

窗口控件属性由隔离的 Win32 辅助进程处理，同一次命令复用进程和当前工程的支持库环境。请求通过独立的命名管道在内存中传输；隐藏宿主窗口复用，每条请求的临时控件和容器仍单独销毁。请求超时或进程崩溃后，后续请求会重新启动辅助进程；主程序结束时回收辅助进程。x64 发布包需携带同次构建的 `e-packager-x86.exe`。

排查窗口解包变慢时，搜索 `property_worker_started` 查看启动次数及 `transport=pipe`，搜索 `property_worker_request` 查看操作、控件类型/编号、源文件和失败原因。`elapsed_us` 是请求总耗时（微秒，包含通信和必要的进程启动），`worker_us` 是辅助进程实际处理耗时（包含临时控件清理）；`elapsed_ms` 保留毫秒表示。日志句柄在进程内复用，每条记录仍即时写入并通过文件锁保证多进程追加完整。

排查其他解包开销时，搜索 `operation_stage`。`operation` 表示计时范围，`stage` 表示阶段，`elapsed_us` 是该范围内本阶段的独立耗时（微秒），`source` 标明主工程或具体依赖模块。`build_bundle` 的 `prepare_symbols` 记录匿名类型提示和共享符号索引的准备耗时；`build_document` 分别记录 `source_pages`（源码还原）、`declarations`（声明）、`form_properties`（窗口属性）、`constants`（常量）；`unpack` 分别记录 `generate_bundle`、`write_bundle`、`workspace_files`、`dependency_artifacts`。不同 `operation` 存在包含关系，不能跨层直接相加；依赖模块会单独记录自己的阶段。

同一工程的源码、声明、窗口 XML、公开接口和原生快照生成共享符号解析器及支持库元数据；类型名称按类型 ID 缓存，命令和类型重名检查使用哈希集合。缓存随当前工程生成结束释放，各依赖模块独立建立，不写入磁盘，也不跨命令保留。

### 指定与探测易语言安装目录

`--e-dir` 是通用选项，支持解包、回包、预检、更新依赖、直接传入文件及默认回包，也支持 `--e-dir=<目录>`：

```powershell
e-packager unpack MyApp.e MyApp --e-dir "D:\易语言"
e-packager pack MyApp MyApp.e --e-dir "D:\易语言"
e-packager validate MyApp --e-dir "D:\易语言"
e-packager update MyApp --add-elib spec --e-dir "D:\易语言"
e-packager MyApp.e --e-dir "D:\易语言"
```

有效安装目录必须同时包含 `lib/`、`static_lib/`、`ecom/` 三个子目录。显式指定无效目录会报错并列出缺失目录；普通支持库搜索被限定为该目录的 `lib/`，不会使用注册表或工程旧路径中的同名库。此配置也会传递给 x86 辅助进程。直接编译中已有的目标架构专用搜索规则仍优先，避免跨位数加载。

未显式指定时，依次检查 `E_PACKAGER_EIDE`、注册表安装路径、`e-packager.exe` 所在目录及其直接上级目录；无效候选会记录缺失目录并继续探测。因此将程序放在易语言安装目录的 `tools/` 等一级子目录中，也可以在注册表失效时找到安装。普通模式仍允许工程自带支持库；查找元数据时，所有允许目录中的 `.fne`、`.fne.dll` 等开发库候选均排在 `.fnr` 运行库之前，避免工程旁不含命令表的运行库遮蔽安装目录中的开发库。同类候选保留原有目录顺序；最终采用哪个文件以 `support_library_loaded` 日志为准。

<details>
<summary><b>手动指定编译工具链</b></summary>

编译器通常能自动探测 Visual Studio，但你也可以显式指定：

```powershell
e-packager compile MyApp.e MyApp.exe `
  --vc-tools-dir "C:\Program Files\Microsoft Visual Studio\2022\Professional\VC\Tools\MSVC\<版本>" `
  --windows-sdk-dir "C:\Program Files (x86)\Windows Kits\10" `
  --e-dir "C:\path\to\易语言安装目录"
```

- `--vc-tools-dir`：MSVC 版本根目录，提供编译器、链接器、头文件和运行库
- `--windows-sdk-dir`：Windows Kits 根目录，提供 SDK 头文件、库和 rc.exe
- `--e-dir`：易语言安装目录，x86 从 `lib`、`static_lib` 查找第三方库；x64 不加载第三方库
- `--compiler`、`--linker`：显式指定 `cl.exe`、`link.exe`

> ⚠️ 不要使用 `e5.6\linker` 下的打包链接器 — 它缺少完整的 MSVC 头文件和 SDK。

</details>

<details>
<summary><b>legacy-blackmoon 编译模式（传统 x86 黑月）</b></summary>

该模式保留 IDE 编译 E-code 的传统行为，仅支持 x86：

```powershell
e-packager compile MyApp.e MyApp.exe `
  --compile-mode legacy-blackmoon `
  --legacy-blackmoon-mode asm `
  --eide "C:\path\to\IDE.exe" `
  --legacy-blackmoon-dir "C:\path\to\BlackMoon"
```

**要求**：
- 易语言 IDE + 已启用的 `AutoLinker.fne`
- 完整的 BlackMoon 工具链（`BlackMoon\bin\LINK.EXE`、入口对象等）
- 传统核心归档（`legacy_static_lib\x86\krnln.lib`，来自 BlackMoonModernCore Release）

**使用场景**：
- 需要与旧编译产物保持逐字节一致
- 依赖传统黑月工具链的特定功能

> 💡 大部分场景推荐使用默认的 `semantic` 模式。

</details>

<details>
<summary><b>CI 友好用法（无交互提问）</b></summary>

在 CI 环境中，提前准备 adapter 并显式传入，避免自动下载提问：

```bash
# 提前解压 adapter
Expand-Archive "BlackMoonKernelStaticLib-v<版本>-x86.zip" "D:\deps\BMC" -Force

# 编译时显式指定
e-packager compile MyApp.e MyApp.exe \
  --arch x86 \
  --blackmoon-x86-dir "D:\deps\BMC\adapter"
```

</details>

<details>
<summary><b>完整编译选项参考</b></summary>

| 选项 | 作用 |
|------|------|
| `--compile-mode semantic\|legacy-blackmoon` | 编译模式 |
| `--arch host\|x86\|x64` | 目标架构 |
| `--codegen-opt speed\|size` | C++ 速度优先（默认 /O2）或尺寸优先（/O1）；与语义优化独立 |
| `--semantic-opt baseline\|reachable\|typed` | 方法可达性及标量生成；默认 typed，见[优化说明](docs/semantic-optimization.md) |
| `--optimization-report <json>` | 输出方法保留/降级原因、编译耗时及未压缩 PE 大小 |
| `--subsystem auto\|console\|windows` | 子系统类型 |
| `--dll` | 编译为 DLL |
| `--define <宏>` / `-D <宏>` | 条件编译宏 |
| `--diagnostics text\|json` | 诊断输出格式 |
| `--compiler <路径>` | 显式指定 cl.exe |
| `--linker <路径>` | 显式指定 link.exe |
| `--vc-tools-dir <路径>` | MSVC 版本根目录 |
| `--windows-sdk-dir <路径>` | Windows Kits 根目录 |
| `--e-dir <路径>` | 易语言安装目录 |
| `--blackmoon-x86-dir <路径>` | x86 核心库 adapter 目录 |
| `--blackmoon-x64-dir <路径>` | x64 核心库 adapter 目录 |
| `--blackmoon-timeout <秒>` | 编译链接超时（默认 120） |

**legacy-blackmoon 专用选项**：
- `--legacy-blackmoon-mode asm\|cpp\|mfc` — 入口模式
- `--eide <路径>` — 易语言 IDE 路径
- `--legacy-blackmoon-dir <路径>` — 传统黑月工具链目录
- `--legacy-blackmoon-linker <路径>` — 覆盖传统黑月 linker

</details>

---

## 📚 更多资源

- **完整上手教程**：[`docs/compile-from-source-guide.md`](docs/compile-from-source-guide.md) — 从零开始的编译指南
- **实现原理**：[`docs/independent-compiler-architecture.md`](docs/independent-compiler-architecture.md) — 编译器架构与已验证范围
- **从零上手**：[`docs/getting-started.md`](docs/getting-started.md) — 基础使用教程
- **核心库实现**：[BlackMoonModernCore](https://github.com/aiqinxuancai/BlackMoonModernCore)
- **无头编译支持库**：[AutoLinker](https://github.com/aiqinxuancai/AutoLinker)

---

## ⚠️ 注意事项

**使用前请备份源文件**，作者不对可能的数据损失负任何责任。遇到无法解包或回包的文件，欢迎提交 Issue 并附上文件。

---

## 🙏 致谢

- [OpenEpl/TextECode](https://github.com/OpenEpl/TextECode) — 易语言工程文件与文本代码互转
- [OpenEpl/EProjectFile](https://github.com/OpenEpl/EProjectFile) — 易语言项目文件读写库
