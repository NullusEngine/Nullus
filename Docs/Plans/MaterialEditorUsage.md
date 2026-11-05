# 材质编辑器

## 创建和编辑

在 Asset Browser 的 Create → Material 中创建 PBR 或 Unlit 材质，双击 `.mat` 打开 Material Editor。右键已有材质的 Create Material Instance 创建继承它的 `.mat`。实例只保存本地覆盖；参数旁的 Inherit 删除本地覆盖。

Create → Shader 提供手写 ShaderLab 模板、Material Graph 和 Material Function。双击 `.shader` 打开 ShaderLab/HLSL 代码页，双击 `.shadergraph` 或 `.shaderfunc` 打开节点页；Source JSON 页可查看完整数据。右键 Shader/Graph 的 Create Material 创建使用该共享程序的材质。

节点画布支持连线、删除、移动和右键添加节点。左侧提供参数黑板、图设置、函数端口和选中节点属性。Parameter 使用稳定参数 ID；重命名显示标签不会改变绑定。CustomHLSL 通过类型明确的输入输出端口和 include 文件工作。Function 节点设置函数资源路径和调用端口，FunctionInput 选择对应输入，连到 Function Output 形成返回值。

Surface 图支持 PBR、Unlit，Opaque、Masked、Alpha、Premultiplied、Additive，以及 Vertex 阶段的 WorldPositionOffset。WPO 必须声明正的 Maximum world displacement；生成代码限制最大世界位移，几何剔除使用扩展后的包围盒。Particle Sprite 图使用现有粒子材质 ABI，并提供粒子颜色、年龄、速度和动态参数节点。

Texture2D 与 TextureCube 参数使用已导入的原生纹理产物；Cube 参数保留面和 mip 层级。`white`、`black`、`bump` 是内置纹理默认值，不作为外部资产路径交给场景资源就绪检查。材质复制保留这些纹理的共享所有权，已记录绘制保留对应 RHI 资源。

## 保存、预览、应用

| 操作 | 效果 |
| --- | --- |
| Save / Save source | 持久化 Working，更新 Saved |
| Compile / Preview | 编译冻结的依赖快照，更新私有预览 |
| Show Live Preview | 在 Asset View 中查看预览材质 |
| Apply to Scene | 在帧边界发布当前快照，更新 Applied 和已加载的场景材质 |
| Save and Apply | 显式执行保存与应用 |
| Revert Working | 从 Saved 恢复 Working，保留场景的 Applied |
| Undo / Redo | 使用现有 EditorUndoService 编辑历史 |

依赖默认使用 Applied 内容。材质页 Dependencies 中显式勾选 Use current version 后，该依赖的 Working 才进入当前编译/应用快照。函数通过调用它的材质图预览和应用；函数代码页会提示在调用方选择 Working 依赖。

旧版已加载 Shader 如果没有 Applied 源码归档，工作台保留其当前运行时资源并提示显式选择 Use current shader dependency；不会把磁盘上后来保存的源码自动当成其 Applied 版本。

应用父材质会准备所有已加载子材质的候选资源，再一起更新。应用共享 Shader/Graph 会更新已加载且引用该程序的不同根材质和后代；各自的本地参数保留。运行时 `MaterialInstance` 保留动态覆盖。编译失败或完整依赖快照过期时不发布。

## 资源发布

材质应用成功后，Publish resources 把该快照发布到 `Library/MaterialReleases/<snapshot>-<generation>/Library/ArtifactDB` 和内容寻址的 `Library/Artifacts`。发布重新生成 DXIL/SPIR-V，收集实际材质静态参数组合、扁平化继承并复制冻结的纹理产物。Graph、函数及冻结源码归档留在构建缓存；发布目录不依赖创作源文件。这个入口发布材质资源，Player/安装包仍使用项目原有的构建和运行时清单流程。

工作台 Variant budget 控制本次请求预算。导入器使用 Shader `.meta` 的 `settings.materialVariantBudget`（默认 128，上限 4096）。导入 `.mat` 时记录实际请求的组合，再补编译所需 Shader 变体；发布器从发布材质重新收集使用集合，不会照搬编辑器的历史请求。

无界面构建工具与编辑器复用 `NLS_MaterialCompiler`：

```powershell
build\bin\Debug\NullusMaterialCompile.exe `
  D:\Project D:\Code\Nullus\App\Assets\Engine D:\Project\MaterialRelease `
  Assets\Paint.mat Assets\PaintInstance.mat --budget 128 --target win64
```

一次命令发布一个共享 Shader 家族。CLI 使用磁盘上的 Saved 输入；编辑器发布使用 Applied 冻结快照。`--variant A,B` 可重复，表示显式保留的动态使用组合。`--target` 写入运行时平台标识；`win64` 生成 DXIL，`linux`/`macos` 生成 SPIR-V，默认 `editor` 同时生成两者。生成目标字节码不等于目标设备验证。纹理须已导入到项目 ArtifactDB，或直接引用已有原生纹理产物。

## 已有渲染机制的边界

保留三张 RGBA8 GBuffer；Unlit/发光图使用 ForwardOpaqueDepth → 延迟光照排除 → ForwardOpaqueColor，并共享深度。手写 Shader 要通过 Pass 标签声明 `MaterialRenderPath=ForwardOpaque`、`ShadingModel=Unlit`，提供匹配的 DepthOnly。需要 WPO/AlphaClip 的手写程序还须提供一致的 Picking、SelectionMask 和 ShadowCaster。

Graph 自动生成上述辅助 Pass。当前仓库没有完整的动态阴影渲染器，因此 ShadowCaster 的编译契约不表示动态阴影功能已实现。Metal/macOS 和节点画布实际鼠标操作需要各自的构建/设备/交互验收；Windows 上的编译和自动测试不能替代它们。

首版函数节点的调用端口由作者显式配置；函数通过调用材质发布。Static Switch 只编译请求的组合，但节点图仍对两条分支执行阶段合法性检查，因此不能在 WPO 的未选分支中放置屏幕导数节点。Custom HLSL 的显式资源注册声明须改为图的类型输入，include 仍参与完整依赖冻结。
