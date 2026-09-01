# e-packager

将易语言 `.e` / `.ec` 文件解包为可读目录，或将目录回包为 `.e`，让易语言项目享有 Git 版本管理、代码 Diff、AI 辅助编辑等现代开发体验。

> 📖 参考应用：[易语言 × AI Agent 实践白皮书](https://github.com/aiqinxuancai/Awesome-E-Agent)
> 
> 📖 [易语言AutoLinker支持库，提供**无头编译**，用于AI使用本项目编辑代码后的验证编译](https://github.com/aiqinxuancai/AutoLinker)
>
> 📊 使用 AI 编辑易语言源码前，可参考[易语言大模型基准评分](https://e-language-bench.apptest.dev)选择模型。

## 使用

### 解包

```
e-packager unpack <input.e|input.ec> <output-dir>
```

只需要解包主 `.e` / `.ec` 文件，不刷新依赖易模块工作区和支持库公开接口时，可传入 `--main-only`：

```
e-packager unpack MyApp.e MyApp\ --main-only
```

如果源文件设置了打开密码，解包时传入 `--password`：

```
e-packager unpack MyApp.e MyApp\ --password 111222333
```

解包加密文件后，后续回包默认输出为未加密 `.e`，不需要再次提供密码；如果希望回包结果继续带打开密码，可在 `pack` 时传入 `--password`。

也可直接将 `.e` / `.ec` 文件拖放到 `e-packager.exe` 上，自动在源文件所在目录创建同名子目录并解包：

```
e-packager MyApp.e    # 解包到 MyApp\
e-packager MyMod.ec   # 解包到 MyMod\
```

### 支持库公开接口导出

Win32 版可直接读取 x86 `.fne` 支持库，并导出与解包 `.e` 时 `elib/*.txt` 一致的公开接口文本：

```
e-packager decrypt-fne MyLib.fne MyLib.txt
```

也可直接将 `.fne` 文件拖放到 `e-packager.exe` 上，默认在同目录生成同名 `.txt`。x64 版不会加载 x86 支持库，此命令会提示需要 Win32 版。

**解包目录结构：**

| 路径 | 内容 |
| --- | --- |
| `src/` | 源码文件（`.txt`）及窗口界面定义（`.xml`） |
| `project/` | 封包所需元数据；`.e` 解包后还可能含原生快照，**请勿删除** |
| `header/` | 仅 `.ec` 项目生成；公开接口头文件，不参与回包 |
| `ecom/` | 仅 `.e` 项目生成；每个子目录为一个已解包的模块工作区，不参与回包 |
| `elib/` | 依赖支持库的公开接口导出，仅供查阅，不参与回包 |
| `image/` | 图片资源及任意二进制资源，元数据在 `image/list.json` |
| `audio/` | 音频资源及任意二进制资源，元数据在 `audio/list.json` |
| `tool/e-packager.exe` | 随目录自带的封包工具 |
| `info.json` | 来源文件的类型、路径、修改时间、MD5 |
| `AGENTS.md` | 供 AI Agent 阅读的项目结构说明 |

若 `.e` 工程引用了易模块（`.ec`），默认解包时会自动将这些模块同步导出到 `ecom/<模块名>/`，并导出支持库公开接口到 `elib/`。`project/.module.json` 中对应依赖项会额外写入 `resolvedPath`（本机模块完整路径）与 `localWorkspace`（本地工作区目录）两个只读辅助字段，不参与回包。使用 `--main-only` 时不会生成、更新或删除 `ecom/` 与 `elib/`，也不会写入这些派生辅助字段。

### 回包

```
e-packager pack <input-dir> <output.e|output.ec> [--password <text>]
```

或在项目根目录（或 `tool/` 子目录）内直接运行，自动输出到 `pack/` 目录：

```
e-packager
```

> `.ec` 工作区回包的实际输出始终为 `.e` 格式；无参默认回包时输出至 `pack/<原文件名>.ec.e`。

需要为回包结果设置打开密码时：

```
e-packager pack MyApp\ MyApp-protected.e --password 111222333
```

### 回包前预检

可以只读取工作区并执行快速预检，不生成 `.e` 文件：

```
e-packager validate <input-dir>
```

`pack` 和无参默认回包会自动执行同一套预检。发现确定性错误时，命令返回失败并且不会写出目标文件；诊断包含源码文件、行号和错误代码。

当前预检覆盖声明槽位及属性、严格数组维数、自定义数据类型数组成员的零维限制、声明顺序与同作用域重复名称、跨页面类型/资源名称冲突、未知点指令、流程块配对、智能引号和括号配对、缺少赋值左值/右值、误用半角赋值符号、只读目标赋值、窗体 XML 及窗口程序集绑定、控件成员和事件处理器存在性，以及可静态确定的符号、成员、调用签名、返回值和表达式类型错误。例如 `整数变量 ＝ “文本”`、`a ＝`、`#常量 ＝ 1`、`.局部变量 a 整数型` 会被阻止。

声明的行尾空槽可以省略，预检不会强制补齐固定字段数；填写后续属性、数组维数或说明时，中间空槽仍须用逗号保留。例如 `.局部变量 arr, 整数型, , "0"` 合法，不能压缩为 `.局部变量 arr, 整数型, "0"`。`"0"` 可用于动态局部变量或全局变量，但自定义数据类型的数组成员不能包含零维。

预检不是完整的易语言编译器。目前仍未完整覆盖类继承及访问权限、普通程序集方法与类实例方法的调用边界、支持库或易模块的重载和特殊命令规则、变体型及泛型类型流、窗口事件参数签名、全部隐式类型转换和特殊表达式。DLL 入口是否存在以及数组越界、空对象、文件或动态资源缺失等运行期问题也不属于源码预检。依赖元数据缺失或语义无法可靠确定时会标记为未知而不是臆测报错。因此 `errors=0` 表示没有发现当前覆盖范围内的确定性错误，不代表最终编译一定成功。

对已修改或新增的可执行语句，回包器还会要求其能够生成原生语义表达式；编码失败会中止回包，不再静默写入未检查的原始代码。未修改语句继续复用原生快照，注释、空行和屏蔽代码仍可保留。需要 IDE 级别结论时，可使用下面的 `--compile-check`。

### 权威无头编译检查

如果本机安装了易语言和 AutoLinker，可以让封包在提交 `.e` 前执行一次真实编译：

```powershell
tool\e-packager.exe pack . .\pack\checked.e `
  --compile-check `
  --eide "C:\path\to\e.exe" `
  --autolinker-test "D:\git\AutoLinker\bin\fne_release\AutoLinkerTest.exe" `
  --compile-static --compile-timeout 120
```

也可以只对已有 `.e` 文件检查，不生成新的 `.e`：

```powershell
e-packager compile-check checked.e `
  --eide "C:\path\to\e.exe" `
  --autolinker-test "D:\git\AutoLinker\bin\fne_release\AutoLinkerTest.exe"
```

`--eide` 可以省略，封包器会从 `E.Document` 注册表打开命令中查找易语言主程序；`--autolinker-test` 可以省略，封包器会依次尝试环境变量 `E_PACKAGER_AUTOLINKER_TEST`、程序同目录和 `PATH`。也可以用 `E_PACKAGER_EIDE` 提供 IDE 路径。编译失败会输出 AutoLinker 的 IDE 页面、行号和输出窗口内容，并且不会覆盖已有目标文件。`--compile-static` 用于同时验证静态链接；不指定时执行 IDE 的普通编译检查。

这一步依赖当前机器的易语言版本、支持库、易模块和链接器配置，失败时应先修复环境或源码，再交付 `.e`。

### 源码直接编译（试验性）

> ⚠️ **试验性功能。** 编译结果尚未经过大规模验证，行为和选项可能随版本变化。窗口工程、窗口事件以及少见的支持库暂不保证可用。交付前请继续用 `compile-check` 或易语言 IDE 确认，不要直接把产物用于生产环境。

`compile` 把易语言源码直接编译成独立的 EXE 或 DLL，不需要打开易语言 IDE：

```text
e-packager compile <input.e|input-dir> <output.exe|output.dll> [选项]
```

输入可以是 `.e` 文件，也可以是 `unpack` 出的工程目录；输出扩展名写 `.dll` 时自动按 DLL 编译，工程里的公开子程序会成为 DLL 导出函数。

**两种编译方式：**

| `--compile-mode` | 架构 | 需要什么 |
| --- | --- | --- |
| `semantic`（默认，推荐） | x86 / x64 | 一套 Visual C++ 工具链；调用核心库命令时还需同架构的 BlackMoonKernelStaticLib adapter |
| `legacy-blackmoon` | 仅 x86 | 已安装的易语言、AutoLinker、黑月工具链，以及匹配的传统核心归档 |

`blackmoon` 是兼容旧脚本的别名：x86 下等同于 `legacy-blackmoon`，x64 下等同于 `semantic`。没有特殊需求时用默认的 `semantic` 即可；只有依赖传统黑月工具链、或需要和旧编译产物保持完全一致时才需要 `legacy-blackmoon`。

#### semantic：默认方式

只需给出输入和输出：

```powershell
e-packager.exe compile MyApp.e .\out\MyApp.exe
e-packager.exe compile .\MyLib .\out\MyLib.dll
```

编译器会自动探测本机的 Visual C++ 与 Windows SDK，并在输出目录生成 `<输出名>.generated.cpp` 等中间文件，方便排查问题，可随时删除。探测失败，或想固定用某个版本时，用 `--compiler`、`--linker`、`--lib` 显式指定：

```powershell
e-packager.exe compile MyApp.e .\out\MyApp.exe `
  --compiler "C:\path\to\VC2022\bin\Hostx64\x86\cl.exe" `
  --linker "C:\path\to\VC2022Linker\bin\link.exe" `
  --lib "C:\path\to\VC2022Linker\lib"
```

x86、x64 都可以用 `--arch x86` / `--arch x64` 指定，默认跟随当前程序架构。

#### 需要核心库命令时：下载 adapter

工程如果用到了易语言核心库（黑月核心）里的命令，需要额外下载一份匹配架构的 [BlackMoonKernelStaticLib](https://github.com/aiqinxuancai/BlackMoonKernelStaticLib) adapter。前往 [Releases](https://github.com/aiqinxuancai/BlackMoonKernelStaticLib/releases) 下载对应架构的资产（不要用 GitHub 自动生成的 Source code 压缩包）；带 `beta`/`pre`/`rc` 标记的是预发布版，生产环境请选稳定版：

| 发布资产 | 用途 |
| --- | --- |
| `BlackMoonKernelStaticLib-v<版本>-x64.zip` | x64 编译，通常只需下载此文件 |
| `BlackMoonKernelStaticLib-v<版本>-x86.zip` | x86 编译 |
| `BlackMoonKernelStaticLib-v<版本>.zip` | 同时包含 x86 与 x64 |
| `BlackMoonKernelStaticLib-v<版本>-vc6.zip` | 仅传统 x86 黑月链路的 VC6 `krnln.lib` |

解压后得到 `adapter` 目录，直接传给编译器即可：

```powershell
Expand-Archive "C:\Users\<用户名>\Downloads\BlackMoonKernelStaticLib-v<版本>-x64.zip" "D:\deps\BlackMoonKernelStaticLib" -Force

e-packager.exe compile MyApp.e .\out\MyApp-x86.exe `
  --arch x86 --blackmoon-x86-dir "D:\deps\BlackMoonKernelStaticLib\adapter"

e-packager.exe compile MyApp.e .\out\MyApp-x64.exe `
  --arch x64 --blackmoon-x64-dir "D:\deps\BlackMoonKernelStaticLib\adapter"
```

`--blackmoon-x86-dir` / `--blackmoon-x64-dir` 均可重复传入，用于补充同架构的其他支持库；但核心 adapter 里的 FNE、主归档、后备归档和清单文件必须来自**同一个** Release，不要混用不同版本。`.e` 输入会自动调用 Win32 版 `e-packager` 做权威解码，一般无需干预。

`-vc6.zip` 不是 semantic adapter：它只有由 Visual C++ 6.0 生成的传统 x86 `krnln.lib`，没有 `krnln.fne`、`krnln_adapter.json` 或现代包装层。需要使用它时，将压缩包中的 `krnln.lib` 放入传统黑月工具链的库搜索目录（或通过 `--lib` 指定），并按下面的 `legacy-blackmoon` 方式提供完整的 `BlackMoon\bin`、入口对象和 VC6/MFC 运行库；不能把该目录作为 `--blackmoon-x86-dir` 来替代现代 x86 adapter，也不能用于 x64。

#### legacy-blackmoon（传统 x86 黑月）

该路线保留 IDE 编译 E-code -> `BlackMoon.obj` -> 传统黑月入口对象的行为。
使用 `--compile-mode legacy-blackmoon`（旧的 `--blackmoon`/`blackmoon` 兼容别名仍可用）。`--legacy-blackmoon-mode` 选择入口模式，可取 `asm`、`cpp` 或 `mfc`：

```powershell
e-packager.exe compile MyApp.e .\out\MyApp.exe `
  --compile-mode legacy-blackmoon `
  --legacy-blackmoon-mode asm `
  --eide "C:\path\to\e.exe" `
  --autolinker-test "C:\path\to\AutoLinkerTest.exe" `
  --legacy-blackmoon-dir "C:\path\to\BlackMoon"
```

若所用的静态库带有 MFC 依赖，`asm`、`cpp` 会在链接失败后自动改用 MFC 入口重试，成功消息中的 `effective_mode` 表示实际生效的入口。不需要 MFC 的工程不受影响。

`BlackMoonKernelStaticLib` 的 [Release](https://github.com/aiqinxuancai/BlackMoonKernelStaticLib/releases) 同时提供现代语义 adapter 与传统归档。传统黑月归档位于 `legacy_static_lib\x86\krnln.lib`，不能覆盖到现代语义 adapter，也不能与现代 adapter 混用。传统路线仍需完整的 `BlackMoon\bin\LINK.EXE`、`BlackMoonExe.obj`、`EyInit.obj` 等文件。

不要把不同版本的核心 `.fne` 和 `krnln.lib` 混用；x86 黑月转换会读取易语言安装目录下的 `lib\krnln.fne`。除非已验证与当前 IDE/黑月工具链匹配，不要用发布包内的 x86 FNE 直接覆盖 IDE 的核心 FNE。

#### 常用选项

| 选项 | 作用 |
| --- | --- |
| `--compile-mode semantic\|legacy-blackmoon\|blackmoon` | 选择源码语义或传统黑月路线；默认 `semantic`，`blackmoon` 为兼容别名 |
| `--arch host\|x86\|x64` | 选择输出架构，默认跟随当前程序；semantic 两种架构均可用 |
| `--legacy-blackmoon-mode asm\|cpp\|mfc` | 选择传统黑月入口模式（仅 x86） |
| `--dll` | 按 DLL 编译；输出扩展名为 `.dll` 时可省略 |
| `--define <宏>` / `-D <宏>` | 添加条件编译宏，可重复传入 |
| `--compiler <cl.exe>` / `--linker <link.exe>` / `--lib <目录>` | 指定 C++ 编译器、链接器和库目录；未指定编译器时按目标架构自动探测 |
| `--eide <e.exe>` / `--autolinker-test <exe>` | 易语言 IDE 与 AutoLinker 启动器（x86 黑月） |
| `--legacy-blackmoon-dir <目录>` | 传统黑月工具链根目录（`--blackmoon-dir` 为兼容别名） |
| `--blackmoon-core-dir <目录>` | semantic 核心 adapter 根目录，可重复传入 |
| `--blackmoon-x86-dir <目录>` / `--blackmoon-x64-dir <目录>` | semantic 指定架构核心 adapter 根目录 |
| `--blackmoon-timeout <秒>` | 黑月编译与链接超时，默认 120，范围 1 至 3600 |

如果支持库或静态库中缺少某个命令的实现，请更换匹配版本的支持库，而不是修改源码绕开。实现原理与已验证范围详见 [`docs/independent-compiler-architecture.md`](docs/independent-compiler-architecture.md)。

### 刷新派生内容

解包后，若依赖的易模块或支持库发生变化，或需要新增图片、音频等二进制资源，可用 `update` 命令刷新 `ecom/` 与 `elib/` 中的派生内容并写入资源索引，无需重新解包整个工程。

```
e-packager update <input-dir>
```

**示例：**

```
# 刷新工作区的所有 ecom/elib 派生内容
e-packager update MyApp\

# 新增一个 .ec 模块到 ecom/，并刷新其导出内容
e-packager update MyApp\ --add-ecom D:\modules\MyLib.ec

# 新增多个 .ec 模块
e-packager update MyApp\ --add-ecom D:\modules\Net.ec --add-ecom D:\modules\UI.ec

# 新增支持库（按文件名 stem，自动在 lib/ 目录中查找 .fne/.fnr/.dll）
# 仅 Win32 版 e-packager 支持此选项
e-packager update MyApp\ --add-elib 互联网支持库

# 也可直接传入 .fne 文件的完整路径
e-packager update MyApp\ --add-elib D:\易语言\lib\互联网支持库.fne

# 同时新增模块与支持库
e-packager update MyApp\ --add-ecom D:\modules\Net.ec --add-elib 互联网支持库

# 新增图片资源，默认使用文件名 stem 作为常量名，代码中写 #logo
e-packager update MyApp\ --add-image D:\res\logo.png

# 新增音频资源，默认使用文件名 stem 作为常量名，代码中写 #notify
e-packager update MyApp\ --add-audio D:\res\notify.wav

# 显式指定资源常量名，代码中写 #启动画面
e-packager update MyApp\ --add-image 启动画面=D:\res\splash.bin
```

`--add-image` 与 `--add-audio` 都写入易语言常量资源表，使用方式与普通常量一致，代码中以 `#资源名` 引用。目录名只是决定资源写入 `image/list.json` 还是 `audio/list.json`，实际内容可以是程序需要携带的任意二进制数据。

### 其他命令

```
# 从 GitHub Release 下载最新 e-packager 并替换当前 exe
e-packager /update

# 查看当前程序版本
e-packager version

# 导出单个 .fne 支持库公开接口（仅 Win32 版）
e-packager decrypt-fne <input.fne> [output.txt]

# 快速检查目录工程，不生成 .e
e-packager validate <input-dir>

# 比较原文件与目录内容是否一致
e-packager compare-bundle <input.e|input.ec> <input-dir> [--password <text>]

# 解包后立即回包（快速验证）
e-packager roundtrip <input.e|input.ec> <work-dir> <output.e|output.ec> [--password <text>]

# 往返并校验字节一致性
e-packager verify-roundtrip <input.e|input.ec> <work-dir> <output.e|output.ec> [--password <text>]
```

`/update` 会启动后台替换脚本：先查询 `aiqinxuancai/e-packager` 的最新 GitHub Release，下载匹配的 Win32 二进制压缩包，等待当前进程退出后替换当前 `e-packager.exe`。当前 GitHub Actions 不上传 x64 包，因此 x64 构建会跳过自更新。若需要在本地开发版或同版本上强制覆盖，可使用 `e-packager /update --force`。

## 注意

**使用前请备份源文件**，作者不对可能的数据损失负任何责任。遇到无法解包或回包的文件，欢迎提交 Issue 并附上文件。

## 致谢

- [OpenEpl/TextECode](https://github.com/OpenEpl/TextECode) — 易语言工程文件与文本代码互转
- [OpenEpl/EProjectFile](https://github.com/OpenEpl/EProjectFile) — 易语言项目文件读写库
