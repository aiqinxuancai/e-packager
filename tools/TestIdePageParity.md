# IDE 真实页与拆包代码对照

`TestIdePageParity.py` 使用 AutoLinker MCP 的 `read_real_file`，不以 e-packager 镜像文本作为预期结果。镜像仅用于枚举路径和映射真实程序项。应先在已安装 AutoLinker 的易语言 IDE 中打开输入文件的测试副本，再运行：

```powershell
python -X utf8 tools/TestIdePageParity.py `
  --source 'E:/temp/test/input.e' `
  --packager bin/Win32/Release/e-packager.exe `
  --e-dir 'C:/Users/aiqin/OneDrive/e5.6' `
  --output 'E:/temp/test/ide-parity-first'
```

多实例时可传 `--instance-id`，以 MCP `list_instances` 返回的 ID 为准。脚本核对目标工程路径，拒绝从其他工程读取基准；不编辑真实页、不保存工程、不关闭 IDE。

`oracle.json` 保存输入 SHA-256、IDE 进程信息、每个真实页的完整文本、哈希和原生读取 trace。`responses/` 保留分页原始响应，校验连续行号和稳定代码哈希；不接受镜像来源。`unpacked/` 是待测工具直接从同一 `.e` 拆出的独立目录。

可对相同输入使用保存的真实页基准重复验证不同构建，无需再次启动 IDE：

```powershell
python -X utf8 tools/TestIdePageParity.py `
  --source 'E:/temp/test/input.e' `
  --oracle 'E:/temp/test/ide-parity-first/oracle.json' `
  --packager bin/x64/Release/e-packager.exe `
  --e-dir 'C:/Users/aiqin/OneDrive/e5.6' `
  --output 'E:/temp/test/ide-parity-x64'
```

输出目录必须不存在。离线比较要求输入 SHA-256 与基准一致；原文件前后重新校验。差异写入 `diff/`，结果写入 `summary.json`。发现任何差异退出 1，不将尚未解决的差异列为预期通过。

比较保留语句内部空格、数组格式、限定名、声明字段和文本字面量。仅忽略字符串外的空行/行尾空白以及 `.版本 2`、`.支持库` 页头：`read_real_file` 本身会补版本头、略去支持库头，空固定表也会返回版本头。字符串中的空白和换行不按排版删掉。该接口的固定表读取可能规范化常量显示，trace 中会注明；本工具不是未经处理的系统剪贴板字节对比。

真实页一致是独立于语义回包的检查。修改拆包器后仍须执行双架构语义回包测试、检查原生调用/类型身份，并在需要时用原生编译器编译运行；两种检查不能相互代替。
