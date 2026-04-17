# e-packager

实验项目，将易语言 `.e` 工程文件解包为可读目录并回包为 `.e`，也支持将 `.ec` 模块文件解包为可编辑目录后回包为 `.e`。
让易语言项目享有 Git 版本管理、代码 Diff、AI 辅助编辑等现代开发体验。

> 📖 [《易语言 × AI Agent 实践白皮书》](https://github.com/aiqinxuancai/Awesome-E-Agent)

如果有拆不了的或者拆完封不起来的，请提交lssues把文件放上来，**易包估计不支持，因为我不用，也没测过**。

## 使用

### 解包

```
e-packager unpack <input.e|input.ec> <output-dir>
```

或直接将 `.e` / `.ec` 文件拖放到 `e-packager.exe` 上，自动在源文件所在目录创建同名子目录并解包：

```
e-packager MyApp.e    # 解包到 MyApp\
e-packager MyMod.ec   # 解包到 MyMod\
```

将 `.e` / `.ec` 文件解包到指定目录。解包后的目录结构如下：

| 路径 | 内容 |
| --- | --- |
| `src/` | 源码文件（`.txt`）及窗口界面定义（`.xml`） |
| `project/` | 封包所需元数据；由 `.e` 解包得到的目录还可能包含原生快照，请勿删除 |
| `header/` | 仅 `.ec` 项目生成的公开接口头文件目录，不参与回包 |
| `ecom/` | 仅 `.e` 项目在引用 `.ec` 模块时生成；每个子目录对应一个已同步解包的模块工作区 |
| `image/` | 图片资源 |
| `audio/` | 音频资源 |
| `tool/e-packager.exe` | 随目录自带的封包工具 |
| `info.json` | 来源文件的类型、文件名、路径、修改时间、MD5 |
| `AGENTS.md` | 供 AI Agent 阅读的项目结构说明 |

如果 `.e` 工程引用了易模块（`.ec`），解包时会额外尝试将这些模块同步导出到 `ecom/<模块名>/`。这些子模块目录是独立工作区，但不会额外生成 `AGENTS.md`，便于主工程目录保持整洁。

同时，主工程 `project/模块.json` 的对应依赖项会额外写入 `resolvedPath` 与 `localWorkspace` 两个辅助字段，分别表示当前机器上解析到的模块完整路径，以及本次解包后对应的本地工作区目录。它们仅用于辅助阅读与 AI 理解，不参与回包，还原时仍以原始 `path` 字段为准。

### 回包

```
e-packager pack <input-dir> <output.e|output.ec>
```

或在项目根目录（或 `tool/` 子目录）内直接运行，自动输出到 `pack/` 目录：

```
e-packager
```

如果 `input-dir` 来自 `.ec` 解包，实际输出始终为 `.e` 文件；当你传入 `output.ec` 之类的目标名时，工具会自动在末尾补上 `.e`。无参默认封包时，`.ec` 工作区通常会输出到 `pack/<原文件名>.ec.e`。

### 其他命令

```
e-packager compare-bundle <input.e|input.ec> <input-dir>   # 比较原文件与目录内容是否一致
e-packager roundtrip <input.e|input.ec> <work-dir> <output.e|output.ec>        # 解包后立即回包
e-packager verify-roundtrip <input.e|input.ec> <work-dir> <output.e|output.ec> # 往返并校验内容一致性
```

## 注意

本工具的解包与回包功能尚不完善，存在数据损坏的可能性。**使用前请备份源文件，作者不对可能的损失负任何责任。**

补充说明：

- `.e` 工作区仍以尽量保持原始工程结构、便于往返校验为目标。
- `.ec` 直解工作区现在不会再写入桥接 `.e` 的原生快照；这是为了避免回包后的 `.e` 在易语言 IDE 中出现“非法伪装篡改代码”之类的告警。

## 致谢

本项目依赖以下开源项目：

- [OpenEpl/TextECode](https://github.com/OpenEpl/TextECode) — 易语言工程文件与文本代码互转
- [OpenEpl/EProjectFile](https://github.com/OpenEpl/EProjectFile) — 易语言项目文件读写库
