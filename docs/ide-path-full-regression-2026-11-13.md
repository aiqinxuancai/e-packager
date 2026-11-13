# 普通路径与命令作用域调整：双架构抽样全流程回归

日期：2026-11-13。结论：在本次完成的 40 个文件、80 组架构流程中，没有发现本次 C++ 调整引入的语义回包回归；不能表述为所有导出文本均无问题。扩展扫描仍发现 3 个样本的既有符号名称问题，另有 1 个样本被本机防护隔离资源而阻塞。

## 范围与可复现证据

- 按 AGENTS.md 的全量测试约定，从 `C:/Users/aiqin/OneDrive/code/模块` 随机选取 20 个 `.e` 和 20 个 `.ec`，排除零字节及文件名含 `KanEc` 的文件；不是遍历全部项目。候选为 488 个 `.e`、187 个 `.ec`，种子 `20261113`。
- Win32/x64 各跑一次；隔离副本中逐页添加唯一注释，检查无修改回包字节一致、修改后语义回包、再拆包源码/窗口/资源一致、注释保留、第二次回包。1262 个代码页次实际被修改，80 个有效产物均发生字节变化。
- 5 个缺依赖样本在副本中补齐对应依赖后重测。因 ID 644 被隔离，另外随机抽取 1 个 `.ec`（ID 120，种子 `20261114`）；保留 ID 644 失败记录，没有以替补掩盖它。
- 总计尝试 41 个文件（20 `.e` + 21 `.ec`）。修改版首次 80 次 + 补测 12 次；HEAD 基线 78 次 + 补测 12 次。基线未重新提取被隔离资源。
- 基线来自不含工作区修改的 `git archive HEAD`，提交 `d724e8567261a5345618219b483dd5ade716a41d`，独立构建两个架构。不是把 v1.2.16 当作此次唯一基线。

证据根目录：`C:/Users/aiqin/AppData/Local/Temp/AutoLinker-full-regression-20261113-220212/`。

`patched-audit/manifest.json` 保存原始随机清单；`supplement-selection.json` 保存替补选择；各轮 `run.json`、`results.json`、命令与原始日志保留失败与重测过程。`tested-source.diff` 保存被测生产代码差异。原始审计脚本冻结在根目录，扩展符号扫描另存于各轮 `extended-symbol-scan.json`，没有覆盖原来的通过/失败记录。

## 结果

| 检查 | 结果 |
|---|---|
| 40 个有效文件的双架构完整流程 | 80/80 完成 |
| 无修改回包与原输入逐字节相同 | 80/80 |
| 页尾修改后源码、窗口、资源及标记回读 | 80/80 一致（忽略空行和行首尾空白） |
| 修改前后工具：初次拆包原生代码数据对比 | 80/80 相同 |
| 修改前后工具：语义回包后原生代码数据对比 | 80/80 相同 |
| 修改前后工具：资源差异 | 0 |
| 修改版 Win32/x64：语义数据及源码/资源清单 | 40/40 相同 |
| 扩展占位符扫描无发现 | 37 文件 / 74 组流程 |
| 既有占位符问题 | 3 文件 / 6 组流程 |
| 环境阻塞，未完成验证 | 1 文件 / 2 组流程 |
| 原文件及冻结工具哈希复核 | 全部未变化 |

语义对比递归检查 `.native_source_map.json`，包含声明 ID、类型、参数/局部变量、原生表达式数据、行/块偏移及方法/变量/常量引用。只排除进程地址和文本缓存摘要五个字段：`memoryAddress`、`classMemoryAddress`、`textDigest`、`contentDigest`、`classShapeDigest`。这不是对整个文件逐字节相同的宣称，也不等同于所有工程编译运行验证。

17 个文件的导出文本有预期变化（34 组架构对比）。逐块检查 Win32 的 1773 处差异：284 处为普通路径格式，1489 处为名称限定或两者组合；没有无法归入这两类的差异。x64 导出内容与 Win32 相同。完整差异及分类见 `source-rendering-differences.json`、`rendering-classification.json`。

26 个文件的双架构回包二进制相同；另外 14 个文件二进制不同，但原生代码数据、源码和资源一致，基线也恰好是同样 14 个文件存在架构间字节差异。见 `architecture-comparison.json`；不把语义一致写成全部字节一致。

## 首轮失败与补测

修改版首轮记录 68 通过 / 12 失败（当时的扫描器只查 `_Lib*`）。基线去掉被隔离的 ID 644 后为 68 通过 / 10 失败，重现相同缺依赖问题。

- ID 60、68、78、79：Ex_DirectUI 示例缺对应的 `0.LibExDui.Helper.ec`、`0.YHan.Helper.ec`，回包产生 `source_preserved_unexamined`。将同版本 Source 目录依赖放到输入副本的 ecom 目录后，修改版和基线双架构全部通过。
- ID 203：`NotificationTest.ec` 缺 `AQGlobalNotificationFne.fne`。使用本机对应项目的 Release 支持库，仅放入输入副本 lib 目录；修改版和基线均通过。依赖来源及哈希见补测 `run.json`。
- ID 120 是额外随机选中的 `ExDui修改YZC/类回调YZC.ec`，两版本、两架构全部通过。

## 尚存问题与限制

1. **ID 107：类型名称未解析。** `Ex_DirectUI4.1/Ex_DirectUI 4.1 [Demo]/1.soliderbar.e` 的 `src/控件类_Ex.txt:461` 导出 `.参数 路径, _Cls_0x160FE, 参考`。基线和修改版均如此；参数类型 ID `0x490160FE` 未改变。原始工程编译同样要求更新 `0.YHan.Eternal.ExFC`、`0.YHan.Helper` 引用，不能用它证明编译通过。目前证据不足以还原正确类名，不能猜成另一匿名类；需要匹配的模块版本或 IDE 更新引用后的副本继续核对。证据：`type-placeholder-107/original-compile.json`。
2. **ID 636、672：被注释代码引用没有声明的局部变量。** 精易模块 v8.4.5 的 `缩略图_创建` 出现 `_Local_0x2B8DB`；黑月界面类的 `创建W` 出现 `_Local_0x1E217`。对应原生表达式仍引用 `0x2502B8DB` / `0x2501E217`，但原生方法声明中没有这两个 ID，不能把它们武断替换成同页其他变量。基线和修改版输出相同；出现在注释中，仍作为导出完整性问题保留。证据：`commented-local-findings.json`。
3. **ID 644：资源被隔离。** `编译/爱琴炫彩的应用模块_Z.ec` 解出的 `image/匿名图片_11.bin` 被 Defender 检测为 `Trojan:Win32/Wacatac.B!ml` 并隔离，事件 1116/1117 见 `defender-events.json`。资源消失导致读取失败；该样本还缺 eAPI 支持库。此为阻塞记录，不据此断言原文件一定恶意，也没有恢复资源或关闭防护。

审计脚本现已扩展扫描 `_Cls_0x*`、`_Local_0x*` 及其他原生命名占位符，避免只查 `_Lib*` 漏报。扫描包含注释，命中不自动等同于可执行代码损坏；应结合声明、原生引用及缺依赖记录调查。上述三个旧问题并未通过样本专用名称替换来掩盖。

`.ec` 修改测试输出的是可编辑 `.e`，不是重新编译生成 `.ec`。本轮没有逐个运行任意抽样程序；ID 107 的原件仅做编译诊断。之前指定的 `指针_读写通用型.e` 已单独编辑、语义回包、编译并运行，读取字节集与中文文本的实测输出符合要求，证据在 `D:/git/AutoLinker/.tmp/variant-roundtrip-20261113-215347/`。

本轮没有新增生产 C++ 修改。之前 Release Win32/x64 与 AutoLinker fne_release/Win32 构建通过；此次仅增强审计扫描和记录报告。没有安装覆盖、提交 Git 或改动原样本。

## 冻结工具 SHA-256

- patched-audit / Win32: `27073b78ce95a2381491a098fee354958e1419c66507536a304d70fa18bad72b`
- patched-audit / x64: `afaf6c0ff7ff692d52e4e85bf5598f3817a0b170ac576b8e7762be07ea1a075d`
- baseline-Win32-audit / Win32: `43566c15481fd1ef264ad912e1ff8877239aadff6ac0a645e67d74ccfad2d9ac`
- baseline-x64-audit / x64: `5209fc312cca3330cb7402b465e9453d3cd7b8189c13ed20b82bc3e4f5616576`

## 抽样清单

| ID | 原文件相对路径 | 有效结果 |
|---|---|---|
| 6 | aero.e | 通过 |
| 12 | AiQinMenu.e | 通过 |
| 14 | AiQinPath2.ec | 通过 |
| 29 | copydata.e | 通过 |
| 32 | curl/zyCurl.e | 通过 |
| 41 | Dock/方案5/方案5.e | 通过 |
| 48 | ekmk.ec | 通过 |
| 60 | Ex_DirectUI4.1/4.1.22.310/Demo/layout/demo.layout.absolute.e | 补齐依赖后通过 |
| 68 | Ex_DirectUI4.1/4.1.22.310/Demo/object/[扩展]navbtn(导航按钮)及选择夹的控制/demo.navbtn.exdui[带缓动效果].e | 补齐依赖后通过 |
| 78 | Ex_DirectUI4.1/4.1.22.310/Demo/object/Label/demo.label.e | 补齐依赖后通过 |
| 79 | Ex_DirectUI4.1/4.1.22.310/Demo/object/Label/demo.label.shadow.e | 补齐依赖后通过 |
| 103 | Ex_DirectUI4.1/Ex_DirectUI 4.1 [Demo]/0.YHan.Eternal.ExFC.ec | 通过 |
| 107 | Ex_DirectUI4.1/Ex_DirectUI 4.1 [Demo]/1.soliderbar.e | 流程完成；既有类型名称问题 |
| 120 | ExDui修改YZC/类回调YZC.ec | 通过（追加随机样本） |
| 203 | NotificationTest.ec | 补齐依赖后通过 |
| 227 | PopWindow.e | 通过 |
| 242 | Sinai界面类_1.2.e | 通过 |
| 283 | try.e | 通过 |
| 322 | wininet.e | 通过 |
| 345 | zyJson1.3test.e | 通过 |
| 375 | 宽文本模块.e | 通过 |
| 381 | 控制台匿名管道.ec | 通过 |
| 397 | 易支持库/wps支持库exek/LWPS测试支持库.e | 通过 |
| 415 | 易支持库/易写易库孤岛版/测试/GDIPlus类2.4.2.ec | 通过 |
| 445 | 易模块反编译/ec/Aero.ec | 通过 |
| 446 | 易模块反编译/ec/AiQin_Browser.ec | 通过 |
| 447 | 易模块反编译/ec/aiqin_http.ec | 通过 |
| 454 | 易模块反编译/ec/AiQinFindColor.ec | 通过 |
| 469 | 易模块反编译/ec/AiQinRegedit.ec | 通过 |
| 480 | 易模块反编译/ec/disasm.ec | 通过 |
| 493 | 易模块反编译/ec/Match.ec | 通过 |
| 496 | 易模块反编译/ec/PELoader144.ec | 通过 |
| 498 | 易模块反编译/ec/QEHashKai.ec | 通过 |
| 563 | 易语言EP助手开发/模块源码/TabControl.ec | 通过 |
| 600 | 环境存取模块.e | 通过 |
| 636 | 精易模块[v8.4.5].ec | 流程完成；既有注释变量问题 |
| 640 | 编译/爱琴炫彩的API操作模块 1.2.ec | 通过 |
| 644 | 编译/爱琴炫彩的应用模块_Z.ec | 资源隔离阻塞 |
| 649 | 验证码.e | 通过 |
| 654 | 鱼刺类_HTTPv5.0_版本20/例子_新手如何使用Winhttp.e | 通过 |
| 672 | 黑月界面类3.8/黑月界面类3.9.6+Masonry.ec | 流程完成；既有注释变量问题 |
