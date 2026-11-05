# ECS 性能测量与优化

`NullusECSBenchmark` 是独立可运行的 Release 基准，默认覆盖 1,000、10,000、
100,000、1,000,000 Entity。它校验移动 checksum 和命令提交数量，分配失败输出
out_of_memory，不虚构吞吐量。编译和运行方式见 [README](README.md)。

```powershell
build/ecs-headless/Release/NullusECSBenchmark.exe 16384 > tmp/ecs-final-benchmark-16k.csv
```

唯一参数为 Chunk payload 字节预算。16 KiB 默认值包含 Entity 数组与组件列；
列元数据、enable 位、shared 对象和实体 slot 表另计。8/16/32 KiB 的测量文件与
本机实际结果见验证记录。它们衡量本机和本次负载，不能推广为所有游戏的性能提升。

基准包含创建、读查询、串行/并行 Position 更新、增删组件及迁移、命令记录/回放、
销毁和空 Chunk 回收。这组历史 Core CSV 不包含 Scene/Prefab；本阶段新增的
ArchiveBenchmark 单独记录在后文。传统 Component 对照尚未实现。

CSV 中的 ns_per_operation 和 operations_per_second 按 operations 列计算；
Position 更新使用 32 次迭代。小于默认并行阈值的查询会自动串行，parallel 一栏
表示调用并行策略，不表示强制创建 worker job。worker 数固定为 4，避免硬件线程
数变化导致一次运行配置不清楚。

known_world_bytes 是已知 Chunk payload、列/enable 元数据、shared 值和 slot
容量的合计，**不是进程峰值内存**。它不包括通用 STL 分配器开销、Registry/System
容器、命令 payload、以及 string/vector 内部的二级分配。完整分配追踪和每帧
allocation profiler 仍未实现。

优先一次指定实体布局，缓存 Query 对象；用 Tag 表达结构条件，避免热路径反复迁移。
Shared 值会分割 Chunk，过多唯一值可能降低利用率；Buffer 的数组头在列中，内容仍
可能离散分配。ChunksPerJob 与 MinimumParallelEntities 应由真实负载测量决定，
避免每个小 Chunk 单独提交一个 Job。

写权限应最小化；只读查询和 Count 不推进变更版本。Changed 使用 Chunk 粒度，
结构迁移和同 Chunk 其他实体写入可能产生保守的额外匹配。Resource 冲突同样会
串行化系统，跨系统访问不应靠隐藏的共享指针绕过声明。

WorldConfig.enableStatistics 可关闭累计计时；变更版本和安全检查仍然保留。
统计默认累计，ResetFrameStatistics 提供显式重置；尚未自动生成完整的每帧 allocation
记录。Archetype 复用目前使用规范化签名的有序 map，进一步的 hash 索引是优化项。
空 Chunk 在同一 Archetype 内复用；TrimEmptyChunks 按配置保留少量空 Chunk，
Cleanup 阶段也执行该策略。实体 slot 表保留容量以复用 ID，回收 Chunk 不等于释放
整个 World 的全部容量。

## 第一阶段 Core 实际结果（2026-10-29，历史 TaskGraph 版本）

环境为 Windows x64、MSVC 19.44.35228、Release/C++20，CPU 标识为
`AMD64 Family 25 Model 97 Stepping 2, AuthenticAMD`，32 个逻辑处理器，TaskGraph
使用 4 个 worker。8/16/32 KiB 依次各运行一次，没有绑定 CPU 核心、做统计重复或
计算置信区间。测量前仍能观察到其他工作在运行 MSBuild/cl；没有终止它们或承诺
独占整台机器。以下是本次环境下的实测，不能据此宣布某个 Chunk 大小普遍最优。

16 KiB 默认布局，更新耗时是 CSV 中 32 次更新总耗时除以 32；创建是整个批次耗时：

| Entity 数量 | 创建 ms | 串行更新 ms/次 | 并行策略更新 ms/次 | 创建后 known_world_bytes |
| ---: | ---: | ---: | ---: | ---: |
| 1,000 | 0.157900 | 0.011803 | 0.011478 | 100,848 |
| 10,000 | 1.195600 | 0.113134 | 0.065862 | 996,624 |
| 100,000 | 10.073700 | 1.151191 | 0.335528 | 10,693,440 |
| 1,000,000 | 100.751100 | 13.070434 | 3.962325 | 90,882,912 |

1,000 个实体低于默认并行阈值，两列均走串行执行路径。这里的移动仅是三维
Position += Velocity，不包含真实游戏渲染、物理、脚本或 GPU 工作。

16 KiB、1,000,000 Entity 的其余操作：

| 操作 | 整批耗时 ms |
| --- | ---: |
| 只读 Query | 9.495400 |
| 添加组件及迁移 | 191.602000 |
| 删除组件及迁移 | 148.376400 |
| 记录 1,000,000 条 Set 命令 | 197.945300 |
| 回放上述命令 | 118.276600 |
| 销毁全部 Entity | 35.691500 |
| 回收空 Chunk | 8.221200 |

跨 Chunk 预算的同一百万实体负载：

| Chunk KiB | 创建 ms | 串行 32 次更新 ms | 并行 32 次更新 ms | 创建后 Chunk 数 | 创建后 known_world_bytes |
| ---: | ---: | ---: | ---: | ---: | ---: |
| 8 | 103.614700 | 433.878600 | 122.201500 | 4,902 | 91,413,648 |
| 16 | 100.751100 | 418.253900 | 126.794400 | 2,445 | 90,882,912 |
| 32 | 107.266000 | 420.011500 | 136.649200 | 1,222 | 90,651,280 |

原始数据：[8 KiB CSV](BenchmarkResults-8k.csv)、[16 KiB CSV](BenchmarkResults-16k.csv)、
[32 KiB CSV](BenchmarkResults-32k.csv)，每份 40 条测量。迁移后的空 Chunk 尚未
Trim，因此默认布局的已知字节数升到 135,563,336；销毁不会立即释放 slot 表容量。
Trim 后保留两个空 Chunk，known_world_bytes 为 50,426,888。上述数字均不是进程
峰值，也不包含 ECB 的全部命令内存。示例 System 的端到端 CPU 更新测量见
[Validation.md](Validation.md)。

## Phase 5 Scene/Prefab 实测

```powershell
cmake --build build/ecs-headless --config Release --target NullusECSArchiveBenchmark -- /m:1
build/ecs-headless/Release/NullusECSArchiveBenchmark.exe
```

默认运行四种规模；可传最大实体数限定测试。百万规模执行前在 Windows 查询可用物理
内存，少于 8 GiB 时明确输出未执行状态。此次四种规模全部运行，36 条结果均为 ok，
checksum、创建/销毁计数均校验通过。原始数据见 [ArchiveBenchmarkResults.csv](ArchiveBenchmarkResults.csv)。
这是 Release 单次顺序测量，无核心绑定，期间有其他任务及后台编译；没有做统计重复。

输入为一个三维 Position 组件，Scene 测试完整的实体文件；Prefab 使用双实体模板，
批量生成到表中所列实体总数（百万实体对应 500,000 个实例）。该模板不含渲染或资源
加载，不能代表带材质、物理、动画和脚本的完整游戏场景。

| 操作 | 100,000 Entity ms | 1,000,000 Entity ms |
| --- | ---: | ---: |
| 捕获 Scene 数据 | 464.7472 | 4,428.5893 |
| 编码 JSON | 1,782.5637 | 18,056.7695 |
| 解析及校验 JSON | 2,239.8406 | 16,807.8331 |
| 写文件（包含再次编码和原子替换） | 1,823.4382 | 14,836.7917 |
| 读文件（包含解析和校验） | 2,205.7298 | 24,263.0785 |
| 创建运行时实体和组件 | 357.6654 | 4,007.2842 |
| Scene owner 卸载 | 4.1760 | 44.4009 |
| Prefab 批量生成 | 273.8658 | 3,080.6121 |
| Prefab 实例销毁 | 17.2121 | 173.6537 |

百万实体文本为 **432,889,161 bytes**，超过默认 256 MiB 存档限制；本基准显式把
maxBytes 提升到 2 GiB。available_memory_before 是执行该规模前的系统可用内存，
不是本进程峰值。当前 JSON 每实体元数据和 DOM/DTO/组件值分配仍然昂贵；更紧凑的
Cook 产物、字段布局/分块与批量加载是后续优化目标。

基准发现并修复了真实的解析复杂度问题：本仓库 JSON 库的 callback DOM 路径在
end_object 时扫描父容器寻找 discarded 节点（ThirdParty/Json/json.hpp:7105）。
即使回调永不丢弃节点，大实体数组仍被反复扫描。ECS 当前先通过 SAX 检查重复键、
深度和数值，再走普通 DOM 解析，没有修改第三方库，也没有移除校验。

[修复前原始记录](ArchiveBenchmarkBeforeParserFix.csv) 中 100,000 Entity 的解析为
27,279.7443 ms；该运行的百万解析被主动中止，没有伪造其完成行。另一次诊断的
十万规模为 37,515.9822 ms。当前最终记录为 2,239.8406 ms。测量背景负载不同，
不据此作通用加速比保证；二次扫描的来源和替代路径可以直接在源码中核对。

## JobSystem Core results (2026-10-29)

Current CPU dispatch uses Base Jobs with four foreground workers and one background
worker; Complete may also assist on the waiting caller. The Release 16 KiB run
produced [40 successful rows](JobSystemBenchmarkResults-16k.csv), covering all four
entity counts. The host has 32 logical processors and the same AMD identifier
listed above. Other Nullus builds remained active; no CPU affinity or statistical
confidence interval was collected. A preliminary run overlapped other samples;
the retained CSV was rerun sequentially after those samples finished. Older
TaskGraph numbers are historical measurements with different background load and
scheduler code and should not be used to isolate a scheduler speedup/regression.

| Entities | Create ms | Serial update ms/iteration | Jobs update ms/iteration |
| ---: | ---: | ---: | ---: |
| 1,000 | 0.290400 | 0.030556 | 0.028144 |
| 10,000 | 2.539800 | 0.281563 | 0.192066 |
| 100,000 | 21.283100 | 2.661400 | 0.868934 |
| 1,000,000 | 247.107700 | 31.733044 | 7.626150 |

Update columns divide the CSV's 32-iteration total by 32. The 1k row uses the
small-query serial fallback in both columns. All rows validate data; the workload
is only Position += Velocity, with no rendering/physics/VM/GPU work. SceneStreaming
uses the existing archive format and does not change its measured JSON costs.
Pump commits whole chunks, so splitting authoring output into suitable chunks is
still required to control owner-thread stalls. Entity-level time slicing and a
compact cooked representation remain open work.
