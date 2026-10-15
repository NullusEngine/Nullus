# Prefab 工作流

Prefab 把一段对象层级变成可复用资产：源 Prefab 定义结构，场景里的实例引用它，实例上的修改以 Override 形式记录。编辑 Prefab 时会在独立的 Prefab Stage 中打开，而不是直接改场景。

**当前状态：部分实现。** 创建、实例化、Stage 保存与 Inspector Override 已接入编辑器；Variant、嵌套与 Unpack 有原生工作流和测试，但入口尚未全部接入当前面板。下面区分可以直接操作的步骤与引擎扩展接口。

## 基本概念

| 概念 | 说明 |
| --- | --- |
| Prefab 资产 | 由对象层级序列化而来的 `.prefab` 资产，带资产 ID 与子资源键 |
| Prefab 实例 | 场景中引用某个 Prefab 的对象根，记录到源对象的映射 |
| Override | 实例相对源 Prefab 的字段或结构差异，以补丁操作记录 |
| Prefab Stage | 单独打开某个 Prefab 进行编辑的模式 |
| Prefab Variant | 以另一个 Prefab 为基底派生的 Prefab |
| 解除关联（Unpack） | 把实例展开为场景自有对象，断开与源 Prefab 的连接 |
| 生成只读 | 由导入器生成的 Prefab 子资产，标记为不可直接编辑 |

## 从场景创建 Prefab

前置条件：处于编辑态，场景已保存到项目 `Assets/`，目标根对象还没有连接到 Prefab。

1. 在 Hierarchy 中组织好对象层级并命名根对象，例如 `Crate`。
2. 在 Asset Browser 中用 `Create > Folder` 创建并打开 `Assets/Prefabs`。
3. 把 Hierarchy 的根对象拖到该目录。编辑器按对象名生成 `Crate.prefab`，重名时追加编号，并把原对象连接为实例。
4. 等待导入完成，再把 `Crate.prefab` 从 Asset Browser 拖到 Hierarchy 的空白处创建第二个实例，或拖到对象行下创建子实例。
5. 保存场景。预期两个实例都能显示，重新打开场景后仍保持 Prefab 引用。

若 Console 报 owning scene 或 Undo scope 错误，先保存场景再重试。已经连接的实例不能用同一条保存路径创建新 Prefab；Variant/Unpack 的当前边界见下文。

导入模型时也会生成只读 Prefab 子资产。它们可以实例化，但不能直接覆盖源 Prefab。

## Prefab Stage

打开 Prefab 会进入 Prefab Stage：一个只包含该 Prefab 层级的独立编辑上下文，场景与 Stage 各自持有自己的对象图。

Stage 状态包含：

- 关联的 Prefab 资产 ID、路径与子资源键；
- 是否已加载、是否有未保存修改；
- 是否可编辑、是否为生成只读；
- Stage 根对象与打开时的对象图快照。

双击 Asset Browser 中的 Prefab 打开 Stage。在 Hierarchy 中编辑其对象，用 `File > Save Scene` / `Ctrl+S` 保存；处于 Stage 时该操作实际保存 Prefab。预期 Console 出现 `Active prefab stage saved.`。

当前 Stage 退出与替换流程尚未提供完整的未保存修改保护。切换其他 Prefab 或离开前先显式保存，不要依赖关闭确认对话框。生成只读 Prefab 的 Stage 不能保存修改。

## Override

Override 以补丁操作记录，而不是为每种对象写专用载荷：

- 补丁记录源对象、实例对象、属性路径与操作内容；
- 结构变化（新增对象、删除对象）记录为对象记录数组；
- 记录所属的 Prefab 层级名，用于嵌套场景。

Hierarchy 会用不同颜色区分状态：普通、连接根、连接子对象、含 Override、缺失引用、生成只读、待处理、已解除关联。选中实例时，Inspector 会显示可回退的 Override。

在实例上修改一个属性后，右键该属性可看到 `Apply Override` / `Revert Override`；对象工具栏的 `Overrides` 菜单提供 `Apply All Overrides` / `Revert All Overrides`。Apply 写回源资产，Revert 恢复源值。生成只读或缺失源资产时 Apply 会禁用。保存场景可保留实例自己的修改。

## Prefab Variant

Variant 以另一个 Prefab 为基底创建，自身只记录差异。创建时需要给出：

- 基底 Prefab 的资产 ID 与子资源键；
- 目标路径与新 Variant 的资产 ID；
- 基底是否为生成只读、目标路径是否已存在。

基底是生成只读时，创建流程会先把它转换为可编辑形态，再生成 Variant。

这些能力由原生 [PrefabUtilityFacade](../../../Project/Editor/Assets/PrefabUtilityFacade.h) 与 [AssetDragDropWorkflow](../../../Project/Editor/Assets/AssetDragDropWorkflow.h) 提供。当前 Asset Browser 仍未把 `Create Variant` 命令描述接成可点击菜单，不应按其他引擎的菜单路径操作。

## 嵌套与保留数据

实例记录会保留：

- 源到实例的对象 ID 映射，以及实例对象到源对象 ID 的反向查找；
- 局部补丁列表；
- 实例根对象 ID；
- 新增对象的保留记录与对象对应关系；
- 保留的资产引用与已解析资产；
- 嵌套子实例列表。

这些数据让实例在保存、重载与重新导入后仍能准确回表到源 Prefab。

## 解除关联

解除关联会把实例展开成场景自有对象：

- 返回展开后的根对象；
- 记录转为场景所有的对象 ID 列表；
- 保留仍然指向外部资产的引用。

解除后该层级不再随源 Prefab 更新。

原生 `PrefabUtilityFacade` 提供 Unpack，当前面板尚无完整的 Unpack 操作入口。不要通过手改 `.prefab`、GUID 或序列化记录来模拟解除关联。

## 资源生命周期

Prefab 实例共享源 Prefab 的只读数据。实例创建、保存与销毁由专门的资源生命周期管理器跟踪，避免同一 Prefab 被重复解码，也避免销毁过程中留下悬空引用。

## 相关格式

Prefab 与场景共用同一套对象图格式：`$owned` 表达生命周期归属，`fileID` / `guid` / `type` 表达引用。写入与读取规则见[反射与序列化](../architecture/reflection-serialization.md)。

## 下一步

- [反射与序列化](../architecture/reflection-serialization.md)：理解 Prefab 与场景的文件结构。
- [Asset Browser 与导入](../assets/pipeline.md)：管理 Prefab 资产的导入与依赖。
