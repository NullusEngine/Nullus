# 生命周期与激活状态

Component 提供一组虚函数作为生命周期回调，Scene 负责在正确时机调用它们。理解顺序与门控条件是避免“回调没触发”类问题的前提。

## 回调顺序

进入 Play 时，对每个激活对象依次执行：

```text
OnCreate  （组件被创建并绑定宿主时，只执行一次）
OnAwake   （对象首次进入运行态，只执行一次）
OnEnable  （每次对象或组件被激活时执行）
OnStart   （首次进入运行态且已 Awake，只执行一次）
```

每帧：

```text
OnUpdate(deltaTime)      普通更新
OnFixedUpdate(deltaTime) 固定步长更新
OnLateUpdate(deltaTime)  普通更新之后
```

退出 Play 时：

```text
OnDisable  （对象或组件被停用时执行）
OnDestroy  （对象或组件被销毁时执行）
```

## 门控条件

- `OnAwake` 与 `OnStart` 只在对象在层级上处于激活状态时执行。
- 对象已经被 Awake 或 Start 过，后续重新激活不会重复调用这两个回调。
- `OnEnable` 每次从“未生效”变为“生效”都会执行，因此在一次运行中可能被调用多次。
- 组件被销毁且此前处于生效状态时，先 `OnDisable` 再 `OnDestroy`。
- 组件只销毁一次；重复销毁请求不会重复触发回调。

## 激活状态

激活状态有两层：

| 层 | 判断 | 说明 |
| --- | --- | --- |
| 自身 | `IsSelfActive()` / `GetActive()` | 只看该对象自己的开关 |
| 层级 | `IsActive()` | 自身与所有父对象都激活才为真 |

组件的生效条件是“自身 `enabled` 且宿主 GameObject 在层级上激活”，即 `IsActiveAndEnabled()`。父对象被停用会连带停用整棵子树上的组件，父对象重新激活时子对象按规则重新触发 `OnEnable`。

`SetActive` 只影响对象自身的开关，不修改父对象状态。

## 组件启用与停用

`SetEnabled(bool)` 只在状态真正变化时触发回调：

- 从生效变为不生效：`OnDisable`
- 从不生效变为生效：`OnEnable`
- 组件已被宿主销毁时，调用被忽略

这套语义保证切换开关不会产生重复的 `OnEnable`。

## 场景切换

`SceneManager` 管理活动场景的加载与切换。编辑器在运行态加载场景时会先停用当前对象的运行态回调，再激活新场景对象，避免两个场景同时驱动同一帧。

编辑器临时对象（例如 Gizmo 与辅助绘制）不参与保存，也不应被脚本依赖。

## 常见问题

**回调没有触发**

先检查宿主对象是否在层级上激活：父对象停用会让整棵子树都不生效。

**`OnEnable` 被调用多次**

这是设计行为。需要“只执行一次”的逻辑应放在 `OnAwake` 或 `OnStart`。

**`OnStart` 没执行但 `OnAwake` 执行了**

说明对象在 `OnAwake` 之后被停用，`OnStart` 会在对象再次激活时补执行。

## 脚本侧回调

C# 与 Lua 的脚本组件使用同一组回调名。C# 侧是 `Nullus.Behaviour` 上的虚方法，Lua 侧是全局函数。写法见[脚本系统概览](../scripting/overview.md)。

## 下一步

- [场景、GameObject 与组件](objects.md)：回到对象模型。
- [脚本系统概览](../scripting/overview.md)：在脚本里使用这些回调。
