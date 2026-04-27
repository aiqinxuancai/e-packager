# e-packager

将易语言 `.e` / `.ec` 文件解包为可读目录，或将目录回包为 `.e`，让易语言项目享有 Git 版本管理、代码 Diff、AI 辅助编辑等现代开发体验。

> 📖 参考应用：[易语言 × AI Agent 实践白皮书](https://github.com/aiqinxuancai/Awesome-E-Agent)

## 使用

### 解包

```
e-packager unpack <input.e|input.ec> <output-dir>
```

如果源文件设置了打开密码，解包时传入 `--password`：

```
e-packager unpack MyApp.e MyApp\ --password 111222333
```

解包加密文件后，后续回包默认输出为未加密 `.e`，不需要再次提供密码。

也可直接将 `.e` / `.ec` 文件拖放到 `e-packager.exe` 上，自动在源文件所在目录创建同名子目录并解包：

```
e-packager MyApp.e    # 解包到 MyApp\
e-packager MyMod.ec   # 解包到 MyMod\
```

**解包目录结构：**

| 路径 | 内容 |
| --- | --- |
| `src/` | 源码文件（`.txt`）及窗口界面定义（`.xml`） |
| `project/` | 封包所需元数据；`.e` 解包后还可能含原生快照，**请勿删除** |
| `header/` | 仅 `.ec` 项目生成；公开接口头文件，不参与回包 |
| `ecom/` | 仅 `.e` 项目生成；每个子目录为一个已解包的模块工作区，不参与回包 |
| `elib/` | 依赖支持库的公开接口导出，仅供查阅，不参与回包 |
| `image/` | 图片资源 |
| `audio/` | 音频资源 |
| `tool/e-packager.exe` | 随目录自带的封包工具 |
| `info.json` | 来源文件的类型、路径、修改时间、MD5 |
| `AGENTS.md` | 供 AI Agent 阅读的项目结构说明 |

若 `.e` 工程引用了易模块（`.ec`），解包时会自动将这些模块同步导出到 `ecom/<模块名>/`。`project/.module.json` 中对应依赖项会额外写入 `resolvedPath`（本机模块完整路径）与 `localWorkspace`（本地工作区目录）两个只读辅助字段，不参与回包。

### 回包

```
e-packager pack <input-dir> <output.e|output.ec>
```

或在项目根目录（或 `tool/` 子目录）内直接运行，自动输出到 `pack/` 目录：

```
e-packager
```

> `.ec` 工作区回包的实际输出始终为 `.e` 格式；无参默认回包时输出至 `pack/<原文件名>.ec.e`。

### 刷新派生内容

解包后，若依赖的易模块或支持库发生变化，可用 `update` 命令刷新 `ecom/` 与 `elib/` 中的派生内容，无需重新解包整个工程。

```
e-packager update <input-dir>
```

**示例：**

```
# 刷新工作区的所有 ecom/elib 派生内容
e-packager update MyApp\

# 新增一个 .ec 模块到 ecom/，并刷新其导出内容
e-packager update MyApp\ --add-ecom D:\modules\MyLib.ec

# 新增多个 .ec 模块
e-packager update MyApp\ --add-ecom D:\modules\Net.ec --add-ecom D:\modules\UI.ec

# 新增支持库（按文件名 stem，自动在 lib/ 目录中查找 .fne/.fnr/.dll）
# 仅 Win32 版 e-packager 支持此选项
e-packager update MyApp\ --add-elib 互联网支持库

# 也可直接传入 .fne 文件的完整路径
e-packager update MyApp\ --add-elib D:\易语言\lib\互联网支持库.fne

# 同时新增模块与支持库
e-packager update MyApp\ --add-ecom D:\modules\Net.ec --add-elib 互联网支持库
```

### 其他命令

```
# 比较原文件与目录内容是否一致
e-packager compare-bundle <input.e|input.ec> <input-dir> [--password <text>]

# 解包后立即回包（快速验证）
e-packager roundtrip <input.e|input.ec> <work-dir> <output.e|output.ec> [--password <text>]

# 往返并校验字节一致性
e-packager verify-roundtrip <input.e|input.ec> <work-dir> <output.e|output.ec> [--password <text>]
```

## 注意

**使用前请备份源文件**，作者不对可能的数据损失负任何责任。遇到无法解包或回包的文件，欢迎提交 Issue 并附上文件。

## 致谢

- [OpenEpl/TextECode](https://github.com/OpenEpl/TextECode) — 易语言工程文件与文本代码互转
- [OpenEpl/EProjectFile](https://github.com/OpenEpl/EProjectFile) — 易语言项目文件读写库
