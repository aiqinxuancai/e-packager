# 易语言源码直接编译：实现原理与当前成果

本文记录 `e-packager` 当前已经实现的独立编译后端。这里的“独立”是指：输入易语言 `.e` 工程或其拆包目录后，由 `e-packager` 自己完成源码读取、语义建模、C++ 代码生成、OBJ 编译和最终链接，不启动易语言 IDE。

本文描述的是现有代码的真实能力和边界，不把当前后端描述成对全部易语言语法、窗口框架和所有支持库 ABI 的完全替代。

## 1. 目标与定位

### 目标

- 支持 Win32 控制台 EXE 的无 IDE 编译。
- 支持 Win32 DLL 的生成，并把易语言公开子程序导出为 DLL 导出函数。
- 从工程依赖的 FNE 中读取公开接口和 ABI 元数据，建立通用命令绑定。
- 链接已有支持库静态实现，而不是重新实现每个支持库函数。
- 把 `.DLL声明.txt` 中的外部 DLL 命令转换为真正的 PE 导入表项。
- 保持源码、支持库元数据、静态库实现和最终 PE 文件之间的职责边界清晰。

### 非目标

- 当前不编译窗口工程。检测到窗体文件或窗口绑定时，会返回
  `window_project_not_supported_by_independent_compiler`。
- 当前独立后端只生成 Win32 目标。x64 版本的 `e-packager` 可以正常构建，但不能代替 Win32 进程加载 x86 FNE，也不会伪装成支持 x64 独立编译。
- 当前不是易语言 IDE 的逐字节代码生成器。完整语法、窗口事件、所有隐式转换、COM/Variant 边界、线程语义以及每一个支持库的特殊 ABI 仍需要逐步补齐和验证。

因此，准确的表述是：已经形成一条可运行的、元数据驱动的 Win32 独立编译链，并覆盖控制台程序、DLL、支持库调用和外部 DLL 导入等核心路径；不是“任意易语言项目都已经与 IDE 完全等价”。

## 2. 端到端流水线

```text
原生 .e ───────────────┐
                       ├─ 读取为 ProjectBundle
拆包目录（*.txt 等） ──┘
                              │
                              ▼
                   BuildCompilerModel
                    ├─ 读取工程声明
                    ├─ 加载每个 FNE 的 GetNewInf
                    ├─ 注册系统类型/复合类型/常量
                    ├─ 解析 .DLL声明.txt
                    ├─ 解析子程序、变量、表达式和控制流
                    └─ 建立命令、方法、库的语义索引
                              │
                              ▼
                       EmitCppSource
                    ├─ 计算可达方法、命令和支持库
                    ├─ 生成 Value/Array/MData 运行时桥接
                    ├─ 绑定 FNE executeSymbol
                    ├─ 生成外部 DLL dllimport 声明
                    └─ （DLL）生成公开子程序包装器
                              │
                              ▼
                       cl.exe /c（Win32）
                              │
                              ▼
                              OBJ
                              │
             ┌────────────────┴────────────────┐
             │                                 │
             ▼                                 ▼
   VC6 LIB.EXE 生成导入库                 VC/Windows SDK 库
   （每个外部 DLL 声明）                  + FNE 静态 .lib
             │                                 │
             └────────────────┬────────────────┘
                              ▼
                   linker /MACHINE:I386
                    ├─ EXE：入口为生成的 main
                    └─ DLL：/DLL + /DEF，附带 DllMain
                              │
                              ▼
                         output.exe/.dll
```

`ECompiler.cpp` 负责流程编排和工具链调用；`CompilerModel.*` 负责语义模型；`CppEmitter.*` 负责从模型生成一个可由 VC 编译的 C++ 翻译单元。这样可以把“解析错误”“模型错误”“代码生成错误”“工具链错误”区分开，而不是在一个函数中拼接命令行和魔法分支。

## 3. 输入形式与工程读取

编译入口为：

```text
e-packager compile <input.e|input-dir> <output.exe|output.dll> [--dll] [--linker <link.exe>] [--lib <lib-dir>]
```

### 原生 `.e`

原生工程先由现有 `e2txt::Generator` 解码为 `ProjectBundle`。编译器后续只依赖这个统一结构，不在语义阶段重复解析 `.e` 二进制布局。

### 拆包目录

目录输入由 `BundleDirectoryCodec` 读取。目录中的源码、程序集、模块、依赖声明、资源索引和 `.DLL声明.txt` 进入同一个 `ProjectBundle`，因此“直接编译原生 `.e`”与“修改后编译拆包目录”走的是同一条后端路径。

### 窗口项目检查

若 `ProjectBundle` 含有窗体文件或窗口绑定，当前后端在生成 C++ 前明确拒绝。这样不会把窗口语义误当成控制台语义，也不会生成一个看似成功但运行时不正确的程序。

## 4. FNE 元数据：接口与实现的分工

FNE 是易语言支持库文件。独立编译涉及两类不同数据，必须分开理解：

| 对象 | 作用 | 是否直接提供函数实现 |
| --- | --- | --- |
| FNE 的 `GetNewInf`/公开信息 | 描述命令、参数、返回值、复合类型、常量、通知函数和依赖 | 否，主要是接口和运行时描述 |
| 支持库静态 `.lib` | 提供命令的机器码实现以及其 CRT/平台依赖 | 是，链接阶段使用 |
| `.DLL声明.txt` | 描述工程要调用的外部原生 DLL 函数 | 否，由链接器生成导入库 |
| 生成的 `.dll` | 本工程的输出文件 | 包含本工程包装器和导出表 |

`SupportLibraryPublicInfo.*` 把 FNE 内存中的结构复制成稳定的 C++ 元数据对象，避免后续阶段继续持有 FNE 内部指针。当前复制的主要信息包括：

- `LibraryMetadata`：文件、名称、GUID、版本、通知符号、依赖库列表。
- `CommandMetadata`：易语言命令名、英文名、执行符号、分类、状态、返回类型和参数数组。
- `ArgumentMetadata`：参数类型、状态位、默认值和名称。
- `DataTypeMetadata`：复合类型、枚举标志、成员命令索引和成员定义。
- `ConstantMetadata`：常量名称、类型、数值或文本值。

FNE 的加载本身依赖 x86 DLL 加载环境，所以真正读取 FNE 元数据的操作在 Win32 构建中执行。x64 构建不会通过错误的位数加载 x86 FNE。

## 5. 语义模型

`CompilerModel.h` 中的模型是从“文本行”到“可生成代码的程序”的中间层，主要对象如下：

- `Program`：整个工程，包括库、DLL 命令、程序集、全局变量、方法、类型和常量索引。
- `Library`：工程中的一个 ELib/FNE 依赖及其完整 `LibraryMetadata`。
- `Method`：子程序的返回类型、参数、局部变量、公开标志、调用约定和语句体。
- `Assembly`：程序集级变量和方法归属。
- `Variable`：类型、按引用属性、可空属性和数组维度。
- `DllCommand`：外部 DLL 文件名、入口名、调用约定、返回类型和参数。
- `TypeInfo`：支持库复合类型或枚举的大小、成员偏移和成员命令关联。
- `Statement`：表达式、赋值、返回、条件、分支、循环、`break` 和 `continue`。

### 类型解析

系统类型使用统一编码，例如整数、长整数、文本、字节集和子程序指针。支持库类型不会通过函数名猜测，而是由 FNE 的数据类型表注册到 `typeByName`/`typeByCode`。复合类型大小按成员类型递归计算，数组成员按易语言运行时的对象指针模型处理；遇到递归类型或未知类型时在建模阶段报错。

### 命令解析

源码中的命令名先在工程方法、支持库命令、成员命令和 DLL 命令索引中解析。绑定结果包含 `(libraryIndex, commandIndex)`，最终使用对应 FNE 的 `executeSymbol`。因此同名命令、命令顺序变化或新增支持库时，不需要在发射器中新增“某个函数名的特殊分支”。

### 参数状态

参数的按引用、可空、数组维度和默认值来自声明及 FNE 参数状态，不以参数名或命令名推测。该信息同时影响：

1. 语义检查和局部变量表示；
2. 调用支持库时的 `MData` 打包；
3. 调用返回后的写回；
4. DLL 导出包装器的 C ABI 类型。

## 6. 源码解析与控制流

`SourceExpressionParser` 负责表达式树，支持字面量、调用、成员访问、数组索引、运算符和括号等基本结构。`CompilerModel.cpp` 在表达式树之上识别普通易语言源文件中的声明和语句，并构造结构化控制流：

- 条件与否则分支；
- `while`、先判断/后判断循环；
- 计次循环和范围循环；
- `break`、`continue`、`return`；
- 普通赋值和表达式语句。

发射器先从 `_启动子程序` 和可达调用开始遍历方法、支持库命令及依赖库，只生成需要的代码和链接对象引用。生成结果中会统计 `methods`、`commands`、`libraries`，便于检查实际编译覆盖范围。

源码页中的 `.支持库 <name>` 声明也会参与依赖恢复。当编辑后的 `.e` 头部依赖快照不完整、但源码仍保留支持库声明时，模型会按标准 FNE 搜索目录补入 ELib 依赖；这避免把二进制快照是否被重建误认为源码语义变化。

类程序集（`.程序集 类名,`）使用同一类型系统注册为用户类型。类实例变量拥有独立 `Value` 字段，成员方法通过对象接收者调用，局部实例在构造阶段执行 `_初始化`。当前类继承、多态分派和完整的 `_销毁` 生命周期仍在扩展中。

ECom/EC 模块依赖也会展开到同一模型：读取模块源码、类型页、DLL 声明和常量页，剔除模块自身的 `_启动子程序`，再合并其 ELib 依赖。模块公开类和方法因此可以参与主工程的解析；模块内部生成的匿名类型/匿名子程序仍要求其符号页能够完整导出。

## 7. C++ 发射器与运行时桥接

`CppEmitter.cpp` 生成一个带 UTF-8 BOM 的 C++ 源文件，内容可以分为四层：

1. **值层**：用统一 `Value` 表示整数、浮点、文本、字节集、数组和复合对象。
2. **表达式/语句层**：把语义模型翻译成 C++ 表达式、临时值、作用域和控制流。
3. **FNE 调用层**：按元数据构造参数数组，调用具体 `executeSymbol`，再按返回类型和状态转换回 `Value`。
4. **宿主层**：提供易语言/FNE 需要的内存、路径、命令行、生命周期和通知回调符号。

### MData ABI

传统 FNE 命令通过固定的三参数入口接收 MData 数组，当前生成器使用等价布局：

```text
ExecuteCommand(MData* result, int argumentCount, MData* arguments)
```

MData 内含类型编码和联合值。发射器会根据参数的元数据状态执行以下通用步骤：

```text
Value
  -> MarshalValue / MarshalByReference
  -> MData 数组
  -> FNE executeSymbol
  -> MData 返回值
  -> CopyReturned
  -> Value
```

文本和字节集使用运行时分配的缓冲区；数组带维度和元素区；复合类型通过类型描述计算大小并进行成员复制。调用结束后根据所有权和按引用状态释放或写回，避免把某个核心库命令的参数顺序硬编码到发射器。

### 通知号和宿主符号

生成的宿主实现了当前 FNE ABI 用到的系统通知，包括数值转整数、内存分配/释放/重分配、数组释放、命令行、EXE 路径、文件登记和程序退出等。FNE 可能引用 `BlackMoonFuncForeLibNotifySys`、`BlackMoonCalleLibList`、`hBlackMoonHeap` 等历史 ABI 名称；这些名称是现有 FNE 二进制接口要求的符号名，并不表示链接了黑月实现源码。

`E_Init` 会把宿主通知函数交给可达支持库；`E_DestroyRes` 按相反的生命周期方向释放全局变量、程序集变量和支持库资源。

### 编译期内建与置入代码

FNE 中没有运行时执行符号的编译期命令由统一的英文元数据名分派，不会被当作普通 FNE 调用。例如 `hex`、`binary`、`GetAppName`、`XchgVar`、`ForceXchgVar`、`GetRuntimeDataType` 和 `IsCondMacroDefined` 在生成阶段完成对应的常量求值或值操作。

`置入代码`（`MachineCode`）是另一类编译期命令。字节集参数会在建模阶段解析为字节序列，生成器随后发出 x86 `__declspec(naked)` helper，并由易语言语义方法以整数参数调用该 helper。这样原始 `ret`、栈参数和 EAX 返回值位于独立的机器码 ABI 中，不会破坏外层 `Value` C++ 栈帧。当前要求机器码方法体只包含这一条置入代码语句；机器码与普通易语言语句混合时会明确报错，而不是猜测寄存器和栈状态。

可用 `--define <macro>`（或 `-D <macro>`）提供编译条件宏。子程序头注释中的 `$([!]宏名,...)` 会在建模阶段决定该对象是否进入当前编译，宏名称不区分大小写。未定义宏引用的子程序不会进入方法索引，因此调用它会得到确定的未知符号诊断。

## 8. 支持库静态链接

编译器不会把 FNE 当作最终机器码来源，而是在发射器收集可达库之后查找对应静态归档：

1. 核心库优先查找 `krnln_static.lib`。
2. 普通 FNE 根据 FNE 文件名、依赖文件名和 `_static.lib`/`.lib` 变体查找。
3. 每个可达 FNE 的 `NL_GET_DEPENDENT_LIBS` 返回值会继续解析到产品 `static_lib`、VC 库目录或工程目录。
4. 最终把支持库 `.lib`、VC6 CRT/MFC、现代 CRT 和 Windows SDK 导入库一并交给链接器。

当前产品目录的默认根为 `C:\Users\aiqin\OneDrive\e5.6\linker`。默认 C++ 编译器为其下的 `VC2022Linker\bin\cl.exe`，链接器为同目录 `link.exe`；也可以通过 `--linker` 和 `--lib` 覆盖。编译器会自动探测 Visual C++ 和 Windows SDK 的 Win32 include/lib 目录。

为了兼容现有核心静态归档，当前还保留核心库级别的 `odbcdb_static.lib`、`mp3_static.lib` 依赖补充。这是归档级兼容项，不按某个源代码函数分支；长期方向是让这些依赖也完全由 FNE/静态库元数据发布，消除产品树约定。

## 9. 外部 DLL：导入表而不是运行时假调用

项目中的 `.DLL声明.txt` 会被解析为 `DllCommand`。例如：

```text
.DLL命令 SomeCall, 整数型, "kernel32.dll", "SomeCall"
.参数 value, 整数型
```

发射器为它生成内部导入符号：

```cpp
extern "C" __declspec(dllimport) int __stdcall ecompiler_import_0(int);
```

随后 `ECompiler.cpp` 为每个外部命令写临时 `.def`，调用 VC6 `LIB.EXE` 生成 import `.lib`，最后把该 `.lib` 交给最终链接器。结果是 DLL 名称和函数名进入 PE 的 Import Directory，而不是由后端偷偷调用 `LoadLibrary`/`GetProcAddress`。

可用 `dumpbin /imports` 检查最终 EXE/DLL。静态导入的标准行为是：如果声明的 DLL 在进程启动时不存在，Windows loader 可能直接拒绝启动；这不是编译器遗漏，而是导入表语义本身。

## 10. Win32 DLL 输出与公开函数导出

当输出扩展名为 `.dll` 或传入 `--dll` 时，生成器进入 DLL 模式。

### 公开子程序

源文件中的：

```text
.子程序 TestPub1, , 公开
```

会产生一个内部 C++ 包装器，例如 `ecompiler_export_1`。包装器负责：

- 把原生 C 参数转换成易语言 `Value`/`MData`；
- 调用语义模型中的同一个方法实现；
- 把按引用参数、文本、字节集和返回值转换回 C ABI；
- 对复杂类型按已注册的类型大小和成员布局复制。

然后生成 `.def`：

```text
LIBRARY "e-win32-dll-new-proj"
EXPORTS
    TestPub1=_ecompiler_export_1@0
```

实际修饰名会按参数栈大小和调用约定计算；`($cdecl)`、`($stdcall)` 和 `($name=...)` 等方法注释指令会影响调用约定和导出名。最终 PE 导出表只包含标记为公开的子程序，不会把普通内部方法误导出。

### DLL 生命周期

生成 DLL 自带 `DllMain`：

- `DLL_PROCESS_ATTACH`：关闭线程通知，调用 `E_Init()` 和 `ECodeStart()`；
- `DLL_PROCESS_DETACH`：调用 `E_DestroyRes()`，释放全局/程序集值和支持库资源。

这使 DLL 与控制台 EXE 共用同一套模型和运行时桥接，只在入口和导出层有目标类型差异。链接器同时使用 `/DLL`、`/DEF`、`/MACHINE:I386`，并可生成带 `__imp_TestPub1` 的导入库供其他程序调用。

## 11. 工具链和产物

默认编译阶段使用以下关键选项：

```text
/c /O2 /Gy /Zl /GS- /GR- /EHsc /arch:IA32 /MT /std:c++20
/source-charset:utf-8 /execution-charset:.936
```

链接阶段固定 Win32 目标，并加入 VC6/MFC 兼容库、现代 CRT、Windows SDK 导入库及可达支持库。默认会保留以下调试/审计产物：

- `<output>.generated.cpp`：生成的 C++ 翻译单元；
- `<output>.obj`：`cl.exe` 产生的 COFF 对象；
- `<output>.dll` 或 `<output>.exe`：最终 PE；
- DLL 模式下的 `<output>.def`；
- 外部 DLL 声明对应的临时 import `.lib`（可在保留产物时检查）。

`Result.message` 会报告输出路径以及方法、命令、库数量，例如：

```text
compiled:<output>;methods=...;commands=...;libraries=...;source=...;object=...
```

## 12. 使用示例

### 控制台 EXE

```powershell
bin\Win32\Release\e-packager.exe compile eproj\e-console-exe-full-test.e temp\e-console-exe-full-test.exe
temp\e-console-exe-full-test.exe
```

### Win32 DLL

```powershell
bin\Win32\Release\e-packager.exe compile eproj\e-win32-dll-new-proj.e temp\e-win32-dll-new-proj.dll --dll
dumpbin /exports temp\e-win32-dll-new-proj.dll
```

### 指定链接器或支持库目录

```powershell
bin\Win32\Release\e-packager.exe compile <input.e> <output.exe> `
  --linker C:\Users\aiqin\OneDrive\e5.6\linker\VC2022Linker\bin\link.exe `
  --lib C:\Users\aiqin\OneDrive\e5.6\linker\VC2022Linker\lib
```

编译原生 `.e` 和编译拆包目录的命令形式相同；目录必须是 `e-packager unpack` 生成或符合项目目录布局的目录。

## 13. 已验证成果

### 构建

- Release Win32 `e-packager`：已成功构建。
- Release x64 `e-packager`：已成功构建；其独立编译命令会明确返回 Win32 后端限制，而不是尝试加载错误位数的 FNE。

### 控制台回归

`eproj\e-console-exe-full-test.e` 已通过独立编译并运行，退出码为 `0`，未出现不支持系统功能函数提示。验证输出包括：

```text
字节集相加长度=5
写环境变量成功 读回=AutoLinkerCoreTest
取字段名1=编号
取字段类型2=10
取剪辑板文本长度=19
置剪辑板后读回=core-clipboard-test
核心支持库全面测试结束
```

该结果证明当前链路已经贯通：源码解析、核心 FNE 绑定、核心静态库链接、运行时通知、文本/字节集/环境变量/剪贴板等调用和程序退出。

### DLL 回归

`eproj\e-win32-dll-new-proj.e` 已生成 Win32 DLL。`dumpbin /exports` 显示：

```text
1 number of functions
1 number of names
TestPub1
```

外部 32 位调用程序已成功调用 `TestPub1`，退出码为 `0`。单独的导入表探针也确认最终 PE 含有 `kernel32.dll`、`user32.dll` 及其声明函数的导入项，说明外部 DLL 不是仅停留在源码声明层。

## 14. 通用性与反魔法原则

当前实现遵循以下约束：

1. **函数绑定由 FNE 元数据决定**：通过命令索引、执行符号、参数状态和返回类型绑定，不通过 `if (name == "某函数")` 改写行为。
2. **类型由统一类型表决定**：系统类型、复合类型、枚举和数组共享 `TypeRef`/`TypeInfo` 路径，不为某个库单独复制一套参数转换器。
3. **运行时能力按 ABI 通知号提供**：文件、内存、路径、退出等能力由通知接口统一分派，新增支持库不应要求新增函数名分支。
4. **依赖按可达图收集**：从启动子程序遍历到达的命令和库，再解析 FNE 发布的传递依赖，避免无条件把整个库树塞进最终文件。
5. **导入/导出由 PE 标准机制完成**：项目 DLL 声明生成 import library，公开子程序生成 `.def`，不以自定义运行时表冒充导入表或导出表。
6. **失败尽量前移**：未知类型、缺失 FNE、缺失静态库、位数不匹配和窗口工程在模型/工具链阶段报告，而不是生成一个运行时才弹错的半成品。

仍需诚实指出两处产品兼容约定：默认工具链路径来自当前部署环境；核心静态归档的 ODBC/MP3 依赖有一个归档级补充列表。它们都不是针对某个易语言命令的行为修正，但后续应继续迁移到可发现的库元数据中。

## 15. 已知限制与后续方向

### 已知限制

- 窗口工程、窗口事件和界面资源尚未接入独立后端。
- 完整易语言语法仍在扩展，特殊编译指令、少见表达式、所有隐式类型转换尚未宣称覆盖。
- `置入代码` 当前支持字节集字面量和 x86 naked helper 路径；机器码文件名输入、依赖当前 C++ 栈帧/局部变量的复杂内联片段，以及与普通语句混合的机器码尚未宣称与 IDE 等价。
- 复杂数组、复合类型、二进制返回值和 DLL 边界参数虽然走通用包装路径，但还需要按更多真实库和调用者进行 ABI 兼容性测试，不能直接承诺与 IDE 在所有边界上等价。
- COM/Variant、线程模型和部分支持库私有运行时约定仍可能要求额外 ABI 描述。
- FNE 元数据读取依赖 Win32；要支持 x64，需要对应位数的 FNE 和静态库，而不仅是把当前进程重新编译为 x64。
- 静态导入 DLL 的缺失文件会按 Windows loader 规则阻止进程启动；若业务需要可选 DLL，应在语言/工程模型中显式定义延迟加载语义。

### 后续优先级

1. 将剩余核心静态库传递依赖全部纳入 FNE 或配套清单元数据，移除产品目录约定。
2. 扩展语法树和类型检查，优先覆盖控制台项目中常见但尚未建模的语句。
3. 为复杂参数建立按库/版本可验证的 ABI 测试样例，并用自动化 PE 导入导出检查替代人工判断。
4. 设计窗口项目的独立运行时边界，再实现窗口资源和事件，而不是把窗口源码硬塞进控制台入口。
5. 在具备对应 x64 FNE/静态库之后，再增加真正的 x64 后端；不对现有 x86 二进制做跨位数猜测。

## 16. 黑月源码后端

除默认的 `native` C++ 后端外，`compile` 还支持 `blackmoon` 后端：

```powershell
bin\Win32\Release\e-packager.exe compile <input.e|input-dir> <output.exe|output.dll> `
  --backend blackmoon --blackmoon-mode asm `
  --eide C:\Users\aiqin\OneDrive\e5.6\e5.95.exe `
  --autolinker-test D:\git\AutoLinker\bin\fne_release\AutoLinkerTest.exe `
  --blackmoon-dir C:\Users\aiqin\OneDrive\e5.6\BlackMoon
```

该后端不加载、不注入、也不调用 `BlackMoon.fne`。它采用 BlackMoonNG 的易代码到 COFF 转换实现：

```text
.e / 拆包目录
  -> AutoLinker 无头动态编译（取得原生易代码 PE）
  -> BlackMoonNG EcodeToObjFile 源码转换
  -> BlackMoon.obj
  -> BlackMoonKernelStaticLib 入口 OBJ + krnln.lib
  -> link.exe
  -> output.exe / output.dll
```

动态阶段仍需 `e.exe`，因为黑月转换器的输入是 IDE 生成 PE 中 `E0000040` 标志的易代码段，而不是 `.e` 源码文本。该步骤通过 AutoLinker 无头接口完成；黑月 FNE 插件不参与其中。

`--blackmoon-timeout <seconds>` 同时限制无头易代码阶段和最终 `LINK.EXE` 阶段，允许范围为 1 至 3600 秒。

| 参数 | 首选黑月入口 | 实测最小工程尺寸 |
| --- | --- | --- |
| `--blackmoon-mode asm` | `BlackMoonExe.obj` + `EyInit.obj`，自定义 `BMEntrypoint` | 3,584 B |
| `--blackmoon-mode cpp` | `BlackMoonExe.obj` + `EyInit.obj` | 36,864 B |
| `--blackmoon-mode mfc` | `MFCBlackMoonCon.obj` / `MFCBlackMoon.obj` | 114,688 B |

上述实测均来自 `e-console-exe-new-proj.e`，三个产物均可运行并以 `0` 退出。DLL 会选择相应 `BlackMoonDll*.obj` 或 `BlackMoonMFCdll*.obj` 并生成导出 `.def`；`e-win32-dll-new-proj.e` 的汇编模式产物为 20,480 B，导出 `TestPub1`。

入口选择是能力驱动的，而不是按函数名写死。当核心归档的实际链接成员带入 `nafxcw`/MFC 运行库时，非 MFC 请求模式的第一次链接会被拒绝；后端随后改用对应的 `MFCBlackMoon*.obj`、`EyInit.obj`/`EyMFCComInit.obj` 和 CRT 初始化路径重试。结果消息会保留两项信息，例如：

```text
mode=asm;effective_mode=mfc;runtime_fallback=mfc
```

这不是忽略 `LNK2005` 的 `/FORCE` 方案。它避免混用 `MSVCRT`、`LIBCMT` 与 MFC 的对象，确保全局构造、浮点环境和 MFC 状态先完成初始化。当前 `e-console-exe-full-test.e` 含有核心拼音命令，所用 `PY.OBJ` 需要 MFC；重新测试时 `asm`、`cpp`、`mfc` 三种请求均以退出码 `0` 运行完成，输出均为 345 行并到达结束标记，前两种的 `effective_mode` 为 `mfc`。

默认保留 `<output>.blackmoon.ecode.exe`（或 `.dll`）和 `<output>.blackmoon.obj`，用于检查原生易代码和 COFF 转换结果。

黑月后端使用黑月核心静态库自己的命令映射和实现，不等同于默认后端所使用的 `krnln_static.lib`。因此它只支持黑月核心库实际实现过的命令。若 FNE 中存在声明但归档没有实现，转换阶段会明确报告命令英文名，例如旧版测试工程曾报告：

```text
黑月核心归档未提供核心库命令337的实现“_krnln_fnGetErrCode” [core_command=GetErrCode]
```

这是归档能力差异，不是针对项目或函数名写入的特殊分支。当前完整测试源码已移除该不兼容调用；如果更换为包含对应符号的新版黑月核心归档，无需修改编译器逻辑即可恢复该命令。

转换器代码位于 `src/compiler/blackmoon/`，来源为 BlackMoonNG，并在同目录保留其 MIT 许可证；黑月核心静态库遵循其仓库附带的 BSD 3-Clause 许可证。

## 17. 结论

当前成果已经把“`.e` 源码 → 语义模型 → 元数据驱动的 FNE 调用 → C++/OBJ → VC6/现代 CRT/支持库链接 → Win32 EXE 或 DLL”串成一条可运行链路。核心价值不在于为少数函数增加兼容补丁，而在于建立了可扩展的中间模型、ABI 转换层和标准 PE 链接路径。

在此基础上，新增支持库、外部 DLL 或公开子程序的主要工作应当是提供正确的元数据和静态实现，而不是修改编译器核心中的函数名分支。对于尚未覆盖的语言和窗口语义，文档中的限制应视为当前实现边界，后续按同样的模型驱动原则逐项扩展。
