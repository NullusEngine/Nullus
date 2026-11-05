# MetaParser 并发与性能

MetaParser 默认以最多 4 个线程并行解析头文件。每个任务拥有独立的
CppAst/libclang 上下文；模块反射数据与 Runtime 类型目录共用按头文件保存的
反射模型索引，可在多个目标和后续构建之间复用。Irony 注解解析器按线程复用，
T4 模板在构建 MetaParser 时预编译为 C# 转换类，运行时直接执行。
校验、排序、生成与原子写入由主进程执行，生成内容不随任务完成顺序改变。

## 配置

原有 `MetaParser <precompile.json>` 命令保持兼容。可以在配置中增加：

```json
{
  "ParseMode": "threads",
  "ParseJobs": 0,
  "TotalParseJobs": 0,
  "UseCache": true,
  "CacheDirectory": "D:/Code/Nullus/build/Tools/MetaParser/cache"
}
```

| 配置 | 含义 |
| --- | --- |
| `ParseMode: "threads"` | 同一进程内并行解析，默认模式。 |
| `ParseMode: "processes"` | 每个子进程串行解析一批头文件，隔离原生解析器；主进程汇总结果。 |
| `ParseMode: "serial"` | 串行解析，便于对照和排查问题。 |
| `ParseJobs: 0` | 自动选择 `min(4, max(1, 逻辑处理器数 / 2))`，并受任务数量限制。 |
| `ParseJobs: N` | 使用指定的正整数并发度，仍受任务数量限制。 |
| `TotalParseJobs: 0` | 共享目录内全部目标的原生解析预算，自动选择 `min(8, max(1, 逻辑处理器数 / 2))`。 |
| `TotalParseJobs: N` | 全部目标共用指定的原生解析任务上限。 |
| `UseCache: true` | 默认启用持久化反射模型缓存。设为 `false` 可作无缓存对照。 |
| `CacheDirectory` | 所有目标共用的缓存/预算目录。CMake 默认使用构建目录下的 `Tools/MetaParser/cache`。旧 JSON 未设置时使用 `RootDir/Library/MetaParserCache`。 |

CMake 提供 `NLS_META_PARSER_MODE`、`NLS_META_PARSER_JOBS` 两个缓存选项，
写入各模块的 `precompile.json`。同名环境变量优先于 JSON 配置，可在无需
重新配置 CMake 的情况下临时切换：

```powershell
$env:NLS_META_PARSER_MODE = "processes"
$env:NLS_META_PARSER_JOBS = "4"
$env:NLS_META_PARSER_TOTAL_JOBS = "8"
```

`ParseJobs` 控制每次 MetaParser 调用的预算，`TotalParseJobs` 通过进程间锁控制
共享目录内全部目标的原生解析任务总数。各目标应使用相同的总预算配置。
多进程模式会增加启动、模型传输和内存开销，
适合需要进程隔离的场景；小任务通常优先使用线程模式。

目标间并行由 CMake 的构建器调度：每个目标已经拥有独立的生成命令，
没有依赖关系的命令可以同时启动多个 MetaParser。`cmake --build ... --parallel N`
控制构建器的并行度，`ParseJobs` 控制每个 MetaParser 内部的并行度。
例如 4 个目标各自设置 4 个线程、总预算为 8 时，最多同时进行 8 个原生解析任务。
`NLS_ENABLE_PARALLEL_REFLECTION_GENERATION=ON` 会将生成调度与 C++ 编译分开，
使下游生成等待上游生成完成即可开始。
Windows 的 `Tools/BuildCleanEnvironment.ps1` 会规范化 Path/PATH，并保留调用方
指定的 `--parallel N`、`/m:N`，允许构建器并行调度目标。
`Tools/RunRepositoryDotnet.ps1` 仍对构建 .NET 项目的命令设置 `/m:1`、`/nr:false`；
这个限制作用于 SDK 构建，不限制已构建的 MetaParser 同时运行多个实例。
共享的外部桩头采用完整文件原子发布，避免目标之间读取到尚未写完的桩头。

每次成功运行都会输出实际模式、任务数、唯一头文件数，以及扫描、解析、
类型校验和生成阶段的耗时。`UniqueHeaders` 同时包含本模块和 Runtime 类型目录
所需的头文件。
`CacheHits` 包含所有缓存命中，`SharedCacheHits` 是其中从共享索引读取的数量，
`CacheMisses` 表示实际进行的原生解析次数，`TotalParseJobs` 显示全局预算。
`CacheReads`、`CacheDecodes`、`CacheValidations` 记录实际缓存读取、解码和公共校验
次数，`CacheLoadReuses` 表示本批次复用已加载缓存的次数。`SourceAnalyses`、
`MacroChecks` 统计唯一源分析和宏检查。`CacheTime` 是各项跨线程累计时间，
不能直接相加作为批次墙钟耗时。

## 缓存与失效

缓存只保存反射模型和输入指纹。缓存键包含解析器程序集、原生 Clang 版本及
二进制指纹、目标平台、编译参数、宏、包含目录和相关环境。libclang 提供实际
读入的传递包含内容指纹；即使文件大小和时间戳未变，内容修改也会使缓存失效。
目录命名空间指纹同时覆盖新增文件、删除文件和 `__has_include` 探测。

CMake 将自身声明的模块导出宏写入 `VisibilityDefines`。跨目标复用前会检查这些宏
在输入中是否只选择 DLL 导入/导出声明；宏参与字段、类型或其它条件编译时保持
独立的解析上下文。功能宏、ABI 和编译选项的差异也保持隔离。

相同输入的冷启动使用进程间锁合并请求。缓存文件通过校验和检查，并以完整文件
原子发布；损坏、不兼容或过期的记录会重新解析。PCH、模块、响应文件、VFS 覆盖
及 `CompilerOptions` 中的原始包含搜索路径会走无缓存解析；包含目录应通过
`IncludeDirs`、`SystemIncludeDirs` 提供。

同一批次优先尝试兼容的共享记录，复用已解码的数据与公共输入校验结果。
宏检查仍按完整解析上下文隔离，源文件的可见性条件分析按内容快照复用。
缓存生产者写入新记录时会使已加载记录失效；损坏或缺失的缓存不会作为成功结果复用。
生成前仍会对非生成源文件执行一次完整内容复核。

CMake 选项 `NLS_META_PARSER_CACHE`、`NLS_META_PARSER_CACHE_DIR`、
`NLS_META_PARSER_TOTAL_JOBS` 分别控制缓存开关、共享目录和总预算。同名环境变量
优先于 JSON。`NLS_META_PARSER_CACHE=off` 关闭结果缓存，仍遵守总并发预算。
停止构建后可删除缓存目录，下一次运行会重建。
生成结果未变化时保留原文件时间戳，避免触发后续 C++ 重编译。

## 批量生成

多个目标可在同一个宿主中执行，共享 Runtime 目录读取、文件内容指纹和 SDK 目录扫描。
批处理先创建必要桩头，再并发解析所有目标；全部校验通过后，才开始写入各目标生成文件。
每个目标使用独立的模板实例与会话，宏和编译参数仍按原有规则隔离。

创建清单后运行 `MetaParser --batch <batch.json>`：

```json
{
  "Configs": [
    "Runtime/Base/NLS_Base.precompile.json",
    "Runtime/Math/NLS_Math.precompile.json"
  ],
  "TargetJobs": 0
}
```

配置路径相对于清单文件。`TargetJobs: 0` 自动选择最多 4 个目标并发，仍共同受
`TotalParseJobs` 的原生解析预算限制。重复配置路径合并处理；相同或相互包含的
输出目录会被拒绝。`threads`、`serial` 模式共享进程内输入快照，`processes` 模式
保留子进程隔离，并通过磁盘缓存复用结果。

CMake 提供已配置、未排除目标的批量入口：

```powershell
cmake --build build --config Release --target NullusReflectionBatch
```

清单位于 `build/Tools/MetaParser/ReflectionBatch.json`，并发目标数通过
`NLS_META_PARSER_TARGET_JOBS` 配置。该目标执行一次批量调用，成功后更新各目标的
生成戳，适合集中生成多个模块的反射产物。

模板生成器属于 SDK 构建依赖，模板修改会触发预处理与工具重编译；模板错误在构建期
报告。已发布工具的模板执行只需要 .NET Runtime，不需要 .NET SDK。

## 验证

在构建过 MetaParser、已有基础配置的目录中运行：

```powershell
python Tools/MetaParser/tests/validate_parallel.py `
  --parser build/Tools/MetaParser/publish/Release/MetaParser.exe `
  --base-config build/Runtime/Base/NLS_Base.precompile.json `
  --artifacts build/validation/metaparser-parallel
```

脚本使用独立夹具验证串行、线程、进程的输出一致性，Runtime 类型目录复用、
模板会话隔离、重复输入、单进程工作任务、原文件时间戳、头文件修改、
参数错误和解析失败后的输出完整性，并记录三种模式的耗时。
Linux/macOS 使用对应的 `MetaParser` 可执行文件路径。

缓存与全局预算回归使用 `validate_cache.py`，参数与上述命令相同。
真实多目标对照可运行：

```powershell
python Tools/MetaParser/tests/benchmark_targets.py `
  --parser build/Tools/MetaParser/publish/Release/MetaParser.exe `
  --configs build/Runtime/Base/NLS_Base.precompile.json build/Runtime/Math/NLS_Math.precompile.json `
  --artifacts build/validation/metaparser-targets
```

脚本依次测量无缓存、冷缓存、热缓存，并与各目标的无缓存输出逐字节比较。
可以通过 `--parallel-targets N` 设置同时启动的目标数。
