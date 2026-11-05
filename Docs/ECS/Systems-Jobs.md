# System、Job 与延迟结构变更

System 派生自 `NLS::ECS::System`。用 SystemDescription 声明名称、阶段、读写权限、
Before/After 和主线程约束，再调用 `world.Systems().RegisterSystem<T>()`。
可运行的完整实现见 `Samples/ECS/MassSimulation.cpp`。

```cpp
SystemDescription movement{"Movement"};
movement.mainThread = false;
movement.Writes<Position>(world).Reads<Velocity>(world);
world.Systems().RegisterSystem<Movement>(movement);
```

OnCreate 在注册时执行；OnStart 在开始或重新启用时执行；OnUpdate 按阶段执行。
禁用和停止执行 OnStop；移除或 World 销毁执行 OnDestroy。OnStop/OnDestroy 要求
noexcept。生命周期回调在拥有 World 的线程运行；动态注册/启用操作需要安全点。

System 的查询、直接组件访问和 Resource 访问必须包含在声明集合内。`Uses(query.Description())`
可复用查询的声明。Resource 需要 ReadsResource/WritesResource，不能用实体组件声明替代。
写 Shared/Tag 列被拒绝。普通 GameObject、VM、ImGui、Physics、RHI 适配应保留默认
的 `mainThread = true`；只有纯 CPU 数据系统才应改为 false。

显式依赖先进行稳定拓扑排序；读写冲突沿该拓扑定向，避免与反向的合法 Before/After
产生假环。真正的环、未知依赖和与阶段顺序矛盾的依赖在派发前报错。Read/Read
允许并行，Read/Write 和 Write/Write 形成边。禁用/启用会重新构建依赖图。

## 任务执行

应用初始化共享 `Base::Jobs::JobSystem` 后，调用 `RunPhase(phase, time)` 即使用
现有 Jobs 服务。ECS 不创建线程池，也不负责启动或关闭全局服务。未初始化时 System
串行执行，`context.jobsEnabled` 为 false。独立系统按
依赖前沿并行，主线程系统直接在拥有线程运行。每个前沿 join，整个阶段结束后才回放
结构命令。前沿之间的 barrier 是当前实现的调度取舍，不是完全流水化的逐任务 DAG。

大查询使用 `ParallelEach(callback)` 或 `ParallelForEachChunk(callback)`，通过
`IJobParallelFor` 和 `ScheduleParallelFor` 执行；chunksPerJob 是工作窃取的最小批量，
minimumParallelEntities 以下自动串行；WorldConfig.singleThreaded 提供调试模式。
并行回调捕获的外部计数器/容器也需要同步或独立归约，不能因组件权限正确就假定
外部状态线程安全。没有承诺任意并行浮点归约都确定。

ECS 的派发接口同步返回：已接受工作全部 join 后才释放借用。嵌套等待使用 JobSystem
的等待协助；等待调用者也可能执行纯数据任务。`mainThread` 系统仍由 ECS 直接在拥有
线程调用。禁止同步等待的 Jobs scope 会在提交前报错。显式并行查询在大数据路径要求
共享服务已初始化，小查询或 singleThreaded 路径可直接执行。RequestCancellation 是协作取消，不能中断正在执行的任意 C++
指令；取消和失败会在 join 后传播。不要在任务回调里销毁其 World，也不要把裸组件
引用捕获到通过其他 API 派发、未被本次调用 join 的任务中。回调异常会在全部已接受
任务 join 后重新抛出；未执行完的取消范围也会报错。World 的 jobs 统计记录提交的
系统任务与 ParallelFor 分片数量，不代表实体数。

## 命令缓冲

```cpp
EntityCommandBuffer commands(world);
auto writer = commands.GetWriter(0);
auto deferred = writer.CreateEntity();
writer.AddComponent<Position>(deferred, {1, 2, 3});
auto result = commands.Playback();
auto entity = result.Resolve(deferred);
```

支持创建、销毁、增删/设置组件、enable 切换。命令 payload 拥有自己的值。
按 writer key、writer 内顺序回放；同一 writer 绑定首次记录的线程，不能跨线程复用。
并行 Job 使用不同 key；System 内可从 `context.commands.Fork(uniqueChunkKey)`
取得独立 writer。父 key 的低 32 位必须为零，Fork 的局部 key 小于 UINT32_MAX。

临时引用属于同一缓冲和 epoch；Reset、消费或销毁后旧 writer/引用失效。创建命令
必须在定义的回放顺序中先于使用；跨 writer 的向前引用报错。Destroy 后再 Set 会
给出明确诊断，不会复活实体。Playback 一旦开始即消费；成功命令不会因后续错误
再次执行，错误按命令返回。它是确定顺序的部分提交，不是全缓冲原子事务。

当前临时引用解析用于命令目标；组件 payload 内嵌实体引用的自动 fixup 和
InstantiatePrefab 已接入字段存档/Prefab 加载路径，返回各局部实体的延迟引用；
后续命令可以使用这些引用。内部图引用通过 codec 重映射，详见 Authoring-Baking.md。
