# 模块目录拆包／语义回包审计脚本

`TestModuleRoundtripAudit.py` 仅使用 Python 3.9+ 标准库，在 Windows 上运行。先构建 Release Win32、Release x64；窗口属性桥接需要 Win32 程序：可放置同目录 `e-packager-x86.exe`，或保持项目现有 `bin/Win32/Release` 与 `bin/x64/Release` 布局。

```powershell
py -3 -X utf8 tools/TestModuleRoundtripAudit.py `
  --source 'C:\Users\aiqin\OneDrive\Code\模块' `
  --output 'D:\test\module-roundtrip-new' `
  --workers 4 --timeout 90
```

输出目录必须不存在，且不能位于输入目录内。脚本复制整棵输入目录以保留相对依赖，默认只测试递归找到的 `.e`；传入 `--extensions .e .ec` 可同时测试 `.ec`。编号按所选扩展名的全部文件排序，因此不同扩展名选择的编号不可混用。每个架构使用独立工作区。零字节文件记录为跳过；其他拆包失败、缺依赖、崩溃、超时均保留，不以跳过隐藏。

每个样本执行：

1. 对原文件记录大小、MD5 和 SHA256，复制后校验一致。
2. 使用默认 `unpack` 拆包（导出依赖），然后 `pack`，对无修改产物检查逐字节一致；原始 `.ec` 对应输出 `.ec`。
3. 找到主工程 `src/` 中包含 `.程序集` 或 `.类` 声明的每个文本文件，在底部追加一行唯一注释，保持 UTF-8 BOM、CRLF。不编辑依赖模块、不删除原生快照。
4. 修改后再次 `pack` 为可编辑 `.e`，确认产物非空且字节变化；将产物放在输入副本旁的唯一临时文件中，用同架构工具 `unpack --main-only`，完成后删除该临时文件。这样保留工程旁支持库的解析上下文，避免把依赖环境变化误报为回包损坏。
5. 检查每条注释仍在对应文件，比较全部源码／窗口内容、图片／音频索引和二进制数据。文本比较仅忽略空行及行首尾空白，不忽略注释或语句。
6. 将再次拆出的目录回包为 `second.e`，记录能否继续回包。最后重新读取全部受测原文件，检查大小和两个哈希均未变化，并复核工具二进制没有中途变化。

首次和再次拆包都扫描主工程 `src/` 下文本及 XML 中的原生符号占位符，包括 `_Lib*`、`_Cls_0x*`、`_Sub_0x*`、`_User_0x*` 等，记录文件、原始行号、符号和整行内容。即使命令返回 0，发现占位符或 `control_property_decode_failed` 也列为问题。

页尾修改使工程摘要失效，当前回包器保留声明身份并禁止复用旧执行代码，进行语义重建；脚本本身不手动删除快照。空程序集中的页尾注释是否能保留也纳入检查。无类／程序集可修改的样本单独标记，不冒充语义回包通过。

每条命令独立超时，标准输入关闭。Windows 崩溃弹窗被禁用；超时终止本次命令的进程树。原始日志、退出码（含十六进制）、命令、耗时、修改页清单、依赖和源码差异保存在 `cases/<ID>/<架构>/`。脚本不启动 IDE、不执行编译或样本程序。

语义回包若出现 `source_preserved_unexamined` 警告，即使退出码为 0 也记为失败，防止将未检查语句保存误报为完整语义重建。

缺失支持库可重复传入 `--support-library <文件.fne>`；脚本仅将库放进输入副本的 `lib/`，检查同名冲突，并在 `run.json` 保存来源和哈希。不修改原工程或 IDE 安装。

审计结果：

- `manifest.json`：编号、相对路径、原文件哈希。
- `run.json`：时间、Git 提交、脚本与工具哈希。
- `progress.jsonl`：逐例完成记录，运行中即可查看。
- `results.json` / `summary.json` / `report.md`：详细结果、汇总和 Markdown 问题清单。
- `content.diff.txt`：每个差异文本文件最多保留 160 行 unified diff；全部差异文件名记录在 `result.json`。

脚本完成发现问题返回 1；这不意味着脚本没有完成测试。`unpack_unresolved_tokens` / `reunpack_unresolved_tokens` 表示发现原生符号占位符，需要结合依赖日志判断是否缺库或缺失类型名称，并排除用户自定义的同前缀名称。`*_property_decode_failed` 表示控件属性未能完整读取。源码不一致也需查看差异，区分语义丢失、注释丢失、路径变化和资源索引变化。

定向复现（编号取决于完整目录排序，应先核对 `manifest.json`）：

```powershell
py -3 -X utf8 tools/TestModuleRoundtripAudit.py `
  --output 'D:\test\module-roundtrip-repro-new' `
  --ids 3,4 --architectures Win32 x64 --workers 1 --timeout 180
```

对早期将产物移出工程目录再拆包的完整审计，可以使用 `--recheck-decode-context --output <已有审计目录>` 补验全部回包成功样本。工具二进制 SHA256 必须与首次运行一致；首次比较保存在 `initial_decode`，新结果与差异放在 `decoded-context/`。原始 `progress.jsonl` 不重写，最终结论读取 `results.json` 和 `summary.json`。该补验只运行一次，再次复测应使用新的输出目录。

`.ec` 的修改测试是恢复可编辑 `.e`，不是重新生成编译后的 `.ec`；后者须另行使用 IDE 编译。原样 `.ec` 回包仍要求字节完全相同。
