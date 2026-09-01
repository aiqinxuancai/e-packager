# 不打开易语言 IDE，把 `.e` 直接编译成 EXE

> ⚠️ **`compile` 是试验性功能。** 编译结果尚未经过大规模验证，行为和选项可能随版本变化。窗口控件与窗口事件、少见的支持库暂不保证可用。交付前请继续用 `compile-check` 或易语言 IDE 复核，不要直接把产物用于生产环境。
>
> 本文所有命令、输出和文件尺寸都来自一次真实执行，环境为 Windows 11 + Visual Studio 18 Professional（MSVC 14.51.36231）+ e-packager `dev` 构建。你的数字会略有差异，但量级应当一致。

## 这件事解决什么问题

易语言的编译按钮藏在 IDE 里。这带来两个后果：CI 上没有 IDE，所以没法自动出包；AI 或脚本改完 `.txt` 源码后，也没有一条命令能直接确认"这份源码能不能变成 EXE"。

`e-packager compile` 把这条路补上：

```text
e-packager compile <input.e|input-dir> <output.exe|output.dll> [选项]
```

输入可以是原生 `.e`，也可以是 `e-packager unpack` 出来的工程目录。输出扩展名写 `.dll` 就按 DLL 编译。整个过程不启动 `e.exe`，由 e-packager 自己完成源码解析、语义建模、C++ 生成、`cl.exe` 编译和 `link.exe` 链接。

它和已有的 `compile-check` 是两件不同的事：`compile-check` 借 AutoLinker 调用真实 IDE 做权威确认，`compile` 则完全不依赖 IDE，自己生成产物。前者是"裁判"，后者是"另一条生产线"。

## 第一步：确认手上有什么

只需要两样东西：一份 e-packager，和一套 Visual C++ 工具链。

```bash
$ ./bin/Win32/Release/e-packager.exe version
e-packager dev
```

编译器会自己探测本机的 Visual C++ 与 Windows SDK，通常不需要手工指定路径。如果探测失败，或者你想锁定某个版本，用 `--compiler`、`--linker`、`--lib` 显式覆盖：

```powershell
e-packager.exe compile MyApp.e .\out\MyApp.exe `
  --compiler "C:\path\to\VC\bin\Hostx64\x86\cl.exe" `
  --linker   "C:\path\to\VC\bin\Hostx64\x86\link.exe" `
  --lib      "C:\path\to\VC\lib\x86"
```

第三样东西——核心库实现——不用提前准备，下一节会看到它自己出现。

## 第二步：第一次编译，以及那个提问

拿仓库里的完整测试工程 `eproj\e-console-exe-full-test.e` 开刀。这份工程有 21 个源码文件、1606 行代码，覆盖流程控制、文本、字节集、数组、时间、文件、数据库、拼音、变体型、类、DLL 声明和图片资源，还引用了一个易模块（`AiQinDateTime`）和两个支持库——它不是 hello world，是一份认真的回归样本。

直接编译：

```bash
$ ./bin/Win32/Release/e-packager.exe compile eproj/e-console-exe-full-test.e temp/full-test-x86.exe
e-packager dev
缺少直接编译依赖：krnln (x86)
是否自动下载并重试？ [Y/n]
```

第一次跑必然会撞上这个提问。原因很直白：易语言的核心库命令（`输出行`、`取文本长度`、`取拼音` 这些）需要一份能被现代链接器使用的静态实现，而它不在 e-packager 内部——那是 [BlackMoonModernCore](https://github.com/aiqinxuancai/BlackMoonModernCore) adapter 的职责。

按回车或输入 `Y`，e-packager 会从 BlackMoonModernCore 最新稳定版 Release 拉取匹配架构的 adapter，缓存到 `%LOCALAPPDATA%\e-packager\dependencies`，然后自动重试一次：

```text
compile: compiled:D:\git\e-packager\temp\full-test-x86.exe;compile_mode=semantic;arch=x86;
methods=53;commands=137;libraries=1;core_archive=blackmoon_kernel_adapter;
source=...\full-test-x86.generated.cpp;object=...\full-test-x86.obj
```

24 秒，其中约 12 秒是下载。缓存之后，同一份工程重编大约 12–15 秒。

这条成功消息值得读一遍，它不是装饰：

| 字段 | 含义 |
| --- | --- |
| `compile_mode=semantic` | 走的是源码语义路线（默认，推荐） |
| `arch=x86` | 目标架构 |
| `methods=53` | 从 `_启动子程序` 出发实际可达并生成的子程序数 |
| `commands=137` | 实际绑定的支持库命令数 |
| `libraries=1` | 实际链接的支持库数（此处只有核心库） |
| `core_archive=blackmoon_kernel_adapter` | 核心实现来自现代 adapter，而非传统归档 |

`methods`/`commands` 是可达性分析的结果，不是工程里声明的总数。数字对不上你的预期时，通常意味着某个子程序没有被真正调用到。

如果你不想被提问打断（比如在 CI 里），提前把 adapter 准备好并显式传入：

```powershell
Expand-Archive "BlackMoonKernelStaticLib-v<版本>-x86.zip" "D:\deps\BMC" -Force

e-packager.exe compile MyApp.e .\out\MyApp.exe `
  --arch x86 --blackmoon-x86-dir "D:\deps\BMC\adapter"
```

注意从 Release 下载真正的**资产文件**，不要用 GitHub 自动生成的 Source code 压缩包。带 `beta`/`pre`/`rc` 的是预发布版。压缩包名里的 `BlackMoonKernelStaticLib-` 前缀只是为兼容旧脚本保留的历史命名，它确实是 BlackMoonModernCore 的发布物，不需要另外安装什么。

## 第三步：跑一下

```bash
$ ./temp/full-test-x86.exe
exit=0
```

287 行输出，退出码 0。开头几行：

```text
======== 核心支持库全面测试开始 ========
运行目录=D:\git\e-packager\temp\
======== 开始流程与运算测试 ========
如果否则: 及格
判断条件式: 条件-二级
计次循环合计=15
变量循环倒序: 5,4,3,2,1,
```

结尾：

```text
======== 常量/DLL/数据结构/类/变体型测试结束 ========
======== 核心支持库全面测试结束 ========
```

> 控制台输出是 GBK（CP936）编码，这和易语言 IDE 编译的产物一致。重定向到文件后用 UTF-8 工具查看会看到乱码，需要按 CP936 解码。

## 第四步：换成 x64

同一份 `.e`，换 x64 版 e-packager 并加 `--arch x64`：

```bash
$ ./bin/x64/Release/e-packager.exe compile eproj/e-console-exe-full-test.e temp/full-test-x64.exe --arch x64
compile: compiled:...\full-test-x64.exe;compile_mode=semantic;arch=x64;
methods=53;commands=137;libraries=1;core_archive=blackmoon_kernel_adapter
```

`methods` 和 `commands` 与 x86 完全相同——同一套语义模型，只是换了目标架构的 FNE 元数据和静态库。首次同样会询问下载 x64 adapter。

两个产物的运行结果对比很能说明问题。287 行输出中有 38 行不同，全部是时间戳、进程 ID、`GetTickCount` 和自身文件名：

```diff
< 取现行时间=2026年9月1日13时34分27秒
> 取现行时间=2026年9月1日13时35分32秒
< 取执行文件名=full-test-x86.exe
> 取执行文件名=full-test-x64.exe
< DLL 进程ID=50340
> DLL 进程ID=26768
```

其余 249 行逐字节相同，包括拼音、数据库字段、字节集运算和结构体读写。

PE 头确认两者都是标准 Windows 控制台程序：

```text
x86:   14C machine (x86)    3 subsystem (Windows CUI)
x64:  8664 machine (x64)    3 subsystem (Windows CUI)
```

## 第五步：编译尺寸

这是很多人最关心的一栏。同一台机器、同一次会话的实测结果：

| 产物 | 输入工程 | 架构 | 尺寸 |
| --- | --- | --- | --- |
| `full-test-x86.exe` | `e-console-exe-full-test.e`（21 文件 / 1606 行） | x86 | **690,688 B**（约 675 KB） |
| `full-test-x64.exe` | 同上 | x64 | **795,648 B**（约 777 KB） |
| `from-dir-x86.exe` | 同上，从拆包目录编译 | x86 | 691,200 B |
| `hello-x86.exe` | `e-console-exe-new-proj.e`（空白控制台工程） | x86 | **116,224 B**（约 114 KB） |
| `gui-x86.exe` | 纯代码 GUI 工程（一个 `信息框`） | x86 | 177,152 B |
| `demo-x86.dll` | `e-win32-dll-new-proj.e` | x86 | 188,416 B |

几点观察：

- **空白控制台工程 116 KB** 是这条编译链的地板价。它包含现代 CRT 静态链接（`/MT`）和运行时桥接层，`commands=0`，一个核心库命令都没调用。
- **完整测试工程 690 KB**，比空工程多出约 574 KB。这部分是 137 个核心库命令的静态实现被真正链入的结果——可达性分析只拉进用到的部分，不是把整个核心库塞进去。
- **x64 比 x86 大 15%**（+105 KB）。这是 64 位指针、更宽的对齐和 x64 调用约定的正常代价。
- **从拆包目录编译比从 `.e` 编译多 512 B**，因为我在目录里多加了一行 `输出行`（见下一节）。同一份源码走两条输入路径，产物尺寸基本一致。

中间产物默认保留，方便排查：

| 文件 | x86 | x64 |
| --- | --- | --- |
| `<输出名>.generated.cpp` | 295,632 B | 295,648 B |
| `<输出名>.obj` | 2,099,575 B | 3,188,826 B |

`.generated.cpp` 是那份 1606 行易语言源码生成的 C++ 翻译单元，可以直接打开看。里面能找到核心库命令的绑定声明：

```cpp
extern "C" void __cdecl krnln_CompPY_78_krnln(ert::MData*,int,ert::MData*);
extern "C" void __cdecl krnln_BinLeft_143_krnln(ert::MData*,int,ert::MData*);
```

以及 `.DLL声明.txt` 转成的真实导入声明：

```cpp
extern "C" __declspec(dllimport) int __stdcall ecompiler_import_13(const char*);
```

这些中间文件可以随时删除，不影响已生成的 EXE。

## 第六步：改源码再编译

这才是这条链路真正有用的地方。先拆包：

```bash
$ ./bin/Win32/Release/e-packager.exe unpack eproj/e-console-exe-full-test.e temp/full-test-src
unpack: source_files=21, form_files=0, resources=1, ecom_modules=1, elib_files=2
```

得到一个可读、可 Git、可被 AI 编辑的目录。改 `src/测试_核心支持库总入口.txt`，加一行：

```text
输出行 (“======== 核心支持库全面测试开始 ========”)
输出行 (“运行目录=” ＋ 目录)
输出行 (“来自拆包目录的改动：Hello from e-packager compile”)
```

然后直接编译这个目录：

```bash
$ ./bin/Win32/Release/e-packager.exe compile temp/full-test-src temp/from-dir-x86.exe \
    --blackmoon-x86-dir "$LOCALAPPDATA/e-packager/dependencies/BlackMoonModernCore/v0.0.1/x86/extracted/adapter"
compile: compiled:...\from-dir-x86.exe;arch=x86;methods=53;commands=137;libraries=1
```

跑起来：

```text
======== 核心支持库全面测试开始 ========
运行目录=D:\git\e-packager\temp\
来自拆包目录的改动：Hello from e-packager compile
======== 开始流程与运算测试 ========
```

改动一路走到了可执行文件里。整个循环——拆包、编辑、编译、运行——没有打开过一次 IDE。

> **目录输入需要显式传 adapter。** 自动下载询问只对直接传入的 `.e` 启用；编译已拆包目录时，e-packager 不会暗中修改依赖搜索路径。缺依赖时它会直接失败（例如 `mfc_runtime_file_not_found:...NAFXCW.LIB`，意为它退回去找传统 x86 运行库了），而不是悄悄换一份库。这是刻意的设计：目录是你的工作区，编译器不该在你背后动它的依赖。

## 第七步：编译成 DLL

输出扩展名写 `.dll` 就够了，`--dll` 可以省略：

```bash
$ ./bin/Win32/Release/e-packager.exe compile eproj/e-win32-dll-new-proj.e temp/demo-x86.dll
compile: compiled:...\demo-x86.dll;arch=x86;methods=3;commands=2;libraries=1
```

工程里标记为公开的子程序会成为真正的 PE 导出：

```bash
$ dumpbin /exports temp/demo-x86.dll
    ordinal hint RVA      name
          1    0 00009A30 TestPub1
```

只有 `.子程序 TestPub1, , 公开` 被导出，内部方法不会误导出。导出名和修饰按参数栈大小与调用约定计算，`($cdecl)`、`($stdcall)`、`($name=...)` 等方法注释指令会影响结果。

同样值得一看的是导入表。`.DLL声明.txt` 里声明的外部函数进入了 PE 的 Import Directory，而不是编译器偷偷调用 `LoadLibrary`：

```bash
$ dumpbin /imports temp/full-test-x86.exe
    KERNEL32.dll
    USER32.dll
    SHELL32.dll
    OLEAUT32.dll
    SHLWAPI.dll
```

这是标准 PE 语义，也意味着标准后果：声明的 DLL 在启动时不存在，Windows loader 会直接拒绝启动进程。这不是编译器漏了什么，是静态导入本来的行为。

## 第八步：不弹控制台的 GUI 程序

`compile` 默认 `--subsystem auto`：控制台工程用 `CONSOLE`，易语言系统信息段标记为窗口工程的项目用 `WINDOWS`。

带窗体的工程目前会被明确拒绝：

```bash
$ ./bin/Win32/Release/e-packager.exe compile eproj/e-window-exe-new-proj.e temp/window.exe
compile failed: window_project_not_supported_by_independent_compiler
```

控件布局和事件生成还没进入语义编译器，所以它宁可报错，也不生成一个"看起来成功但运行不对"的程序。

但窗口工程删掉全部窗体、只留纯代码后是可以编译的，而且会保留 GUI 子系统。我拆了 `e-window-exe-new-proj.e`，删掉 `src/_启动窗口.xml`，把 `project/_meta.json` 的 `formFiles` 清空、改指向一个代码程序集（`projectSubsystem` 保持 `windows`），源码写：

```text
.子程序 _启动子程序, 整数型, , 本子程序在程序启动后最先执行

信息框 (“来自 e-packager 直接编译的 GUI 程序”, 0, “GUI 演示”)
返回 (0)
```

编译并检查 PE 头：

```bash
$ ./bin/Win32/Release/e-packager.exe compile temp/gui-src temp/gui-x86.exe --blackmoon-x86-dir "..."
compile: compiled:...\gui-x86.exe;arch=x86;methods=1;commands=1;libraries=1

$ dumpbin /headers temp/gui-x86.exe | findstr subsystem
               2 subsystem (Windows GUI)
```

产物 177,152 B，`subsystem 2` = Windows GUI。运行时弹信息框，不会附带一个黑色控制台窗口——GUI EXE 由生成器提供 `WinMain` 入口。这条路径适合黑月界面类之类的纯代码 Win32 UI。

需要覆盖工程元数据时显式指定：

```powershell
e-packager.exe compile MyApp.e .\out\MyApp-gui.exe     --subsystem windows
e-packager.exe compile MyApp.e .\out\MyApp-console.exe --subsystem console
```

`.dll` 输出始终是 DLL 子系统，不会被 `--subsystem console` 改成控制台程序。

## 第九步：编译失败时怎么读错误

诊断默认是人类可读的文本，包含阶段、文件、行号和错误码。我故意把一行改坏（去掉右括号）：

```text
目录 ＝ 取运行目录 (
```

编译直接失败，定位准确：

```bash
$ e-packager.exe compile temp/broken-src temp/broken.exe --blackmoon-x86-dir "..."
compile failed: compiler_model_failed:src/测试_核心支持库总入口.txt:9: expression_parse_failed:expression_operand_missing
```

也可以在编译前用 `validate` 单独跑一遍预检，它更快而且会一次列出多条问题。加 `--diagnostics json` 供脚本或 IDE 解析：

```bash
$ e-packager.exe validate temp/broken-src --diagnostics json
{"diagnostics":[
  {"code":"delimiter_unclosed","file":"src/测试_核心支持库总入口.txt","line":9,
   "message":"expression delimiter is not closed on this statement",
   "phase":"preflight","severity":"error","sourceLine":"目录 ＝ 取运行目录 ("},
  ...
],"errors":3,"files":25,"lines":1730,"ok":false,"warnings":0}
```

JSON 始终包含 `phase`、`code`、`file`、`line`、`column`、`message`、`sourceLine` 和 `rawOutput`。解析和语义错误直接指向易语言源码；C++ 编译错误通过生成代码里的源位置映射回源码；实在映射不了的链接器错误会保留后端文件和原始输出，不会假装知道对应哪一行。

`compile` 同样支持 `--diagnostics json`。

一个诚实的注脚：这份测试工程在 `validate` 下会报一条 `declaration_name_missing`（`.子程序` 后面没有名字），原始 `.e` 一拆包就有，不是编辑引入的。预检比编译器建模更严格，两者不完全同步；`errors=0` 表示"当前覆盖范围内没发现确定性错误"，不等于编译一定成功，反之也一样。

## 常用选项速查

| 选项 | 作用 |
| --- | --- |
| `--compile-mode semantic\|legacy-blackmoon\|blackmoon` | 编译路线；默认 `semantic`，`blackmoon` 是兼容别名 |
| `--arch host\|x86\|x64` | 目标架构，默认跟随当前程序 |
| `--subsystem auto\|console\|windows` | 子系统，默认 `auto` |
| `--dll` | 按 DLL 编译；输出为 `.dll` 时可省略 |
| `--define <宏>` / `-D <宏>` | 条件编译宏，可重复；对应子程序头注释里的 `$(宏名,...)` |
| `--diagnostics text\|json` | 诊断格式 |
| `--compiler` / `--linker` / `--lib` | 显式指定 C++ 工具链 |
| `--blackmoon-x86-dir` / `--blackmoon-x64-dir` | 核心 adapter 目录，可重复传入 |
| `--blackmoon-timeout <秒>` | 编译链接超时，默认 120，范围 1–3600 |

adapter 里的 FNE、主归档、后备归档和清单必须来自**同一个** Release，不要混用版本。

## 两条路线，选哪条

| `--compile-mode` | 架构 | 需要什么 |
| --- | --- | --- |
| `semantic`（默认，推荐） | x86 / x64 | 一套 Visual C++ 工具链；调用核心库命令时加一份同架构 BlackMoonModernCore adapter |
| `legacy-blackmoon` | 仅 x86 | 已安装的易语言、AutoLinker、黑月工具链，以及匹配的传统核心归档 |

本文全程用的是 `semantic`。没有特殊需求就别动它。只有两种情况需要 `legacy-blackmoon`：你依赖传统黑月工具链，或者需要和旧编译产物保持逐字节一致。传统路线仍需完整的 `BlackMoon\bin\LINK.EXE`、`BlackMoonExe.obj`、`EyInit.obj`，以及 x86 Release 包里 `legacy_static_lib\x86\krnln.lib` 那份 VC6 归档——它不能替代 semantic adapter，两者也不能混用。

## 当前边界

这条链路已经贯通"源码 → 语义模型 → 元数据驱动的 FNE 调用 → C++/OBJ → 链接 → EXE/DLL"，但它不是易语言 IDE 的等价替代。以下是实测确认的边界：

- **带窗体的窗口工程不支持**，返回 `window_project_not_supported_by_independent_compiler`。删净窗体的纯代码工程可以编译并保留 GUI 子系统。
- **需要匹配架构的 adapter**。只有 x86 归档时编译 x64 会在库发现阶段被拒绝，编译器不会跨位数猜测。
- **完整易语言语法仍在扩展**。特殊编译指令、少见表达式、全部隐式类型转换尚未宣称覆盖。
- **`置入代码`** 目前支持字节集字面量 + x86 naked helper；与普通语句混合的机器码会明确报错，而不是猜寄存器状态。
- **复杂数组、复合类型、DLL 边界参数**走的是通用包装路径，但还需要更多真实库的 ABI 测试，不能承诺在所有边界与 IDE 等价。
- **COM/Variant、线程模型**和部分支持库的私有运行时约定可能仍需额外 ABI 描述。

如果某个命令在支持库或静态库里缺实现，正确做法是换一份匹配版本的支持库，而不是改源码绕开它。

## 一页速查

```bash
# 最简：让它自己探测工具链、自己下载核心 adapter
e-packager compile MyApp.e out\MyApp.exe

# x64
e-packager compile MyApp.e out\MyApp-x64.exe --arch x64

# DLL（公开子程序自动导出）
e-packager compile MyLib.e out\MyLib.dll

# CI 友好：显式 adapter，不弹交互提问
e-packager compile MyApp.e out\MyApp.exe --arch x86 --blackmoon-x86-dir D:\deps\BMC\adapter

# 拆包 → 编辑 → 编译（AI/脚本工作流）
e-packager unpack MyApp.e MyApp\
#   ...编辑 MyApp\src\*.txt...
e-packager validate MyApp\ --diagnostics json
e-packager compile MyApp\ out\MyApp.exe --blackmoon-x86-dir D:\deps\BMC\adapter

# 交付前用真实 IDE 复核
e-packager compile-check out\checked.e
```

实现原理、语义模型细节和已验证范围详见 [`independent-compiler-architecture.md`](independent-compiler-architecture.md)。
