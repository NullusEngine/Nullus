# 粒子制作工作流

Particle Editor 使用有序模块栈制作粒子系统，运行时支持 CPU 与 GPU 发射器、Sprite 和 Mesh 绘制。首版已有编辑、编译、导入与播放链路，完整模块库、生产工具和各平台设备验收仍需补充。

## 创建与预览

1. 将 `Templates/VFX/Examples/Starter.vfx` 或 `GpuStarter.vfx` 复制到项目 Assets 中，打开 **Window > Particle Editor**，再打开该文件。Asset Browser 的双击、Edit 和组件 System 引用使用同一编辑器。
2. 在 System Overview 中选择发射器，在模块栈编辑输入。要让重力影响运动，把 **Gravity Force** 放在 **Solve Forces and Velocity** 之前；关闭的模块不执行。
3. 在参数中定义 User 输入，再绑定到模块；曲线或渐变输入可在 Curves 中编辑，Dynamic Input 打开与父输入类型匹配的表达式图。
4. 用整系统预览检查 CPU/GPU 发射器，播放、暂停或重启。无效脚本会显示诊断并保留上一次成功编译的预览。
5. 保存到项目 Assets 后，经资产数据库导入编译程序；将资源赋给场景中的 Particle System 组件的 System 字段，再进入 Play 模式。

模块输入编辑参与 Undo/Redo。输入变化、预览成功与资产导入成功是不同状态；导入失败时保留上一份发布工件。

## 资源与命令行

可复用脚本和发射器资源支持版本、函数调用、静态开关及继承输入/设置覆盖。模块库在 `App/Assets/Engine/VFX/Modules`，示例在 `Templates/VFX/Examples`。实例引用使用稳定资产身份，复制依赖时保留对应 `.meta` 文件。

```text
NullusVfxCompile new MySystem.vfx "My System"
NullusVfxCompile compile MySystem.vfx MySystem.compiled.json --backend all
```

`--backend all` 生成后端编译包，不等于 Linux、Vulkan 或 Metal 设备执行已经通过。Player 消费编译程序，无需编辑器模块布局。旧图格式样例保留为运行时/导入测试资源；当前编辑器拒绝不支持的格式，不做自动迁移，也不以 v2 覆盖旧文件。

## 场景播放与查询

Particle System 组件提供 Activate、Stop、Restart、暂停和类型化参数控制。场景运行时播放系统中的发射器，并把拥有场景的查询世界绑定给需要碰撞查询的粒子逻辑；[静态碰撞体](../physics/roadmap.md)提供场景几何。

Sprite 与 Mesh 路径具备可见性、实例化、包围盒和透明排序基础。GPU 包围盒通过异步读回更新，在首份结果到达前使用保守回退；不要把延迟到达的调试统计当作当前画面结果。不同 Mesh slot 的透明绘制不能保证全部粒子形成单一全局顺序。

## 验收与边界

分别检查模块顺序、User 参数、曲线、Undo/Redo、错误恢复、多发射器预览、保存重开、场景组件播放和 Cook 后的独立 Player。GPU 编译、专项设备测试、实际 Editor 操作和成品画面各自记录结果。

完整 Niagara 功能兼容、完整数据接口、更多渲染器和资产升级工具尚未完成。音频、视频与后处理状态见[音频与特效现状及规划](roadmap.md)。
