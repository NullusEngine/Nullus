# 渲染概览

Nullus 的渲染分三层：Game Thread 产生命令，Render Thread 消费命令并构建帧，RHI 通道把命令翻译成原生命令缓冲并提交。三层的职责边界是固定的，渲染特性必须落在正确的层上。

## 一帧的路径

```text
Game Thread
  SceneRenderStore 增量更新 + RenderFramePacket（相机、View、输出目标、单帧参数）
        |
        v
Render Thread
  FrameGraph 决定 pass 顺序与资源状态转换
  可见性收集 -> 绘制命令 -> 管线状态 -> 命令录制
        |
        v
RHI 通道（Dispatch / Translate / Submit）
  原生命令缓冲 -> 队列提交 -> 信号量 -> Present
```

Game Thread 不再每帧复制整个场景。`SceneRenderStore` 是 Render Thread 独占的持久数据，由 Game Thread 以命令增量更新；每帧只传递轻量载荷。这消除了大场景下“每帧快照整个场景”的开销。

## FrameGraph

FrameGraph 描述每个 pass 的输入输出与资源状态，并推导执行顺序和屏障。渲染代码声明依赖，框架负责：

- 资源在哪一步创建、使用与释放；
- 纹理与缓冲的状态转换；
- 临时资源的别名与复用；
- pass 之间的依赖边。

这套模型让前向与延迟两条路径共享同一批资源管理逻辑，也让新增 pass 时不需要手写屏障。

## 渲染路径

| 路径 | 内容 |
| --- | --- |
| Forward | 深度预通道、前向着色、透明物体、天空盒、编辑器叠加层 |
| Deferred | GBuffer、clustered lighting、decal、延迟光照、透明与编辑器输出 |

两条路径都通过 FrameGraph 构建，编辑器与游戏共用同一套 pass 实现，只是输出目标不同。

## 场景特性

- **Clustered lighting**：光源按视锥切分到簇，避免逐像素遍历全部光源。
- **Decal**：延迟阶段的贴花投影，挂在 GBuffer 之后。
- **天空盒**：独立 pass，先于透明物体绘制。
- **透明物体**：按后到前排序，关闭深度写入。
- **编辑器视图输出**：Scene View 与 Game View 各自持有输出目标，切换焦点不会重建整条管线。

## 大场景优化

- 空间索引驱动的可见性收集，替代逐对象线性扫描；
- 绘制命令缓存与实例化合批；
- 静态网格 LOD 构建与运行时选择；
- HLOD 代理与资源驻留预算；
- DX12 上的 HZB 遮挡剔除，按设备能力保守启用。

细节见[图形后端与大场景特性](backends.md)。

## 着色器工作流

HLSL 是唯一维护的着色器源。DXIL、SPIR-V 与反射数据都是编译产物，不允许出现“维护一份 HLSL 再手写一份 GLSL”的组合。

引擎自带一份着色器库与一组 ShaderLab 着色器：`StandardPBR`、`Transparent`、`UnlitColor`、`UnlitTexture`、`AlphaTest`、`ForwardDepthOnly`，以及延迟路径使用的 `DeferredGBuffer`、`DeferredLighting`、`DeferredDecal`、`Skybox`、`HZBBuild`、`HZBOcclusion` 等。

绑定空间是固定的，不随后端变化：

| 空间 | 内容 |
| --- | --- |
| `set0` | Frame / Scene |
| `set1` | Material |
| `set2` | Object |
| `set3` | Pass / Rare |

材质数据属于 `Material` 空间；pass 常量与 pass 纹理由渲染器控制，不放进材质实例。

## 渲染线程模式

项目文件中的 `rhi_thread_mode` 提供三种取值：

| 模式 | 说明 |
| --- | --- |
| `none` | 不使用独立 RHI 线程，便于对照与调试 |
| `dedicated_thread` | RHI 工作由固定线程承担 |
| `tasks` | RHI 通道由 TaskGraph worker 上的执行上下文表示 |

DX12 与 Vulkan 默认 `tasks`，Metal 在完成线程归属与并发提交验证前保持 `dedicated_thread`。后端以能力位声明是否支持 `tasks`；强制请求不支持的组合会给出明确原因并启动失败，不会静默回退。

## 帧资源与围栏

- `FrameResourceRing` 的槽位只回答“GPU 是否仍在使用这批资源”，复用时机由 GPU 围栏决定；
- CPU 帧延迟由 `FrameLatencyController` 与 `RenderCommandFence` 的 Render / Rhi / Swapchain 三级同步深度共同管理；
- 资源释放走延迟队列，避免在 GPU 仍可能引用时回收。

## 下一步

- [相机、光源与画面输出](cameras-lights.md)：把场景渲染出来。
- [材质与 ShaderLab](materials.md)：写自己的着色器与材质。
- [图形后端与大场景特性](backends.md)：后端支持矩阵与大场景优化细节。
