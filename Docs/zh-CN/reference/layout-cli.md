# 目录结构与命令行

本页是查找型参考：项目目录里各文件的作用、可执行文件支持的启动参数。

## 项目目录

| 路径 | 是否入库 | 内容 |
| --- | --- | --- |
| `<Project>.nullus` | 是 | 项目文件，记录启动期选项 |
| `Assets/` | 是 | 源资产与脚本 |
| `ProjectSettings/` | 是 | 项目级设置（标签与层等） |
| `UserSettings/` | 否 | 本机用户设置 |
| `Library/` | 否 | 导入结果、工件、缩略图缓存与 IDE 工作区 |
| `Logs/` | 否 | 运行时日志与 RenderDoc 抓取 |

`Library/` 可以整体删除并重新生成。需要长期保留的只有项目文件、`Assets/` 与 `ProjectSettings/`。

### 模板

项目模板位于 `App/Assets/Templates/<模板 ID>/`：

| 文件 | 说明 |
| --- | --- |
| `template.json` | 模板名、描述、分类与排序 |
| `content/` | 创建项目时复制进项目的初始内容 |
| 预览图 | 可选，供列出模板时显示 |

内置 Empty Project 模板的 `content/` 包含 `Assets/Scripts/ExampleBehaviour.cs` 与 `ExampleBehaviour.lua`。

## 项目文件

`<Project>.nullus` 使用 `key=value` 文本格式，由 Launcher 写入、由编辑器与游戏读取。

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

| 键 | 说明 |
| --- | --- |
| `gravity` | 全局重力加速度。当前是配置项，尚未被物理解算消费 |
| `executable_name` | 游戏可执行文件名 |
| `x_resolution` / `y_resolution` | 启动分辨率 |
| `fullscreen` | 是否全屏 |
| `start_scene` | 启动场景；为空时回退到 `last_opened_scene` |
| `vsync` | 垂直同步，编辑器与游戏共享 |
| `multi_sampling` / `samples` | 多重采样开关与采样数 |
| `graphics_backend` | 首选图形后端名 |
| `dev_build` | 是否作为开发构建运行 |
| `last_opened_scene` | 编辑器上次打开的场景 |
| `csharp_debugging` | 是否启用 C# 调试支持 |
| `lua_panda_debugging` / `lua_panda_port` | LuaPanda 调试开关与端口 |
| `stop_on_entry` | 是否在入口处暂停 |
| `enable_threaded_rendering` | 是否启用线程化渲染 |
| `renderdoc_startup_capture_frames` | 启动后自动抓帧的帧号，`0` 表示关闭 |

缺失的键保留默认值。项目文件里没有 `rhi_thread_mode` 时，后端使用自己的默认模式；写入 `rhi_thread_mode` 后按值解析。

## 用户设置

`UserSettings/editor-settings.json` 按设置对象分组保存本机偏好：

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

## 编辑器命令行

```text
Editor [options] [project_path]
```

`project_path` 是 `.nullus` 项目文件或项目目录，为必需参数。

### 通用选项

| 选项 | 说明 |
| --- | --- |
| `--backend <name>` / `-b <name>` | 指定本次运行的图形后端 |
| `--renderdoc` / `--no-renderdoc` | 开关 RenderDoc 集成 |
| `--capture-after-frames <N>` | 第 N 次呈现后自动抓帧 |
| `--help` / `-h` | 显示帮助 |
| `--editor-prewarm-assets` | 导入项目资产后退出，不打开编辑器 |
| `--editor-dx12-debug-layer` | Debug 构建下启用 DX12 校验层（启动更慢） |
| `--editor-startup-resource-pipeline <async\|legacy\|pipelined>` | 启动资源配置方式；Windows 默认 `pipelined`，其它平台默认 `async` |

### 启动校验选项

| 选项 | 说明 |
| --- | --- |
| `--editor-validation-focus-view <scene\|game>` | 启动校验时聚焦某个视图 |
| `--editor-validation-exclusive-view <scene\|game>` | 启动校验时关闭另一个视图 |
| `--editor-validation-open-frame-info` | 打开 Frame Info |
| `--editor-validation-open-profiler` | 打开 Profiler |
| `--editor-validation-trace-frames <N>` | 导出 N 帧的 TimelineProfiler 追踪 |
| `--editor-validation-select-gameobject <name>` | 选中指定 GameObject |
| `--editor-validation-camera-forward-frames <N>` | 按固定步长向前推进 Scene View 相机 N 帧 |
| `--editor-validation-create-asset <path>` | 创建一个资产实例 |
| `--editor-validation-asset-browser-folder <path>` | 在 Asset Browser 中选中目录 |
| `--editor-validation-disable-hzb-occlusion` | 关闭 HZB 遮挡用于 A/B 对比 |
| `--editor-validation-occlusion-stack <N>` | 创建 N 个重叠立方体用于遮挡验证 |
| `--editor-validation-scene-readback-output <path>` | 写出 Scene View 渲染目标 PNG |
| `--editor-validation-scene-readback-summary <path>` | 写出 Scene View 读回摘要 |
| `--editor-validation-prefab-drag-proxy-summary <path>` | 写出 Prefab 拖拽代理摘要 |
| `--editor-validation-scene-camera <pos;rot>` | 强制 Scene View 相机，例如 `1,2,3;10,20,30` |
| `--editor-log-render-draw-path` | 输出渲染器绘制与打包诊断 |
| `--editor-log-scene-camera-input` | 输出 Scene View 输入与相机移动诊断 |

### 相机基准选项

| 选项 | 说明 |
| --- | --- |
| `--editor-camera-performance-output <path>` | 写出低开销 Scene View 相机基准 JSON |
| `--editor-camera-performance-warmup-frames <N>` | 预热帧数，默认 30 |
| `--editor-camera-performance-frames <N>` | 测量帧数，默认 300 |
| `--editor-camera-performance-settle-frames <N>` | 相机运动后采集的静止帧数，默认 0 |

### 缩略图与遥测选项

| 选项 | 说明 |
| --- | --- |
| `--editor-thumbnail-telemetry-summary <path>` | 编辑器退出时写出缩略图遥测摘要 |
| `--editor-thumbnail-resident <0\|1>` | 启用常驻 Prefab 缩略图源 |
| `--editor-thumbnail-proxy-pool <0\|1>` | 启用预览代理池 |
| `--editor-thumbnail-atlas <0\|1>` | 启用解码后的缩略图图集 |
| `--editor-thumbnail-readback-ring <0\|1>` | 启用 GPU 读回环 |
| `--editor-thumbnail-adaptive-budget <0\|1>` | 启用自适应缩略图预算 |
| `--editor-thumbnail-lanes <0\|1>` | 启用显式缩略图通道 |
| `--editor-thumbnail-cache-root <path>` | 覆盖缩略图缓存根目录 |

### 示例

```powershell
Editor.exe MyProject.nullus
Editor.exe --renderdoc MyProject.nullus
Editor.exe --capture-after-frames 60 MyProject.nullus
Editor.exe --backend dx12 --editor-dx12-debug-layer MyProject.nullus
```

## Game 命令行

见[Game 运行时与资产清单](../platform-build/runtime.md#game-命令行)。

## 环境变量

| 变量 | 作用 |
| --- | --- |
| `NLS_BUILD_JOBS` | 覆盖构建并行度 |
| `NLS_MSVC_MP_JOBS` | 限制目标内 MSVC `/MP` 进程数 |
| `NLS_BUILD_TARGETS` | 只构建指定目标 |
| `NLS_CMAKE_OPTIONS` | 传入额外 CMake 配置参数 |
| `NLS_RENDERDOC_ENABLE` / `NLS_RENDERDOC_CAPTURE` / `NLS_RENDERDOC_CAPTURE_AFTER_FRAMES` | RenderDoc 启动抓取 |
| `NLS_RENDERDOC_CAPTURE_DIR` / `NLS_RENDERDOC_CAPTURE_LABEL` / `NLS_RENDERDOC_AUTO_OPEN` | RenderDoc 抓取输出与自动化 |

## 下一步

- [构建选项](build-options.md)：编译期开关。
- [编辑器与项目设置](../editor/settings.md)：设置项的语义。
