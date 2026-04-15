# e-packager （测试中）

将易语言 `.e` 工程文件解包为可读目录，或将目录回包为 `.e` 文件。  
让易语言项目享有 Git 版本管理、代码 Diff、AI 辅助编辑等现代开发体验。

如果有拆不了的或者拆完封不起来的，请提交lssues，把文件放上来。

## 使用

### 解包

```
e-packager unpack <input.e> <output-dir>
```

或直接将 `.e` 文件拖放到 `e-packager.exe` 上，自动在 `.e` 文件所在目录创建同名子目录并解包：

```
e-packager MyApp.e   # 解包到 MyApp\
```

将 `.e` 文件解包到指定目录。解包后的目录结构如下：

| 路径 | 内容 |
| --- | --- |
| `src/` | 源码文件（`.txt`）及窗口界面定义（`.xml`） |
| `project/` | 封包所需元数据与原生快照，请勿删除 |
| `image/` | 图片资源 |
| `audio/` | 音频资源 |
| `tool/e-packager.exe` | 随目录自带的封包工具 |
| `info.json` | 来源 `.e` 文件的文件名、路径、修改时间、MD5 |
| `AGENTS.md` | 供 AI Agent 阅读的项目结构说明 |

### 回包

```
e-packager pack <input-dir> <output.e>
```

或在项目根目录（或 `tool/` 子目录）内直接运行，自动输出到 `pack/` 目录：

```
e-packager
```

### 其他命令

```
e-packager compare-bundle <input.e> <input-dir>   # 比较 .e 文件与目录内容是否一致
e-packager roundtrip <input.e> <work-dir> <output.e>        # 解包后立即回包
e-packager verify-roundtrip <input.e> <work-dir> <output.e> # 往返并校验内容一致性
```

## 注意

本工具的解包与回包功能尚不完善，存在数据损坏的可能性。**使用前请备份源文件，作者不对可能的损失负任何责任。**

## 致谢

本项目依赖以下开源项目：

- [OpenEpl/TextECode](https://github.com/OpenEpl/TextECode) — 易语言工程文件与文本代码互转
- [OpenEpl/EProjectFile](https://github.com/OpenEpl/EProjectFile) — 易语言项目文件读写库
