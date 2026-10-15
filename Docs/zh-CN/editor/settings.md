# 编辑器与项目设置

设置分为三层：项目文件（启动期选项）、项目设置（随项目版本控制）、用户设置（本机偏好）。

## 三层设置

| 层 | 位置 | 典型内容 | 是否入库 |
| --- | --- | --- | --- |
| 项目文件 | `<Project>/<Name>.nullus` | 分辨率、图形后端、线程模式、调试开关 | 是 |
| 项目设置 | `<Project>/ProjectSettings/` | 标签与层、项目级编辑器设置 | 是 |
| 用户设置 | `<Project>/UserSettings/editor-settings.json` | 视口缓存、面板布局、本机调试开关 | 否 |

`Edit > Settings...` 打开项目设置面板。设置项按对象分组注册，组 ID 例如 `editor.large-scene`、`editor.rendering`、`editor.runtime`、`editor.view`、`editor.debug-draw`、`editor.scene-tools`。

## 项目文件

`<Name>.nullus` 使用简单的 `key=value` 文本格式，由 Launcher 写入、由编辑器与游戏读取。启动期选项不能在运行中修改。

```ini
gravity=-9.810000
executable_name=Game
x_resolution=1280
y_resolution=720
fullscreen=false
start_scene=
vsync=false
multi_sampling=true
samples=4
graphics_backend=DX12
dev_build=true
last_opened_scene=New Scene.scene
csharp_debugging=true
lua_panda_debugging=true
lua_panda_port=8818
stop_on_entry=false
enable_threaded_rendering=true
renderdoc_startup_capture_frames=0
```

| 键 | 含义 |
| --- | --- |
| `gravity` | 全局重力加速度 |
| `executable_name` | 游戏可执行文件名 |
| `x_resolution` / `y_resolution` | 启动分辨率 |
| `fullscreen` | 是否全屏 |
| `start_scene` | 启动场景；为空时使用 `last_opened_scene` |
| `vsync` | 垂直同步，游戏运行时读取同一键 |
| `multi_sampling` / `samples` | 多重采样开关与采样数 |
| `graphics_backend` | 首选图形后端名 |
| `dev_build` | 是否作为开发构建运行 |
| `last_opened_scene` | 编辑器上次打开的场景 |
| `csharp_debugging` | 是否启用 C# 调试支持 |
| `lua_panda_debugging` / `lua_panda_port` | LuaPanda 调试开关与端口 |
| `stop_on_entry` | 是否在入口处暂停 |
| `enable_threaded_rendering` | 是否启用线程化渲染 |
| `renderdoc_startup_capture_frames` | 启动后自动抓帧的帧号，`0` 表示关闭 |

缺失的键保留默认值。代码通过 `has*` 标志区分“显式写入的值”和“尚未迁移的旧项目”，因此新增键不会破坏已有工程。

## 项目设置

项目设置保存在 `ProjectSettings/` 下，随项目一起提交。当前包含：

- **标签与层**：GameObject 的 `tag` 与 `layer` 取值来源；
- **项目级编辑器设置**：需要在团队间一致的编辑行为。

设置面板的写入通过序列化后端完成，字段结构由反射描述，设置对象本身也是可序列化对象。

## 用户设置

`UserSettings/editor-settings.json` 保存本机偏好，按设置对象分组：

```json
{
  "objects": {
    "editor.large-scene": {
      "fields": {
        "enableHZBOcclusion": true
      }
    }
  }
}
```

切换某个开关或调整面板布局后，编辑器在退出时写回该文件。这一层不应提交到版本控制。

## 渲染相关设置

- `editor.large-scene` 下的 HZB 遮挡开关用于大场景 A/B 验证，见[可见性与遮挡剔除](../large-scenes/visibility-occlusion.md)。
- Scene View 静态缓存默认关闭，避免在基准测量中因为画面未变化而跳过提交、虚高帧率。
- 候选渲染路径与可见性策略在项目设置中暴露为选项，默认值保持保守。

## 快捷键设置

快捷键配置同样按用户维度保存，结构为版本化 profile：

```json
{
  "version": 1,
  "profileName": "Default",
  "bindings": {},
  "unassigned": []
}
```

`bindings` 记录被覆盖的命令，`unassigned` 记录被显式清除绑定的命令。其余命令继续使用内置默认值。

## 下一步

- [项目设置参考](../reference/layout-cli.md#项目文件)：命令行与目录约定的完整清单。
- [构建选项](../reference/build-options.md)：编译期开关。
