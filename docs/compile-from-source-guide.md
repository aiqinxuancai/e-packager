# 直接编译易语言源码成 EXE/DLL

无需打开 IDE，一条命令把 `.e` 编译成可执行文件。适合 CI 自动构建、AI/脚本编辑后的快速验证。

```bash
e-packager compile <input.e|input-dir> <output.exe|output.dll>
```

> ⚠️ **试验性功能** — 已覆盖常用控件和核心库命令，但不是 IDE 的完整替代。交付前请用 `compile-check` 或 IDE 复核。

---

## 🚀 快速开始

最简单的三步：下载依赖 → 编译 → 运行

```bash
# 1. 第一次编译会自动提示下载核心库，按 Y 确认
e-packager compile MyApp.e out\MyApp.exe

# 2. 运行
.\out\MyApp.exe

# 3. 编译成 x64 版本
e-packager compile MyApp.e out\MyApp-x64.exe --arch x64
```

**常用命令速查**

```bash
# 编译 DLL
e-packager compile MyLib.e out\MyLib.dll

# 从拆包目录编译（需显式指定依赖）
e-packager unpack MyApp.e MyApp-src\
e-packager compile MyApp-src\ out\MyApp.exe --blackmoon-x86-dir <路径>

# 编译前验证源码
e-packager validate MyApp-src\ --diagnostics json

# 指定 GUI 子系统（不弹控制台窗口）
e-packager compile MyApp.e out\MyApp-gui.exe --subsystem windows
```

---

## 📋 环境要求

### 必需组件

#### 1. Visual Studio C++ 工具链（编译器会自动探测）

**下载安装**：
- 下载地址：https://visualstudio.microsoft.com/zh-hans/downloads/
- 选择 **Visual Studio Community**（免费）或 Professional/Enterprise

**安装步骤**：
1. 运行安装器后，在 **工作负载** 标签页中勾选：
   - ✅ **使用 C++ 的桌面开发**（Desktop development with C++）
2. 右侧 **安装详细信息** 中确认已自动选中（通常默认勾选）：
   - ✅ MSVC v14x - VS 20xx C++ x64/x86 生成工具
   - ✅ Windows 10/11 SDK（任意版本）
   - ✅ 适用于 Windows 的 C++ CMake 工具（或 C++ 生成工具）
3. 点击安装，等待完成（约 5-10 GB）

**验证安装**：
- 打开 **Developer PowerShell for VS 20xx**
- 运行 `cl` 能看到 Microsoft C/C++ 编译器版本信息

> 💡 只需勾选"使用 C++ 的桌面开发"即可，安装器会自动包含 MSVC、Windows SDK、RC 等所有必需组件。

#### 2. 核心库实现（首次编译时自动下载）

- 来源：[BlackMoonModernCore Release](https://github.com/aiqinxuancai/BlackMoonModernCore/releases)
- 自动缓存到：`%LOCALAPPDATA%\e-packager\dependencies`

#### 3. 第三方支持库（如需）

- x86 semantic 可提供匹配架构的 `.fne` 和静态库（`.lib`）；x64 semantic 暂不支持第三方易语言库

### 手动指定工具链（可选）

编译器通常能自动探测，但你也可以显式指定：

```powershell
e-packager compile MyApp.e out\MyApp.exe `
  --vc-tools-dir   "C:\path\to\VC\Tools\MSVC\<版本>" `
  --windows-sdk-dir "C:\path\to\Windows Kits\10" `
  --e-dir    "C:\path\to\易语言安装目录"
```

> ⚠️ 不要使用 `e5.6\linker` 下的打包链接器 — 它缺少完整的 MSVC 头文件和 SDK。

---

## 🔨 完整示例

### 示例 1：编译完整测试工程

使用仓库中的测试工程（21 个文件，1606 行代码）：

```bash
# 首次编译会提示下载核心库
$ e-packager compile eproj/e-console-exe-full-test.e temp/full-test-x86.exe

缺少直接编译依赖：krnln (x86)
是否自动下载并重试？ [Y/n] Y

# 下载完成后自动编译
compile: compiled:temp\full-test-x86.exe
  compile_mode=semantic; arch=x86; methods=53; commands=137; libraries=1

# 运行程序
$ ./temp/full-test-x86.exe
======== 核心支持库全面测试开始 ========
运行目录=D:\git\e-packager\temp\
...
exit=0
```

**编译结果解读**

| 字段 | 含义 |
|------|------|
| `compile_mode=semantic` | 使用语义编译路线（推荐） |
| `arch=x86` | 目标架构 |
| `methods=53` | 实际生成的可达子程序数 |
| `commands=137` | 实际绑定的支持库命令数 |
| `libraries=1` | 链接的支持库数量 |

### 示例 2：编译 x64 版本

```bash
$ e-packager compile eproj/e-console-exe-full-test.e temp/full-test-x64.exe --arch x64

# 首次同样会提示下载 x64 核心库
compile: compiled:temp\full-test-x64.exe; arch=x64; methods=53; commands=137
```

### 示例 3：编译 DLL

```bash
$ e-packager compile eproj/e-win32-dll-new-proj.e temp/demo-x86.dll

compile: compiled:temp\demo-x86.dll; methods=3; commands=2; libraries=1

# 查看导出函数
$ dumpbin /exports temp/demo-x86.dll
    ordinal hint RVA      name
          1    0 00009A30 TestPub1
```

只有标记为 `公开` 的子程序会被导出。

### 示例 4：拆包 → 编辑 → 编译（AI/脚本工作流）

```bash
# 1. 拆包成可编辑目录
$ e-packager unpack MyApp.e MyApp-src\

# 2. 编辑源码文件（AI 或脚本修改 src/*.txt）
# ...

# 3. 编译前验证
$ e-packager validate MyApp-src\ --diagnostics json

# 4. 编译（需显式指定依赖路径）
$ e-packager compile MyApp-src\ out\MyApp.exe \
    --blackmoon-x86-dir "%LOCALAPPDATA%\e-packager\dependencies\...\adapter"
```

> 💡 从拆包目录编译时，必须显式传入 adapter 路径，编译器不会自动下载。

### 示例 5：GUI 程序（无控制台窗口）

```bash
$ e-packager compile MyGuiApp.e temp/gui-app.exe --subsystem windows

# 检查 PE 头
$ dumpbin /headers temp/gui-app.exe | findstr subsystem
               2 subsystem (Windows GUI)
```

程序运行时不会弹出黑色控制台窗口。

---

## 📊 编译产物尺寸参考

实测数据（Windows 11 + MSVC 14.51）：

| 产物类型 | 架构 | 尺寸 |
|---------|------|------|
| 空白控制台工程 | x86 | **~114 KB** |
| 完整测试工程（1606 行，137 个命令） | x86 | **~675 KB** |
| 完整测试工程 | x64 | **~777 KB** |
| 纯代码 GUI（一个信息框） | x86 | **~173 KB** |
| DLL 工程 | x86 | **~184 KB** |

**关键点**：
- 空白工程 ~114 KB 是地板价（包含 CRT 静态链接 + 运行时桥接）
- 可达性分析只链入用到的命令，不会把整个核心库塞进去
- x64 比 x86 大约多 15%（64 位指针和对齐开销）

---

## ⚙️ 常用选项速查

| 选项 | 作用 | 默认值 |
|------|------|--------|
| `--arch host\|x86\|x64` | 目标架构 | `host` |
| `--subsystem auto\|console\|windows` | 子系统类型 | `auto` |
| `--dll` | 编译为 DLL（输出为 `.dll` 可省略） | - |
| `--diagnostics text\|json` | 诊断输出格式 | `text` |
| `--blackmoon-x86-dir <路径>` | x86 核心库 adapter 目录 | 自动探测 |
| `--blackmoon-x64-dir <路径>` | x64 核心库 adapter 目录 | 自动探测 |
| `--e-dir <路径>` | x86 从易语言安装目录的 `lib`、`static_lib` 查找第三方库；x64 不加载第三方库 | 自动探测 |
| `--vc-tools-dir <路径>` | MSVC 版本根目录；提供编译器、链接器、头文件和运行库 | 自动探测 |
| `--windows-sdk-dir <路径>` | Windows Kits 根目录；提供 SDK 头文件、库和 rc.exe | 自动探测 |
| `--compiler <路径>` | 显式指定 cl.exe | 自动探测 |
| `--linker <路径>` | 显式指定 semantic 使用的 link.exe | 自动探测 |

**CI 友好用法（无交互提问）**：

```bash
# 提前准备 adapter，显式传入
e-packager compile MyApp.e out\MyApp.exe \
  --arch x86 \
  --blackmoon-x86-dir "D:\deps\BMC\adapter"
```

---

## 🐛 编译失败时怎么排查

### 错误定位

编译失败会显示文件名、行号和错误码：

```bash
$ e-packager compile MyApp-src\ out\MyApp.exe

compile failed: compiler_model_failed:src/测试.txt:9:
  expression_parse_failed:expression_operand_missing
```

### 使用 validate 预检

在编译前运行 `validate` 可以更快发现问题：

```bash
# 文本格式
$ e-packager validate MyApp-src\

# JSON 格式（供脚本/IDE 解析）
$ e-packager validate MyApp-src\ --diagnostics json
{
  "diagnostics": [{
    "code": "delimiter_unclosed",
    "file": "src/测试.txt",
    "line": 9,
    "message": "expression delimiter is not closed",
    "severity": "error",
    "sourceLine": "目录 ＝ 取运行目录 ("
  }],
  "errors": 1,
  "ok": false
}
```

JSON 输出包含：`phase`、`code`、`file`、`line`、`column`、`message`、`sourceLine`。

---

## 🔀 编译模式选择

| 模式 | 架构 | 依赖 | 推荐场景 |
|------|------|------|----------|
| `semantic`（默认） | x86 / x64 | VS C++ 工具链 + BlackMoonModernCore adapter | **推荐** — 现代工具链，支持 x64 |
| `legacy-blackmoon` | 仅 x86 | 易语言 IDE + AutoLinker + 传统黑月工具链 | 仅当需要与旧编译产物保持逐字节一致 |

本文档全部示例使用 `semantic` 模式。除非有特殊需求，无需更改。

**legacy-blackmoon 用法**（不推荐）：

```powershell
e-packager compile MyApp.e temp\legacy.exe `
  --compile-mode legacy-blackmoon `
  --legacy-blackmoon-mode asm `
  --eide "C:\path\to\易语言IDE.exe" `
  --legacy-blackmoon-dir "C:\path\to\BlackMoon"
```

---

## ⚡ 一页速查

```bash
# 最简单 — 自动探测工具链、自动下载依赖
e-packager compile MyApp.e out\MyApp.exe

# x64 版本
e-packager compile MyApp.e out\MyApp-x64.exe --arch x64

# 编译 DLL
e-packager compile MyLib.e out\MyLib.dll

# CI 流程（显式依赖，无交互）
e-packager compile MyApp.e out\MyApp.exe \
  --arch x86 \
  --blackmoon-x86-dir "D:\deps\BMC\adapter"

# 拆包 → 编辑 → 验证 → 编译
e-packager unpack MyApp.e MyApp-src\
# 编辑 MyApp-src\src\*.txt
e-packager validate MyApp-src\ --diagnostics json
e-packager compile MyApp-src\ out\MyApp.exe --blackmoon-x86-dir <路径>

# 交付前用真实 IDE 复核
e-packager compile-check out\checked.e
```

---

## 📌 当前边界与限制

已实现：
- ✅ 源码 → 语义模型 → C++ → 链接 → EXE/DLL 完整链路
- ✅ 常用核心控件、嵌套控件、常见属性和事件
- ✅ x86 / x64 双架构
- ✅ 独立 Win32 窗口宿主

尚未完全覆盖：
- ⚠️ 第三方自绘控件需要独立适配
- ⚠️ 复杂数组、COM/Variant、部分 ABI 细节仍在扩展
- ⚠️ 特殊编译指令、少见表达式仍在补全
- ⚠️ `置入代码` 目前支持字节集字面量 + x86 naked helper

**重要**：这不是易语言 IDE 的等价替代。交付前请用 `compile-check` 或 IDE 复核。

---

## 📚 更多资源

- **原理详解**：[`independent-compiler-architecture.md`](independent-compiler-architecture.md)
- **核心库实现**：[BlackMoonModernCore](https://github.com/aiqinxuancai/BlackMoonModernCore)
- **从零上手**：[`getting-started.md`](getting-started.md)
