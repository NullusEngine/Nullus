# 物理现状与规划

当前状态为**部分实现**：已有基于 PhysX 5.6.1 的场景查询世界与静态碰撞体，供粒子碰撞等原生查询使用；动力学刚体、完整碰撞事件和角色控制器仍未实现。

## 静态碰撞体

1. 给场景对象添加 **Physics / Collider** 组件。
2. 选择 Box、Sphere 或 Capsule，设置 Half Extents、Radius、Half Height、Layers 和 Trigger。
3. 运行场景。场景在组件更新前同步查询几何，移动、禁用或删除对象后相应查询体也应更新或释放。

Trigger 在此处是查询过滤属性，不代表已有 Enter/Stay/Exit 回调。Half Extents 是盒子的半尺寸；组件只提供静态查询几何，不因重力产生运动。

## 原生查询接口

`PhysicsWorld` 提供盒、球、胶囊和三角网格注册，以及位置更新与删除。通过 `Read(QueryFilter)` 获取读取视图，可执行 `Raycast`、`RaycastAll`、`SweepSphere` 和 `OverlapSphere`。读取视图持有同步访问，在同一线程修改世界前先释放它。

每个场景拥有独立查询世界。[Particle System](../audio-vfx/particles.md) 默认连接拥有场景的世界。碰撞体形状、尺寸、层和 Trigger 配置可通过反射/脚本访问；通用查询接口目前仍是原生 API，不等于已提供完整 C# / Lua 物理查询库。

## 依赖与构建

CMake 固定 PhysX 5.6.1 源码版本。需要离线配置时设置 `NLS_PHYSX_SOURCE_DIR`，目录中应包含 `physx/`。SDK 源码与构建产物放在构建目录，CPU SDK 与引擎 RHI 的 GPU 粒子模拟是不同模块。

`Tests/Unit/PhysicsWorldTests.cpp` 与 `PhysicsSceneTests.cpp` 提供世界查询和场景同步检查。测试源文件存在或某一平台编译成功不代表所有平台设备验收完成。

## 尚未实现

- 动力学刚体步进、接触求解、关节、摩擦/恢复系数的完整创作流程。
- 触发器事件、碰撞回调、角色控制器和物理调试可视化。
- 完整脚本查询库、子步与表现插值策略。

项目中的 `gravity` 与固定步长回调仍不能被视为已经接入动力学求解。[动画](../animation/roadmap.md)提供姿态和表现，不能替代运动学或动力学角色控制。

## 相关内容

- [生命周期与激活状态](../scene/lifecycle.md)：固定步长回调。
- [功能状态矩阵](../roadmap/index.md)：能力与验证边界。
