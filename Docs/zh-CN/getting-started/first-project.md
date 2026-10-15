# 第一个项目

本页用最短路径得到一个可运行结果：创建项目、搭一个可见的立方体场景、挂上第一个脚本并进入 Play。项目和场景在编辑器内管理，脚本内容用外部代码编辑器修改。

## 前置条件

已经按[环境准备与构建](build.md)构建出 `Editor` 与 `Launcher`。

## 1. 创建项目

启动 Launcher，选择新建项目，给出项目名、磁盘路径与模板。创建向导会：

1. 建立项目目录，并创建 `Assets/`、`ProjectSettings/` 与 `UserSettings/`；
2. 把所选模板 `content/` 下的内容复制进项目。内置的 Empty Project 模板位于 `App/Assets/Templates/EmptyProject`，随附 `Assets/Scripts/ExampleBehaviour.cs` 与 `ExampleBehaviour.lua`；
3. 写入 `<Project>.nullus` 项目文件。

`Library/` 由编辑器首次打开项目时生成。

`<Project>.nullus` 记录分辨率、图形后端、启动场景、是否启用线程化渲染等启动期选项。这些值在编辑器与游戏启动时读取，运行中不可更改。完整键位说明见[项目设置参考](../reference/layout-cli.md#项目文件)。

## 2. 打开编辑器

在 Launcher 中打开刚创建的项目，确认标题栏与项目路径指向刚创建的目录。首次打开时需要等待资产初始化与脚本构建；不要把仍在导入的资产当成丢失资源。

## 3. 搭一个场景

1. 选择 `File > New Scene`（`Ctrl+N`）。当前编辑器会创建带 `Main Camera`、方向光、环境光与天空盒的基础场景。
2. 选择 `GameObjects > Primitives > Cube` 创建立方体。在 Hierarchy 中选中 Cube，在 Inspector 的 Transform 中设置位置 `(0, 0, 5)`、旋转 `(0, 0, 0)`、缩放 `(1, 1, 1)`。
3. 选中 `Main Camera`，确认位置与旋转均为 `(0, 0, 0)`。引擎的前方为 `+Z`，因此相机应朝向刚创建的立方体。不要把相机和立方体放在同一个位置。
4. 保留默认方向光与环境光。若所用场景没有这些对象，可用 `GameObjects > Others > Camera`、`GameObjects > Lights > Light` 分别创建相机和光源，再在 Inspector 中配置光源类型。
5. 选择 `File > Save Scene As...`（`Ctrl+Shift+S`），保存到项目 `Assets/Scenes/FirstScene.scene`。以后用 `Ctrl+S` 保存。

预期结果：Hierarchy 中有 Cube、相机和光源；进入 Play 后 Game View 能显示立方体。Scene View 使用自己的编辑相机，不能据此判断游戏相机的构图。

需要外部模型时，按[模型、网格与纹理](../assets/models-textures.md)导入。场景与 Prefab 的保存格式见[反射与序列化](../architecture/reflection-serialization.md)。

## 4. 写第一个 C# 脚本

在 Asset Browser 中打开 `Assets/Scripts`（可先用 `Create > Folder` 建立目录），右键空白处选择 `Create > C# Script`，命名为 `Rotator`。用代码编辑器打开 `Rotator.cs`，将内容替换为：

```csharp
using Nullus;

public sealed class Rotator : Behaviour
{
    private void Awake()
    {
        Debug.Log("Rotator awake");
    }

    private void Update()
    {
        transform.Rotate(Vector3.up * (30.0f * Time.deltaTime));
    }
}
```

保存后资产监听器会增量构建程序集，构建成功后编辑器在帧边界热重载并保留字段；构建失败会继续运行上一份有效程序集，错误进入 Console。

等待构建成功后，把 Asset Browser 中的 `Rotator.cs` 拖到 Hierarchy 的 **Cube 对象行**上，不能拖到空白处。选中 Cube，确认 Inspector 出现 Rotator 脚本组件，然后再次保存场景。

按工具栏 Play。预期 Game View 中的立方体持续旋转，Console 出现 `Rotator awake`。按 Stop 回到编辑态，运行时旋转不会写回编辑场景。

## 5. 写第一个 Lua 脚本

这是替代 C# 的可选步骤。用 `Create > Lua Script` 创建脚本，编辑后拖到另一个测试对象上。Lua 脚本返回一个 module 表，生命周期函数是表上的成员，通过 `self` 访问所在对象：

```lua
local Module = {}

function Module:Update()
    local position = self.transform.localPosition
    position.x = position.x + Nullus.Time.deltaTime
    self.transform.localPosition = position
end

return Module
```

`Nullus` 是引擎暴露的全局 API 表（例如 `Nullus.Time`）。保存 `.lua` 文件不需要编译步骤，编辑器下次运行或重载时直接生效。两种语言的差异见[C# 与 Lua](../scripting/overview.md#c-与-lua-的分工)。

## 6. 运行与观察

- 按 Play 进入运行态；Game View 显示最终画面，Scene View 保留编辑视角。
- Console 收集脚本日志与错误。
- 打开 Profiler 面板观察 Timeline 或 Tracy 数据（需要相应构建开关，见[性能分析与图形调试](../quality/profiling.md)）。

## 排错与限制

| 现象 | 检查 |
| --- | --- |
| Game View 没有立方体 | 相机是否启用、Cube 是否在 `+Z` 方向、是否保留光源，以及模型/材质导入是否成功 |
| 脚本不能挂载 | Console 是否有编译错误、类是否派生 `Behaviour`、是否拖到了具体对象行 |
| 双击脚本未打开 IDE | 配置外部代码编辑器，或直接从磁盘打开文件，见[脚本调试与热重载](../scripting/debugging.md) |
| 停止后旋转恢复 | 这是 Play 沙箱的预期行为；在编辑态修改初始 Transform 后保存 |

本页菜单与 API 已按源码核对；编译示例、运行时单元测试和完整桌面操作是不同验证层级。当前验收范围记录在[调试与测试](../quality/debugging.md)。

## 目录里多了什么

| 路径 | 内容 |
| --- | --- |
| `Assets/` | 源资产与脚本 |
| `ProjectSettings/` | 项目级设置 |
| `UserSettings/` | 本机用户设置，例如 `editor-settings.json` |
| `Library/` | 导入结果、工件、缩略图缓存与 IDE 工作区 |
| `Library/ScriptApi/ScriptApi.json` | 合并后的脚本 API 清单 |

`Library/` 是可重建的中间产物，不应纳入版本控制。

## 下一步

- [编辑器工作区](../editor/workspace.md)：把界面用顺手。
- [场景与对象](../scene/objects.md)：理解 GameObject 与组件的关系。
- [脚本 API 参考](../scripting/api.md)：查看可用的类型与成员。
