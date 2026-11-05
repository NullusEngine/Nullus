# 原生 ECS Transform

实现位于 Runtime/Engine/ECS/TransformHierarchy.*，复用既有 Math::Matrix4 和 Quaternion。
组件使用可安全移动的数组数据，避免把当前 Math 值类型中未声明 noexcept 的移动操作
直接引入 Chunk。LocalTransform 的 position/rotation/scale 分别为 3/4/3 个 float；
Parent 持有可校验的 Entity；WorldTransform 为行主序 16 个 float 和 revision。

```cpp
using namespace NLS::Engine::ECSIntegration;
RegisterTransformSystem(world); // 默认 LateUpdate。
auto root = world.CreateEntity();
auto child = world.CreateEntity();
AddTransform(world, root, {{10, 0, 0}});
AddTransform(world, child, {{2, 0, 0}}, root);
world.Systems().RunPhase(NLS::ECS::SystemPhase::LateUpdate, {});
// child 的 WorldTransform.matrix[3] == 12。
SetTransformParent(world, child, {}); // 保持 local pose。
```

输出遵循既有数学规则：local = T * R * S，world = parentWorld * local。支持 2D/3D、
旋转、非均匀/负缩放和零缩放。旋转先验证有限值和非零长度，再归一化；结果发生
浮点溢出时报告错误，不把非有限矩阵写入 World。

System 缓存实际输入和父引用；不变帧不会写输出或推进 WorldTransform 变更版本。
父数据改变、重新挂接、派生输出丢失或被外部改写会触发所需分支重新计算。revision
仅在该实体重算时递增，耗尽会报错。WorldTransform 明确标为 transient，加载后由
System 重建，资产仅保存 LocalTransform 和经过局部引用 codec 重映射的 Parent。

层级排序使用迭代遍历；4096 层测试不依赖递归栈。环在输出提交前报错。
失效、跨 World 或已失去 LocalTransform 的父节点按 root 计算，并在阶段末通过 ECB
移除失效 Parent；已销毁再复用的 slot 不会被误认为原父节点。

同一层中需要重算的节点通过 IJobParallelFor 批量交给已有 JobSystem；下一层开始前 join 上一层。
worker 只处理已复制的数值输入，不访问 GameObject 或结构修改 World。所有层计算和
校验完成后，主线程才提交输出；缺少的 WorldTransform 通过 ECB 在阶段末补齐。
singleThreaded 和 minimumParallelEntities 同样约束这个 System。

WorldTransform 是本 System 拥有的派生输出。业务修改 LocalTransform/Parent，并在
同阶段通过显式依赖确保先更新输入；默认把业务放在 Update、层级放在 LateUpdate。
结构修改使用安全点或 ECB，不能在 Job 中直接调用 AddTransform/SetTransformParent。

当前每帧仍扫描输入，部分索引/临时容器仍分配内存；不是零分配层级流水线。
WorldPose 继续服务已有显式 GameObject 运行时桥接；原生层级没有自动写回该桥接数据。
TransformBaker 提供从 GameObject Authoring 到原生组件的受控单向转换。原生矩阵到
Renderer/Physics 的专用提取与运行时桥接仍在后续子系统阶段。
