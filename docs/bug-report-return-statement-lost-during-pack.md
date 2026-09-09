# 回包语义重建丢失“返回”语句问题报告

## 摘要

在包含可编辑源码的 `.e` 工程回包过程中，某些带行尾注释的 `返回 (...)` 语句会从最终工程中丢失。

该问题会导致：

1. 原始 workspace 中的源码看起来合法；
2. `e-packager pack` 成功返回；
3. `e-packager unpack` 也成功返回；
4. 回包后的工程源码缺少返回语句；
5. 易语言 IDE 编译时报告错误 10022：

   ```text
   错误(10022): 子程序“_启动子程序”具有返回值定义，但实际上却没有返回数据或者并不是所有程序分支都返回了数据。
   ```

这不是 IDE 对原始源码的误判，而是回包后工程已经发生语义变化，IDE 正确地报告了损坏后的工程状态。

## 影响版本与验证范围

已在以下版本验证：

| e-packager 版本 | commit | 结果 |
|---|---|---|
| Release v1.1.3 | `614ac9de507b3ad62de9036af7e46c2b56fea820` | 可复现 |
| master（v1.2.5 系列） | `0ef6b9560afb338e86da8edd157f8bb525ab7d2e` | 仍可复现 |

验证使用 Win32 Release 构建。master 版本虽然包含后续工程/编译器相关改动，但没有针对本报告问题的修复提交。

## 最小复现输入

创建目录 `repro/workspace/src/`，放入以下文件。

### `repro/workspace/src/.版本.txt`

```text
2
```

### `repro/workspace/src/程序集1.txt`

```text
.版本 2

.程序集 程序集1

.子程序 _启动子程序, 整数型, , 本子程序在程序启动后最先执行
.局部变量 结果, 长整数型
结果 ＝ 阶乘 (6)
返回 (0)  ' 可以根据您的需要返回任意数值

.子程序 阶乘, 长整数型, 公开, 递归计算 n 的阶乘
.参数 n, 整数型, , 整数n
.如果真 (n ≤ 1)
    返回 (1)
.如果真结束
返回 (n × 阶乘 (n － 1))
```

为了让 workspace 结构完整，还需要创建空的固定页文件（文件内容可以是一个换行）：

```text
repro/workspace/src/.常量.txt
repro/workspace/src/.全局变量.txt
repro/workspace/src/.数据类型.txt
repro/workspace/src/.DLL声明.txt
```

## 复现步骤

以下命令在 PowerShell 中执行。`e-packager.exe` 替换为待测试版本的绝对路径。

```powershell
$ep = "D:\git\e-packager\bin\Win32\Release\e-packager.exe"
$root = (Resolve-Path .).Path
$workspace = Join-Path $root "repro\workspace"
$packed = Join-Path $root "repro\candidate.e"
$reunpacked = Join-Path $root "repro\reunpacked"

& $ep pack $workspace $packed
& $ep unpack $packed $reunpacked --main-only
```

检查两个源码文件：

```powershell
Get-Content "$workspace\src\程序集1.txt"
Get-Content "$reunpacked\src\程序集1.txt"
```

## 实际结果

原始 workspace 中存在：

```text
返回 (0)  ' 可以根据您的需要返回任意数值
```

回包后重新解包的 `src/程序集1.txt` 中，该行消失，结果类似：

```text
.子程序 _启动子程序, 整数型, , 本子程序在程序启动后最先执行
.局部变量 结果, 长整数型

结果 ＝ 阶乘 (6)

.子程序 阶乘, 长整数型, 公开, 递归计算 n 的阶乘
```

注意：`pack` 和 `unpack` 的进程退出码均为 0。

## 编译阶段表现

将回包生成的 `candidate.e` 交给 AutoLinker/e5.95 编译，IDE 输出：

```text
正在编译现行程序
正在检查重复名称...
正在预处理现行程序
正在进行名称连接...
正在统计需要编译的子程序
正在编译...
错误(10022): 子程序“_启动子程序”具有返回值定义，但实际上却没有返回数据或者并不是所有程序分支都返回了数据。
```

错误位置通常为：

```text
page=程序集1 type=程序集 row=3
```

该错误来自 e5.95 编译器，不是 e-packager 输出的错误码。e-packager 的职责是生成导致该错误的损坏工程。

## 已保存的真实复现样本

本报告基于 e-language-bench 中的真实样本：

```text
D:\git\e-language-bench\results\20260908-deepseek-v4.1-flash-expires-on-0910-max-v1.2-p2-e113\cases\abs-02-skill\workspace\src\程序集1.txt
D:\git\e-language-bench\results\20260908-deepseek-v4.1-flash-expires-on-0910-max-v1.2-p2-e113\cases\abs-02-skill\reunpacked\src\程序集1.txt
```

该样本中：

- workspace 源码含有带行尾注释的 `返回 (0)`；
- v1.1.3 回包后该行消失；
- `pack`、`unpack` 和 `compare-bundle` 均返回成功；
- AutoLinker 编译阶段报告 10022。

## compare-bundle 校验盲点

同一个样本执行：

```powershell
& $ep compare-bundle $packed $workspace
```

输出：

```text
compare-bundle: digest_from_e=D336C834E907266E
digest_from_dir=D336C834E907266E
match=true
```

但 `unpack` 后的文本已经少了 `返回 (0)`。这表明当前 compare-bundle 摘要比较没有检测出该方法体语句丢失，至少存在以下风险之一：

- 摘要比较使用了原生快照或规范化表示，而不是重新解包后的完整源码；
- 方法体中的部分语句没有进入 digest；
- 语义重建前后的比较路径绕过了实际输出文本。

因此该问题不能只依赖 `compare-bundle match=true` 判定回包无损。

## 源码定位

在 `src/e2txt_restore.cpp` 中可以看到相关语义重建路径：

- `IsSemanticOnlyCoreSupportCommandLine(...)` 将“返回”等核心命令列为特殊语义命令；
- `TryEncodeNativeFunctionCallStatementLine(...)` 负责解析和编码函数调用语句；
- `TryEncodeNativeRawStatementLine(...)` 组合对象调用、赋值、函数调用和特殊命令的编码；
- `RestoreBundleToBytesInternal(...)` 在不能复用原生快照时执行源码预检和语义重建。

重点检查以下场景：

1. `返回 (0)` 无行尾注释；
2. `返回 (0)  ' comment` 带行尾注释；
3. `_启动子程序` 中只有返回语句；
4. `_启动子程序` 中包含局部变量、赋值、用户子程序调用后再返回；
5. 普通子程序和 `_启动子程序` 的差异；
6. 语句被复用原生快照和走 semantic rebuild 两种路径的差异。

## 建议的修复方向

### 1. 保证 `返回` 语句不会被静默丢弃

任何无法语义编码的 `返回` 语句都不应被忽略。应当：

- 保留原始 native line snapshot；或
- 正确解析并重新编码；或
- 让 pack 失败并返回明确错误。

不能在 pack 成功的情况下生成缺少返回语句的工程。

### 2. 正确处理行尾注释

建议将源码行拆分为“代码部分”和“注释部分”后再解析，例如：

```text
代码：返回 (0)
注释：' 可以根据您的需要返回任意数值
```

解析器不应把行尾注释当作表达式的一部分，也不应因此丢弃整个语句。

### 3. 强化 round-trip 测试

至少增加以下断言：

```text
pack(workspace) == success
unpack(packed) == success
reunpacked/src/程序集1.txt 包含 “返回 (0)”
```

并使用逐行或语义 AST 比较，而不是只依赖 `compare-bundle` 的摘要结果。

### 4. 将编译回归纳入测试

回包后应使用 e5.95/AutoLinker 编译最小工程，确保不会出现 10022。测试应同时覆盖：

- 无注释返回；
- 行尾注释返回；
- 返回表达式；
- 多分支返回；
- `_启动子程序` 返回整数型。

## 当前结论

截至本报告验证时，Release `v1.1.3` 和最新 master（v1.2.5 系列）均可复现该问题，尚不能认为 master 已修复。问题优先级应高于单纯的 IDE 错误处理，因为回包工具已经在成功状态下产生了与输入源码不等价的工程。

## 2026-09-08 最新 master 复测

远端 `origin/master` 当前仍为 `0ef6b9560afb338e86da8edd157f8bb525ab7d2e`。使用该工作树重新编译 Win32 Release（0 警告、0 错误）后，运行新增的回归脚本：

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File tools\TestReturnStatementRoundTrip.ps1
```

脚本覆盖带注释、无注释、修改注释、修改返回值、单独返回、字符串中含 apostrophe 以及多分支返回；这些“从干净 workspace 语义重建”的 case 均通过，说明当前未提交改动确实修复了该类路径。

此前真实 workspace 的复测确实仍会丢失该行。进一步定位发现，问题来自原生方法/行片段快照复用：复用条件只检查语义形状，未能识别行尾注释等文本变化，导致旧的 native method body 覆盖了新源码。

当前工作树已加入修正：当源码摘要与 native snapshot 不完全一致时，清除 native source/method/section snapshot，强制从当前源码进行语义重建；同时禁止在源码文本变化时复用旧方法快照。修正后重新测试真实 workspace，回包源码已保留：

```text
返回 (0)  ' 可以根据您的需要返回任意数值
```

该真实样本的 pack/unpack 均返回 0，且上述回归脚本的全部 6 组 case 均通过。
