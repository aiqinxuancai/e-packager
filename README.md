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

> ⚠️ **试验性功能** — 已覆盖常用控件和核心库命令，但不是 IDE 的完整替代。交付前请用 `compile-check` 或 IDE 复核。

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

在编辑后快速验证源码，无需生成 `.e` 文件：

```bash
# 文本格式
e-packager validate MyApp\

# JSON 格式（供脚本/IDE 解析）
e-packager validate MyApp\ --diagnostics json
```

**预检覆盖范围**：
- ✅ 声明槽位、属性、数组维数
- ✅ 声明顺序、重复名称、类型冲突
- ✅ 智能引号/括号配对、赋值左右值
- ✅ 窗体 XML、控件成员、事件处理器
- ✅ 符号、成员、调用签名、类型错误

`pack` 和无参回包会自动执行预检。发现错误时不会写出目标文件。

> 💡 预检不是完整的易语言编译器。交付前请用 `compile-check` 或 IDE 复核。

---

### 4. 权威编译检查

使用真实易语言 IDE 和 AutoLinker 执行编译检查：

```bash
# 对已有 .e 文件检查
e-packager compile-check MyApp.e

# 回包时同时检查
e-packager pack MyApp\ MyApp.e --compile-check

# 显式指定 IDE 和 AutoLinker 路径
e-packager compile-check MyApp.e \
  --eide "C:\path\to\IDE.exe" \
  --autolinker-test "D:\AutoLinker\bin\fne_release\AutoLinkerTest.exe"
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
| `AGENTS.md` | 供 AI Agent 阅读的项目结构说明 |

---

## 🔧 其他命令

### 刷新派生内容

解包后，若依赖的易模块或支持库发生变化，或需要新增资源，可用 `update` 刷新：

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
```

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
