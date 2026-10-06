# 窗口事件与扩展属性

使用 Win32 Release 版处理经典易语言的 32 位 FNE。x64 版不能直接加载这些支持库的设计期接口。

## 新增事件

拆包后，窗口 XML 的 `窗口.事件定义` 按控件类型列出支持库事件和通用事件，以及返回类型、参数顺序、类型、参考传递标志。该目录供查询使用，修改它不会改变支持库定义。

在目标控件下添加事件节点，并在绑定的窗口程序集 TXT 中新增处理器。例如：

```xml
<按钮 名称="按钮1" 左边="20" 顶边="20" 宽度="100" 高度="30" 标题="测试">
  <按钮.事件 名称="被单击" 处理器="窗口程序集_启动窗口::点击处理" />
</按钮>
```

```text
.子程序 点击处理
_启动窗口.标题 ＝ “点击成功”
```

`索引` 可省略，由真实支持库元数据解析；如果同时提供名称和索引，两者必须一致。旧版拆包器输出的 `_Lib…Event…` 占位名称可继续配合索引使用。通用事件使用负索引，例如 `被双击` 为 `-3`，`按下某键` 为 `-9`。这些事件的处理器必须按目录声明参数，不能省略参数或猜测类型。

菜单事件放在菜单项下，例如 `<菜单.事件 名称="单击" 处理器="窗口程序集_启动窗口::菜单处理" />`。菜单处理器不接受参数、无返回值。

封包会拒绝未知事件、名称和索引冲突、重复绑定、无法解析的处理器，以及不匹配的参数数量、类型、参考传递和返回类型。绑定应指向所属窗口程序集；新增子程序本身不会自动建立事件绑定。

## 扩展属性

普通属性按支持库公开类型写入：整数、颜色、枚举、逻辑值、文本、双精度数、日期及二进制。日期采用易语言支持库使用的 OLE DATE 数值。图片、图标、声音、图片组等二进制属性使用 Base64。

字体可用结构化节点编辑：

```xml
<编辑框.字体 高度="-24" 粗细="700" 字符集="134" 字体名="宋体" />
```

字体高度单位为像素，负值表示字符高度；粗细 `400` 为常规、`700` 为粗体。还可指定宽度、倾斜角度、方向、斜体、下划线、删除线、输出精度、裁剪精度、质量、间距和字体族。省略字段保持原字体值；没有原字体时以零初始化。

已识别的列表项目可直接修改结构化子节点。结构化节点优先于同名 Base64 属性；如果希望直接修改 Base64，先删除同名结构化节点。仅四字节对齐的私有数据不会再被猜测成整数列表。

封包使用支持库的属性通知接口，按要求重新创建控件，并从保存数据重新读取修改值。未知属性、非法值、修改只读／非设计期属性，以及不能持久化的赋值会明确失败，不会静默丢弃。

属性支持范围以 `lib2.h` 为准：20 种 `UD_*` 类型均按 `UNIT_PROPERTY_VALUE` 的规定成员读写。`UD_CUSTOMIZE` 使用 `m_data`（指针和长度），XML 以 Base64 表示，可以替换有效字节数据，不需要解析库私有的内部字段。修改经 setter、保存、重建及 getter 回读核对后才算成功。

`ITF_DLG_INIT_CUSTOMIZE_DATA` 是支持库自带的交互编辑对话框接口，`lib2.h` 没有定义通用的自定义属性字节 setter；部分库另外允许通过 `ITF_NOTIFY_PROPERTY_CHANGED` 写入自定义数据，这种情况可直接验证。仅允许对话框编辑的库不能保证无头写入，工具会明确报赋值未持久化，原始全量属性数据仍保留。`ITF_PROPERTY_UPDATE_UI` 是设计器界面的可操作性判断，不作为能否保存字节数据的额外限制。

## 验证

```powershell
python tools/TestWindowEventsAndProperties.py `
  --ide C:/path/to/e5.95.exe `
  --launcher D:/git/AutoLinker/bin/fne_release/AutoLinkerTest.exe `
  --output temp/window-events-run
```

测试复制 `e-window-exe-full+otherFne.e` 到新目录，按导出的真实签名批量新增事件，经 IDE 编译后重新拆包核对。另一个可运行工程验证按钮、编辑框、键盘、第三方双击和菜单事件，以及字体、图片、日期、列表项目，包含非法输入回归。原始工程不会被修改。

## 独有事件运行测试

```powershell
python tools/TestUniqueControlEvents.py `
  --ide C:/path/to/e5.95.exe `
  --launcher D:/git/AutoLinker/bin/fne_release/AutoLinkerTest.exe `
  --output temp/unique-events-run
```

该测试从 Win32 支持库导出的事件目录生成处理器，新增窗口程序集变量和 27 个事件绑定，回包、重新拆包，再通过真实 IDE 编译和运行。测试会短暂显示测试窗口、移动鼠标以检查手动调节器，结束后恢复鼠标位置。原始 `.e` 不修改。

已验证的 20 种控件独有事件：

| 控件 | 运行验证 |
| --- | --- |
| 列表框 | 列表项被选择、双击选择 |
| 组合框 | 列表项被选择、将弹出列表、列表被关闭 |
| 选择夹 | 将改变子夹、子夹被改变；返回假阻止切换，真允许切换 |
| 高级选择夹（iext3） | 将改变子夹、子夹被改变、子夹头被单击、子夹头被右击、子夹头被点燃；验证子夹索引和返回值 |
| 动画框（iext2） | 动画框鼠标位置改变、物体位置将改变、物体位置已改变、物体将销毁；验证物体 ID、坐标、允许／否决移动 |
| 滑块条、日期框 | 位置被改变、选择日期被改变 |
| 编辑框 | 调节钮被按下，向上按钮参数为 1；XML 的调节器方式设为 2（手动） |
| 标签 | 反馈事件接收 `(37,9)` 并返回 `379`；调用反馈事件的第三参数须为真以同步取得返回值 |

报告包含 33 次运行结果检查。“子夹头被点燃”连续验证索引 `0 → 1 → 0`，结果记录于 `hover-probe.json`。支持库会使用 `WindowFromPoint` 排除被遮挡的控件；测试必须显示窗口并确保目标点真实命中高级选择夹。64 位 Python 调用 `SetWindowPos` 时需显式声明句柄参数类型，否则 `HWND_TOPMOST=-1` 传参错误会使置顶失败。`iext3` 使用 `EVENT_INFO2`；本测试覆盖其整数参数及逻辑返回值，尚未覆盖参考参数事件。

窗口程序集的原生基类字段应为 0，不能将窗口控件类型 ID 65537 写入该字段；否则新增程序集变量虽能回读，IDE 编译引用它们时仍会崩溃。本测试同时覆盖该语义重建回归。

属性和事件的公开定义来自 `elib/lib2.h` 对应的 `LIB_DATA_TYPE_INFO`、`UNIT_PROPERTY`、`EVENT_INFO/2` 表。属性枚举值、只读／设计期限制及事件签名应以实际加载的支持库为准；接口公开了私有属性的类型和读写入口，并不意味着同时公开了 `UD_CUSTOMIZE` 内部二进制布局。

## 引用支持库的 TXT 表

工程引用生成的 `elib/*.txt` 与 `decrypt-fne` 共用导出逻辑。控件属性仍使用 `.成员` 行以兼容现有阅读方式，并导出零起始的 `类型索引`、`属性索引`、`事件索引`、`参数索引`。属性包含英文名、说明、数据类型、属性编辑器类型编号、全部枚举选项、状态标志和平台；隐藏及未命名的占位表项也保留索引。事件包含 V1/V2 版本、返回值、参数顺序和类型、V2 参考传递标志、原始状态及平台。

`UD_FILE_NAME` 的配置单独导出为对话框标题、文件过滤器、默认后缀和保存文件标志，空字段不会截断后续配置。枚举不再静默截断为 128 段。读取失败或超过安全上限会明确标注；属性表损坏时不会伪造八个固定属性。平台位为零输出“未声明”，不猜测兼容平台。

这里的事件数对应 FNE 自己的事件表；IDE 提供的负索引通用窗口事件仍查阅窗口 XML 的 `窗口.事件定义`。TXT 是定义表，不包含某个窗口实例的当前属性值，私有自定义数据格式也不由该表推断。

`tools/TestFnePublicInfoExport.py` 核对四个实际支持库及独立 FNE 测试夹具，检查数量、连续索引、事件参数，以及引用导出与独立导出正文一致。在 VS 的 x86 开发者命令提示符中构建夹具后运行（输出目录须不存在）：

```bat
mkdir temp\fne-export-check
cl /nologo /LD /utf-8 /std:c++20 /Ielib tools\fixtures\FnePublicInfoTest.cpp /Fotemp\fne-export-check\fixture.obj /Fetemp\fne-export-check\fixture.fne /link /EXPORT:GetNewInf=_GetNewInf@0
python tools/TestFnePublicInfoExport.py --fixture temp/fne-export-check/fixture.fne --lib-dir C:/path/to/e/lib --output temp/fne-export-check/results
```

## lib2.h 全属性类型 ABI 验证

`tools/fixtures/Lib2PropertyTest.cpp` 提供真实 Win32 FNE，发布全部 20 种 `UD_*` 类型。`Lib2PropertyCodecTest.cpp` 通过生产代码的 `Apply/Decode` 验证整数、限定选择、普通选择、双精度、逻辑、日期、三种文本编辑器、文件名、图片、图标、光标、音乐、字体、三种颜色、图片组和自定义字节。另验证 setter 要求重建，以及 setter 拒绝赋值时返回 `window_control_property_not_persisted`。该夹具验证 ABI，不代替实际支持库对图片、字体等载荷格式的校验。

在 VS x86 开发者命令提示符中运行：

```bat
mkdir temp\lib2-property-check
cl /nologo /LD /utf-8 /std:c++20 /EHsc /Ielib tools\fixtures\Lib2PropertyTest.cpp /Fotemp\lib2-property-check\fixture.obj /Fetemp\lib2-property-check\fixture.fne /link user32.lib /EXPORT:GetNewInf=_GetNewInf@0
cl /nologo /utf-8 /std:c++20 /EHsc /Ielib /Isrc tools\fixtures\Lib2PropertyCodecTest.cpp src\FormControlPropertyCodec.cpp src\SupportLibraryRuntime.cpp src\PathHelper.cpp /Fotemp\lib2-property-check\ /Fetemp\lib2-property-check\test.exe /link user32.lib advapi32.lib
temp\lib2-property-check\test.exe %CD%\temp\lib2-property-check\fixture.fne
```
