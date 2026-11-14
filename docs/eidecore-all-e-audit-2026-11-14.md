# EideCore 原生源码核对与全部 .e 回归（2026-11-14）

本轮完成全目录审计，结果不是全量通过。新增审计工具和本报告；未修改生产 C++ 实现、未回滚此前改动、未处理 `<对象>`，未提交、部署或改动暂存区。下面明确区分已确认的问题、原生读取边界和格式差异，不能把此次审计理解成这些新问题已被修复。

## 输入与基准

- 原目录：`C:/Users/aiqin/OneDrive/Code/模块`。完整依赖树复制到证据目录中操作。
- 实际为 489 个 `.e` 文件，其中 `draw_menu.e` 为零字节，按规则跳过；488 个非空输入 × Win32/x64 = 976 条流水线。没有随机取样、替换失败文件或筛除旧备份。
- 按最后扩展名选择 `.e`，包括目录中的 `.ec.e` 文件；`.ec` 不作为测试输入，但保留为模块依赖。另有一个名为 `功能集合.e` 的目录，不是文件，未计入。
- AutoLinker / EideCore 源码提交：`8e81b4d`（新增源码读取 API 与底层实现），重新构建后冻结使用生产 `source` 接口，不使用旧安装目录中的工具。
- e-packager 基于 `106883e` 加本轮开始时已暂存的原生身份改动，测试前重新构建并冻结两个架构。不能只用 HEAD 哈希代表这个工作区版本，具体二进制 SHA-256 见 `detailed-summary.json`。
- 证据根：`E:/temp/eidecore-all-e-20261114`。原文件 489 项（含空文件）在结束时重新核对 SHA-256，全部未改变。

## 测试流程

1. 对每个原输入运行 EideCore `source --project ... --e-dir ...`，检查加载、原生代码完整性、自校验、异常和 UI 拦截状态。
2. 两个架构分别拆包；无修改回包并比较完整文件字节和哈希。
3. 每个程序集／类代码页尾追加唯一注释，保留现有字面量字节；进行语义回包。
4. 把回包文件暂存到输入副本旁，再拆包，保留相同的依赖查找上下文；比较源码、窗口 XML、资源和注释，随后再次回包。
5. 用 EideCore 独立读取回包文件，分别比较“原生原文件 vs 首次导出”“原生回包文件 vs 再次导出”“原生原文件 + 预期注释 vs 原生回包文件”。按原生 ID 映射声明，不只按名称。
6. 额外按字节核对 `src/`、`text/`、`image/`、`audio/`，资源索引按 JSON 结构比较；对原生表达式做辅助结构与字面量检查。

并发 8 个输入，每条命令限时 120 秒。每个架构独立目录，保留命令、退出码、耗时、原始日志、JSON 和 diff。未运行这些样本生成的程序；这不是 488 个程序的编译运行认证。此前通用型专项的编译运行结果不被本报告替代。

## 全量结果

| 检查项 | Win32 | x64 |
| --- | ---: | ---: |
| 实际输入 | 488 | 488 |
| 首次拆包成功 | 488 | 488 |
| 无修改回包字节一致 | 488 | 488 |
| 编辑后语义回包命令成功 | 442 | 442 |
| 再次拆包成功 | 442 | 442 |
| 第二次回包成功且字节与第一次编辑回包一致 | 442 | 442 |
| 原有严格回归检查无问题记录 | 338 | 338 |
| 原文件 EideCore 完整源码可用 | 456 | 456 |
| 回包文件 EideCore 完整源码可用 | 412 | 412 |

两个架构的上述计数以及逐文件问题类别一致。`命令成功`不等于源码、资源或语义已经全部验证。每个架构有 46 个语义回包失败输入；338 个无旧回归问题记录的输入也不等于其文本与原生 IDE 显示完全一致。

| 独立源码比较（每架构） | 可比较文件数 | 全部本地项一致 | 比较声明／页数 | 一致声明／页数 |
| --- | ---: | ---: | ---: | ---: |
| 原文件原生源码 vs 首次导出 | 456 | 69 | 125794 | 77542 |
| 回包原生源码 vs 再次导出 | 412 | 76 | 67228 | 35913 |
| 原生原文件 + 注释 vs 原生回包文件 | 407 | 295 | 65378 | 64382 |

比较仅排除版本／支持库头、字符串外的空行和行尾排版；不排除声明缩进、内部空白、注释内容、数字拼写、转义前缀或身份差异。125,794 项是所有受测文件的页和声明合计，重复工程副本也分别计数。大量差异来自 DLL 参数／类型成员缩进，不能把全部差异都解释为执行逻辑损坏。

原生前后比较的 295 个本地项一致输入中，有 10 个新增可见项；要求没有新增项则为 285 个。综合旧回归检查和三组独立文本比较，严格全通过为每架构 58 个，共 116 条流水线。这个口径针对当前显示一致性要求，未用放宽比较规则提高通过率。

成功完成再拆包的每架构 442 个输入中，421 个 `src/text/image/audio` 内容完全一致；21 个有 `src/` 字节差异（39 个文件）。长文本载荷、图片、音频及相应索引未发现差异。源码差异仍保留，包含空流程补行、原未检查文本被规范化、备注空白变化，以及下述真实声明问题。

## 已确认的重要发现

### 1. 常量表仍泄漏内部转义格式

`0270 xml/xmltree1.50.e` 的 `XML头`：EideCore 输出原始 XML 字符串，而 `.常量.txt` 输出 `#e2txt_text#` 和 `\x22`。原生回包读取仍得到原值，因此当前证据指向显示边界遗漏，不是该样本的值损坏。

代码路径：`BuildConstantPage` 使用内部 `BuildDumpTextLiteral`；目录写出只对长文本占位进行转换。程序正文已有显示／内部表示分离，普通常量表没有完整覆盖。正确方向是在常量声明的值字段做同样的语法感知转换，并保留真实同形前缀；不可全局删除 `#e2txt_text#`。证据：`cases/0270/Win32/original-parity.diff`、`native-roundtrip.comparison.json`。

### 2. 常量备注被误判为旧格式公开标志

`0391 易语言EP助手开发/模块源码/SK.e`：

```text
原生原文件：.常量 TRUE, "1", , 公开,
原生回包后：.常量 TRUE, "1", 公开
```

原文件第四列的“公开,”是备注；`ParseConstantPage` 在第三列为空、第四列等于“公开”时猜测为旧五列格式，改写了公开属性并消耗备注。469 个常量同时换了 ID。同名常量仍存在，不能称为丢失，但属性／备注和身份发生了非预期变化。

这不是文件特例，而是按内容猜旧格式与合法备注冲突。应由已知格式版本或显式导入方言决定旧格式分支；当前格式的备注尾部应整体保留。证据：`constant-field-shift.json`、`native-identity-remaps.json`、`cases/0391/Win32/content.diff.txt`。

### 3. 导入 DLL 参数 ID 与本地参数冲突，扁平名称缓存覆盖名称

`0013 AppCompatFlags.e` 仅加注释后，再拆包出现 `hwnd → 目标`、`content → 源`、另一个 `hwnd → 长度`。EideCore 回读原生声明仍为原名称，54 个原生本地项在预期注释后全部一致。

原本分别只出现一次的参数 ID `1157694476/1157694477/1157694478`，回包后分别出现两次，另一组属于导入的 `lstrcpyn` 参数。`DependencyImportIdCursor` 的子 ID 范围没有覆盖全部本地身份保护，`BuildUserNameCache` 又把不同声明的子项放进同一个 ID→名称表。应同时建立完整的 ID 保留规则，并按所属声明处理局部名称；仅替换这三个名字不能解决问题。证据：`parameter-id-collision.json` 和 `cases/0013/Win32/`。

### 4. 只追加注释也会触发原生身份歧义

`0096 Ex_DirectUI4.1/ExDui修改YZC.e` 和 `0100` 同源副本，在 `组件基类_Ex.锁定` 报 `ambiguous_native_identity`。原始两个停用条件分别引用 `_Local_0x26683` 与 `_Local_0x26684`，原生显示都变成同一条空名称条件。

当前恢复器按方法中的显示行匹配候选；即使该方法未修改，整页追加注释也会进入逐行匹配并遇到两个候选。拒绝猜 ID 是必要保护，但缺少对未改变方法／结构区间的可靠对应关系。应改善结构化编辑映射，仍对真正无法判定的重构拒绝；不能取第一个候选，也不能按 `_Local_` 名字做豁免。

### 5. 空局部声明被写成可见的匿名名称

例如 `0012 APIHookEx.e`、`0035 dotNet4Version.e`，原生源码不显示空名称占位行，导出器为其生成 `匿名局部变量_<ID>`，语义回包后这个名字成为实际声明。

该无条件兜底来自 `9a439772`（2026-10-08，提升语义回包准确性与自动化审计能力），目的与为语义重建提供可引用身份相符；不能据此断言作者更细的原始动机。实际问题是内部身份名称流入了显示层和写回名称字段。应分离原始名称、内部身份和显示文本，并按原生声明规则处理空占位。

### 6. 其他显示与身份差异

- DLL 参数及数据类型成员：导出缩进四格，原生复制文本无该缩进。
- 整数除法：原生 `＼`，当前导出 `\`；数字存在科学计数法与展开形式差异。已举例的数字展示差异不能直接当作数值变化。
- 声明备注前导空白、空备注末尾逗号会被裁剪；多行备注尾部空白也存在再拆包变化。
- `0093 1.soliderbar.e` 等依赖基类接口，原文件中属于模块依赖的页在语义回包后成为本地可见页，应核对导入属性，而不是忽略新增页。
- 空流程体补空行属于此前为原生编辑器稳定性引入的规则；严格字节比较会记录差异，不能据此认定逻辑变化。

这些发现需要从声明格式、身份作用域与元数据保留规则调整，不能以降低测试标准、替换样本或文件名特判掩盖。`<对象>` 保持既有规则。

## 原生基准范围和失败边界

原始 488 个输入中，456 个能取得完整原生源码；32 个不可用：17 个依赖缺失、6 个原生异常、9 个窗口创建被无头策略拒绝。它们仍完成了双架构拆回包尝试，未被删除出分母，也未被计为原生源码通过。`ImageHash.e` 的目录读取单独复查仍在 load 阶段异常，不能归因于源码比较器。

EideCore 只返回本地定义，不覆盖窗口布局、图片／音频、长文本实际载荷及依赖实现。首次导出比原生目录多出的每架构 6,559 项，6,546 项为原生 API 省略的空名称占位行，13 项能用依赖 `definedIds` 确认来源；没有未分类额外项。详见 `source-scope.json`。

原生 API 统一文本换行，其结果不能代替字面量／资源字节验证。辅助原生 AST 比较保留了完整差异：导入 ID 重映射、真值编码 `FFFF/0100`、原未检查文本被解析等都可能造成原始结构不同，因此没有把所有 AST 差异直接称为语义错误，也没有将解析器不支持的项算作通过。

## 失败类别（两架构合计，可重叠）

| 类别 | 次数 |
| --- | ---: |
| `content_changed` | 42 |
| `semantic_pack_unexamined_statements` | 198 |
| `semantic_pack_error` | 92 |
| `unpack_unresolved_tokens` | 30 |
| `reunpack_unresolved_tokens` | 30 |
| `unpack_property_decode_failed` | 8 |
| `semantic_pack_property_decode_failed` | 8 |
| `reunpack_property_decode_failed` | 8 |

`source_preserved_unexamined` 包含原文件本来就没有检查通过的文本，不能简单等同于新增回归，也不能据此声称完整语义编码成功。占位名称扫描也是待核查提示，可能受缺失支持库影响；最终逐文件事实见 CSV 和原始日志。

## 46 个语义回包失败输入

两个架构的失败集合一致。这里只列主错误，完整警告保存在各自日志。

| ID | 文件 | 主错误 |
| ---: | --- | --- |
| 0085 | `Ex_DirectUI4.1/4.1.22.310/Demo/window/demo.multiwindow.e` | pack failed: semantic_method_rebuild_failed: 程序集1.OnExWndProc (src/程序集1.txt:210) => unsupported_expression: m_hWndB (access_base_variable_not_found: m_hWndB) |
| 0094 | `Ex_DirectUI4.1/Ex_DirectUI 4.1 [Demo]/libexdui.e` | pack failed: semantic_method_rebuild_failed: 程序集1.Ex_Init (src/程序集1.txt:589) => function_not_found: GlobalGetAtomName |
| 0096 | `Ex_DirectUI4.1/ExDui修改YZC.e` | pack failed: src/B系统组件/组件基类_Ex.txt:ambiguous_native_identity:锁定 |
| 0097 | `Ex_DirectUI4.1/libexdui.e` | pack failed: semantic_method_rebuild_failed: 程序集1.Ex_Init (src/程序集1.txt:589) => function_not_found: GlobalGetAtomName |
| 0100 | `ExDui修改YZC/ExDui修改YZC.e` | pack failed: src/B系统组件/组件基类_Ex.txt:ambiguous_native_identity:锁定 |
| 0102 | `file.e` | pack failed: semantic_method_rebuild_failed: 程序集1.文件_枚举EX (src/程序集1.txt:58) => function_not_found: 文件_取扩展名 |
| 0164 | `LuaList（暂时废弃）.e` | pack failed: semantic_method_rebuild_failed: Class_LuaList.UnSet (src/Class_LuaList.txt:419) => function_not_found: GetWindowLongA |
| 0170 | `opencvimage-c.e` | pack failed: semantic_method_rebuild_failed: OpenCVMap.IsMapPointDirectConnectLinkCN (src/OpenCVMap.txt:113) => unsupported_expression: k (access_base_variable_not_found: k) |
| 0217 | `TEXT(2010.8.2).e` | pack failed: semantic_method_rebuild_failed: 核心.核心_等待句柄 (src/核心.txt:138) => unsupported_expression: 完成操作 (access_base_variable_not_found: 完成操作) |
| 0238 | `UI_20150416_改（暂时废弃）.e` | pack failed: semantic_method_rebuild_failed: Class_UI_List.UnSet (src/Class_UI_List.txt:222) => unsupported_expression: m_shell_code (access_base_variable_not_found: m_shell_code) |
| 0239 | `ui_text - 副本.e` | pack failed: semantic_method_rebuild_failed: Class_UI_Text.WinMsgProc (src/Class_UI_Text.txt:35) => access_base_variable_not_found: m_btn |
| 0240 | `ui_text.e` | pack failed: semantic_method_rebuild_failed: Class_UI_Text.WinMsgProc (src/Class_UI_Text.txt:35) => access_base_variable_not_found: m_btn |
| 0246 | `unicode支持库/支持库-controls_W - 静态.e` | pack failed: semantic_method_rebuild_failed: _库实现_组件_Textbox_w.controls_w_Textbox_w_属性被修改 (src/_库实现_组件_Textbox_w.txt:137) => access_member_not_found: 图片 ownerType=1090617469 |
| 0247 | `unicode支持库/支持库-controls_W.e` | pack failed: semantic_method_rebuild_failed: _库实现_组件_Textbox_w.controls_w_Textbox_w_属性被修改 (src/_库实现_组件_Textbox_w.txt:137) => access_member_not_found: 图片 ownerType=1090592488 |
| 0285 | `zyJson1.7.e` | pack failed: semantic_method_rebuild_failed: 程序集_Array._Array_ShellSort (src/程序集_Array.txt:528) => function_not_found: Data_GetValuePoint |
| 0286 | `zyJson4.1.1.e` | pack failed: semantic_method_rebuild_failed: 程序集_启动._临时子程序 (src/程序集_启动.txt:34) => access_base_variable_not_found: cust |
| 0287 | `zyJson4.1.3.e` | pack failed: semantic_method_rebuild_failed: 程序集_启动._临时子程序 (src/程序集_启动.txt:34) => access_base_variable_not_found: cust |
| 0289 | `zyXML.e` | pack failed: semantic_method_rebuild_failed: 程序集_XMLCore.XmlNode_ClearAttribute (src/程序集_XMLCore.txt:1295) => access_base_variable_not_found: xn |
| 0292 | `内存读写模块.e` | pack failed: semantic_method_rebuild_failed: 内存读写类.读文本型 (src/内存读写类.txt:953) => unsupported_expression: 长度 (access_base_variable_not_found: 长度) |
| 0316 | `支持库-controls_W - 静态.e` | pack failed: semantic_method_rebuild_failed: _库实现_组件_Textbox_w.controls_w_Textbox_w_属性被修改 (src/_库实现_组件_Textbox_w.txt:137) => access_member_not_found: 图片 ownerType=1090617469 |
| 0317 | `支持库-controls_W.e` | pack failed: semantic_method_rebuild_failed: _库实现_组件_Textbox_w.controls_w_Textbox_w_属性被修改 (src/_库实现_组件_Textbox_w.txt:137) => access_member_not_found: 图片 ownerType=1090592488 |
| 0371 | `易模块反编译/模块反编译/模块分析.e` | pack failed: semantic_method_rebuild_failed: 程序集1.模块_分析文件 (src/程序集1.txt:43) => access_member_not_found: 程序集 ownerType=1090596103 |
| 0379 | `易语言EP助手开发/EPlus更新.e` | pack failed: semantic_method_rebuild_failed: Hook.My_TrackPopupMenuEx (src/Hook.txt:125) => unsupported_expression: g_bOneKeyCompile (access_base_variable_not_found: g_bOneKeyCompile) |
| 0380 | `易语言EP助手开发/IDETools.e` | pack failed: semantic_method_rebuild_failed: ExAutoVar.InsertVarThreadProc (src/ExAutoVar.txt:288) => function_not_found: ShowWindowAsync |
| 0396 | `易语言EP助手开发/版本备份/IDETools 2015-10-24.e` | pack failed: semantic_method_rebuild_failed: ExtFunction.fneEnKeyWords (src/ExtFunction.txt:94) => function_not_found: IsAllForChinese |
| 0401 | `易语言EP助手开发/版本备份/IDETools 2015-11-05.e` | pack failed: semantic_method_rebuild_failed: ExAutoVar.InsertVarThreadProc (src/ExAutoVar.txt:287) => function_not_found: ShowWindowAsync |
| 0402 | `易语言EP助手开发/版本备份/IDETools 2015-11-10.e` | pack failed: semantic_method_rebuild_failed: ExAutoVar.InsertVarThreadProc (src/ExAutoVar.txt:287) => function_not_found: ShowWindowAsync |
| 0403 | `易语言EP助手开发/版本备份/IDETools 2015-11-16.e` | pack failed: semantic_method_rebuild_failed: ExAutoVar.InsertVarThreadProc (src/ExAutoVar.txt:287) => function_not_found: ShowWindowAsync |
| 0404 | `易语言EP助手开发/版本备份/IDETools 2015-11-20.e` | pack failed: semantic_method_rebuild_failed: ExAutoVar.InsertVarThreadProc (src/ExAutoVar.txt:287) => function_not_found: ShowWindowAsync |
| 0405 | `易语言EP助手开发/版本备份/IDETools 2015-11-28.e` | pack failed: semantic_method_rebuild_failed: ExAutoVar.InsertVarThreadProc (src/ExAutoVar.txt:287) => function_not_found: ShowWindowAsync |
| 0406 | `易语言EP助手开发/版本备份/IDETools 2016-01-06.e` | pack failed: semantic_method_rebuild_failed: ExAutoVar.InsertVarThreadProc (src/ExAutoVar.txt:287) => function_not_found: ShowWindowAsync |
| 0407 | `易语言EP助手开发/版本备份/IDETools 2016-01-07.e` | pack failed: semantic_method_rebuild_failed: ExAutoVar.InsertVarThreadProc (src/ExAutoVar.txt:288) => function_not_found: ShowWindowAsync |
| 0408 | `易语言EP助手开发/版本备份/IDETools 2016-01-09.e` | pack failed: semantic_method_rebuild_failed: ExAutoVar.InsertVarThreadProc (src/ExAutoVar.txt:288) => function_not_found: ShowWindowAsync |
| 0409 | `易语言EP助手开发/版本备份/IDETools 2016-01-11.e` | pack failed: semantic_method_rebuild_failed: Hook.My_TrackPopupMenuEx (src/Hook.txt:125) => unsupported_expression: g_bOneKeyCompile (access_base_variable_not_found: g_bOneKeyCompile) |
| 0410 | `易语言EP助手开发/版本备份/IDETools 2016-01-18.e` | pack failed: semantic_method_rebuild_failed: Hook.My_TrackPopupMenuEx (src/Hook.txt:125) => unsupported_expression: g_bOneKeyCompile (access_base_variable_not_found: g_bOneKeyCompile) |
| 0428 | `爱琴炫彩的综合模块.e` | pack failed: semantic_method_rebuild_failed: 拨号类.判断拨号连接状态 (src/拨号类.txt:184) => constant_not_found: INTERNET_CONNECTION_MODEM |
| 0454 | `精易/精易模块v11.1.0[源码].e` | pack failed: semantic_method_rebuild_failed: 类_桌面窗口管理.缩略图_创建 (src/集_系统/类_桌面窗口管理.txt:18) => unsupported_expression: 错误代码 (access_base_variable_not_found: 错误代码) |
| 0455 | `精易/精易模块v11.1.5[源码].e` | pack failed: semantic_method_rebuild_failed: 类_桌面窗口管理.缩略图_创建 (src/集_系统/类_桌面窗口管理.txt:18) => unsupported_expression: 错误代码 (access_base_variable_not_found: 错误代码) |
| 0456 | `精易/精易模块v8.4.5[源码].e` | pack failed: semantic_method_rebuild_failed: 类_桌面窗口管理.缩略图_创建 (src/集_系统/类_桌面窗口管理.txt:18) => unsupported_expression: 错误代码 (access_base_variable_not_found: 错误代码) |
| 0457 | `精易/精易模块v9.3.5[源码].e` | pack failed: semantic_method_rebuild_failed: 类_桌面窗口管理.缩略图_创建 (src/集_系统/类_桌面窗口管理.txt:18) => unsupported_expression: 错误代码 (access_base_variable_not_found: 错误代码) |
| 0484 | `黑月界面类3.8/黑月界面类3.8.e` | pack failed: semantic_method_rebuild_failed: 窗口类.创建W (src/窗口类.txt:24) => unsupported_expression: parent (access_base_variable_not_found: parent) |
| 0485 | `黑月界面类3.8/黑月界面类3.9.6-Masonry约束布局-beta.1.e` | pack failed: semantic_method_rebuild_failed: 窗口类.创建W (src/窗口类.txt:24) => unsupported_expression: parent (access_base_variable_not_found: parent) |
| 0486 | `黑月界面类3.8/黑月界面类3.9.6-Masonry约束布局-beta.3.e` | pack failed: semantic_method_rebuild_failed: 窗口类.创建W (src/窗口类.txt:24) => unsupported_expression: parent (access_base_variable_not_found: parent) |
| 0487 | `黑月界面类3.8/黑月界面类3.9.6-old.e` | pack failed: semantic_method_rebuild_failed: 窗口类.创建W (src/窗口类.txt:24) => unsupported_expression: parent (access_base_variable_not_found: parent) |
| 0488 | `黑月界面类3.8/黑月界面类3.9.6.e` | pack failed: semantic_method_rebuild_failed: 窗口类.创建W (src/窗口类.txt:24) => unsupported_expression: parent (access_base_variable_not_found: parent) |
| 0489 | `黑月界面类3.9.6.e` | pack failed: semantic_method_rebuild_failed: 窗口类.创建W (src/窗口类.txt:24) => unsupported_expression: parent (access_base_variable_not_found: parent) |

## 构建与可复核产物

- e-packager `Release / Win32`、`Release / x64` 构建通过。
- AutoLinker `fne_release / Win32` 构建通过，0 警告、0 错误。
- EideCore 由当前源码重新构建，使用冻结目录 `bin/EideCore`。
- `manifest.json`：489 个原文件的路径、大小、SHA-256。
- `all-e-results.csv`：976 条流水线的结果、覆盖状态和证据目录。
- `source-audit-summary.json`、`detailed-summary.json`：总计、分架构和比较分母。
- `raw-content-integrity.json`：源码／长文本／资源逐字节结果。
- `source-scope.json`、`native-identity-remaps.json`：额外声明来源与原生 ID 变化。
- `native-ast-summary.json`、`native-literal-summary.json`：辅助原生结构／字面量检查，保留不支持项。
- `build-{Win32,x64,eidecore,autolinker}.log`：构建证据。
- `native/NNNN/original-source.json`、`cases/NNNN/{Win32,x64}/source-audit.json` 及相邻 diff／日志：逐输入细节。

审计脚本：`tools/TestEideCoreSourceAudit.py`、`tools/SummarizeEideCoreSourceAudit.py`。前者沿用既有 `TestModuleRoundtripAudit.py` 和 `TestIdePageParity.normalize`，不引入第三方包。

运行前准备独立输出目录：`inputs/` 为完整依赖树副本，`manifest.json` 为实际 `.e` 文件清单，冻结可执行文件分别位于 `bin/EideCore/EideCore.exe`、`bin/Win32/Release/e-packager.exe`、`bin/x64/Release/e-packager.exe`。随后执行：

```powershell
python tools/TestEideCoreSourceAudit.py --output E:/temp/eidecore-all-e-20261114 --e-dir C:/Users/aiqin/OneDrive/e5.6 --workers 8
python tools/SummarizeEideCoreSourceAudit.py E:/temp/eidecore-all-e-20261114
```

已有完整 `source-audit.json` 的架构结果会复用；更换生产二进制后应新建证据根并全量重跑，不能沿用旧结果。
