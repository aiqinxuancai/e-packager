# samples 语义回包审计

`TestSamplesSemanticAudit.py` 在独立目录复制样例，再执行拆包、编辑全部源码/窗口页、Win32 与 x64 语义回包、重新拆包比较以及 AutoLinker 无头编译。不会运行生成的样例程序，也不会修改原始 `.e`。

先构建 Release Win32，再构建 Release x64；x64 目录需要配套的最新 `e-packager-x86.exe`，用于读取 32 位支持库控件属性。

```powershell
python -X utf8 tools/TestSamplesSemanticAudit.py `
  --output temp/samples-audit-new `
  --ide "C:/Users/aiqin/OneDrive/e5.6/e5.95.exe" `
  --launcher "D:/git/AutoLinker/bin/fne_release/AutoLinkerTest.exe" `
  --workers 2 --timeout 120 --blackmoon-ids 322
```

- 输出目录必须不存在；结果见 `report.md`、`results.json`、`summary.json` 和编号目录的完整日志。
- `.npk`、确实缺失的支持库和空文件跳过，其余错误保留为失败。缺少 `.ec` 不冒充缺少支持库。
- 每个子程序插入注释，固定声明页和窗口 XML 加空行，保证修改后走源码语义重建。重新拆包比较会忽略源码空白行及行首尾空白；图片、音频及其他二进制资源逐字节检查。
- 原例和回包后的工程都编译。在输入副本原位临时放入回包文件，编译后恢复，以保留编译插件要求的工程文件名；产物放在输入副本旁以保留链接器的相对库路径，并复制到审计目录归档。需要静态编译时自动重试静态模式；黑月工程可用 `--blackmoon-ids` 显式指定汇编模式。
- 同时校验退出码、JSON 状态、产物存在且非空和链接错误日志，避免启动器返回成功但链接实际失败。
- `--ids 35,37` 按当前完整排序后的样例编号定向复测；`--bin-root` 可指定隔离构建目录。
- 已有目录可加 `--retry-compile-failures --timeout 120` 串行重试编译；`--ids` 也适用于重试。首次编译结果保存在 `initial_compile` 中。
- `--merge-audits <旧目录> <新目录>` 合并结果，同编号以最后一个目录为准，保留真实日志路径和来源。合并结果代表不同批次，不应描述为同一二进制的一轮测试。

编号取决于完整样例列表；使用 `--blackmoon-ids` 前应核对 `manifest.json`/`results.json`。此批样例的 322 是“黑月例程/调试静态库/调试静态库测试.e”。
