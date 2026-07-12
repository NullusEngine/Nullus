# Nullus 测试说明

本文档说明 Nullus 当前本地开发和 GitHub CI 使用的测试入口。

## 测试模块

- `Tests/Unit/NullusUnitTests` 是正式接入 `CTest`、标签为 `behavior` 的单元测试可执行文件。
- `Tests/Performance/NullusPerformanceTests` 是按需构建并注册到 `CTest`、标签为 `performance` 的性能和压力测试可执行文件。
- `Tools/ReflectionTest` 保留为更轻量的反射冒烟验证工具。

`NullusPerformanceTests` 默认既不会构建，也不会注册到 `CTest`。可重复、确定性的行为回归应继续放在
`Tests/Unit`；性能测量、压力场景和真实资产负载应放在 `Tests/Performance`，避免默认行为测试受机器性能或
本地资产状态影响。

`NullusUnitTests` 当前覆盖：

- Base、Core、Engine 关键类型的运行时反射注册验证
- `Runtime/*/Gen/MetaGenerated.h/.cpp` 生成内容验证
- MetaParser 集成回归问题验证，这类问题通常只会在构建阶段暴露

当前反射相关单元测试已经按桶拆分为：

- 运行时注册桶：`ReflectionRuntimeCoreTests.cpp`、`ReflectionRuntimeEngineTests.cpp`
- 生成产物桶：`MetaParserGenerationModuleTests.cpp`、`MetaParserGenerationEngineTests.cpp`、`MetaParserGenerationDataTests.cpp`
- 共享装配 / 断言辅助：`ReflectionRuntimeTestFixture.h`、`ReflectionTestUtils.h`

`ReflectionTest` 当前负责：

- 以较小成本验证当前反射系统没有整体断裂
- 覆盖每类主要反射模式的哨兵类型，而不是重复整份详细类型清单
- 覆盖少量消费者级冒烟场景，例如：
  - `meta::Type` 驱动组件查询
  - Inspector 枚举选项的反射元数据生成

## 当前建议的职责边界

### `NullusUnitTests`

适合承载：

- 更细粒度的运行时反射断言
- 生成片段和模板行为断言
- 外部反射、私有绑定、自动属性、显式属性等模式化回归
- 需要明确失败定位的精确测试
- 按桶继续扩展时，优先往现有 runtime / generation 分类里追加，而不是重新堆回一个混杂的大文件

### `ReflectionTest`

适合承载：

- 模式级哨兵验证
- 最小成本确认注册链路仍然可用
- 少量跨层消费者冒烟验证

不建议让 `ReflectionTest` 再维护一份与 `NullusUnitTests` 等长的详细覆盖清单，否则两边会持续重复。

## 本地构建与测试

### 默认行为测试

Windows/MSVC 的默认行为测试工作流使用仓库根目录下的 `Build` 构建目录：

```powershell
cmake -S . -B Build -G "Visual Studio 17 2022" -A x64 -DNLS_BUILD_TESTS=ON
cmake --build Build --config Debug --target NullusUnitTests ReflectionTest -- /m:4
ctest --test-dir Build -C Debug -L behavior --output-on-failure
```

`NLS_BUILD_PERFORMANCE_TESTS` 默认保持 `OFF`，因此上面的配置只注册确定性的 `behavior` 测试。
`ReflectionTest` 会随命令一起构建，但它是独立的反射冒烟工具，不属于 `behavior` CTest 标签。
Windows 路径大小写不敏感；本文统一使用仓库现有输出路径约定中的 `Build` 写法。

如果需要调整并行度，可显式传入 `/m:<jobs>`；使用 `build_windows.bat` 时默认会按
`NUMBER_OF_PROCESSORS` 设置 MSBuild worker 数，也可设置 `NLS_BUILD_JOBS=<jobs>`
覆盖。例如复现串行构建问题时设置 `NLS_BUILD_JOBS=1`。

默认 Windows/MSVC 配置会启用目标内部并行编译 `/MP`，用于加速 `NLS_Render`、
`NullusUnitTests` 这类大目标的全量编译。如需诊断编译器并行问题，可在配置时关闭：

```powershell
cmake -S . -B build -G "Visual Studio 17 2022" -A x64 -DNLS_ENABLE_MSVC_MULTIPROCESSOR_COMPILATION=OFF
```

如果外层 MSBuild `/m` 已经很高，而 CPU 仍然跑不满或出现 PDB/IO 竞争，可单独限制
MSVC `/MP` 的目标内 worker 数：

```powershell
cmake -S . -B build -G "Visual Studio 17 2022" -A x64 -DNLS_MSVC_MP_JOBS=8
```

使用 `build_windows.bat` 时可通过环境变量透传：

```bat
set NLS_BUILD_JOBS=20
set NLS_MSVC_MP_JOBS=8
build_windows.bat Debug x64
```

默认配置也会为主要 Runtime、Project 和 Tests C++ 目标启用窄范围预编译头。
预编译头只应放入稳定、常用、低耦合的标准库或基础项目头，不应把 Assimp、ImGui、
Windows、DX12 或 `Json/json.hpp` 这类重依赖加入 PCH 来掩盖公共头文件耦合问题。
如需诊断 include 顺序、宏泄漏或单文件编译问题，可关闭：

```powershell
cmake -S . -B build-no-pch -G "Visual Studio 17 2022" -A x64 -DNLS_ENABLE_PCH=OFF
cmake --build build-no-pch --config Debug --target NLS_Render NullusUnitTests ReflectionTest
```

公共头文件应优先保持轻量：只有当 public 签名确实需要具体类型、枚举值、默认参数或
按值成员时，才在 `.h` 中包含 Assimp、ImGui、Windows、DX12 或 `Json/json.hpp`
等重依赖。实现细节使用的第三方、平台和后端头文件应放到对应 `.cpp`，或拆到更窄的
detail/settings 头中；指针、引用、`std::shared_ptr<T>` 参数优先使用前置声明。

C++20 Modules 当前只作为实验性试点入口保留，默认关闭。由于根工程仍支持
CMake 3.18+，而 Modules 对 CMake、生成器和编译器版本要求更严格，启用前必须先在
独立构建目录验证工具链矩阵：

```powershell
cmake -S . -B build-modules-pilot -G "Visual Studio 17 2022" -A x64 -DNLS_ENABLE_CXX_MODULES=ON
cmake --build build-modules-pilot --config Debug --target NLS_ModulesPilot
```

当前试点只在 Windows MSVC + Visual Studio 生成器下创建 `NLS_ModulesPilot`，
用于验证一个独立的 `Runtime/Math/_DISABLED/ModulesPilot` 低依赖模块链路。它位于
运行时 glob 排除目录中，不会链接进运行时库，也不会参与 MetaParser 反射生成。
其它生成器或平台启用该选项时会跳过试点并继续使用普通头文件路径。

默认 Assimp 配置只编译 Nullus 默认路径需要的 OBJ/glTF 导入器，并关闭
Assimp exporter；FBX 默认仍走 Autodesk FBX SDK。需要按资产选择 Assimp 读取
`.fbx` 时，使用窄开关只启用 Assimp FBX importer：

```powershell
cmake -S . -B build-assimp-fbx -G "Visual Studio 17 2022" -A x64 -DNLS_ENABLE_ASSIMP_FBX_IMPORTER=ON
```

如果需要做第三方格式兼容性验证，可启用完整 Assimp 格式覆盖：

```powershell
cmake -S . -B build-assimp-full -G "Visual Studio 17 2022" -A x64 -DNLS_ASSIMP_BUILD_ALL_FORMATS=ON
```

全量 Debug 构建基线记录：

- 原始 fresh build：`10:42`
- 启用窄开关后的 Assimp OBJ/FBX/glTF import-only：`10:22`
- 启用窄开关后的 Assimp OBJ/FBX/glTF import-only + MSVC `/MP /EHsc`：`4:29`

Linux / macOS 的等价工作流为：

```bash
cmake -S . -B Build -G Ninja -DCMAKE_BUILD_TYPE=Debug -DNLS_BUILD_TESTS=ON
cmake --build Build --target NullusUnitTests ReflectionTest
ctest --test-dir Build -L behavior --output-on-failure
```

### 按需性能测试

性能测试使用独立构建目录，避免复用已配置为行为测试的缓存；性能测量统一使用 `Release`：

```powershell
cmake -S . -B Build-Performance -G "Visual Studio 17 2022" -A x64 -DNLS_BUILD_TESTS=OFF -DNLS_BUILD_PERFORMANCE_TESTS=ON
cmake --build Build-Performance --config Release --target NullusPerformanceTests -- /m:4
ctest --test-dir Build-Performance -C Release -L performance --output-on-failure
```

Linux / macOS 使用单配置生成器时，在配置命令中补充 `-DCMAKE_BUILD_TYPE=Release`，构建同一
`NullusPerformanceTests` 目标，并以 `ctest --test-dir Build-Performance -C Release -L performance
--output-on-failure` 运行。`-C Release` 对 Visual Studio 等多配置生成器用于选择配置，对单配置生成器
可保留为统一写法。

性能测试目标中的合成工作负载随 `performance` 标签运行；真实资产基准仍保留额外的环境变量开关：

- `NLS_RUN_NEWSPONZA_PREFAB_PERF=1`：启用当前 NewSponza prefab 导入、实例化和 prepared-cache 基准。
- `NLS_RUN_PNG_IMPORT_PERF=1`：启用当前 NewSponza 独立 PNG 导入基准。

未设置这些 `NLS_RUN_*_PERF` 开关时，对应真实资产用例会跳过；它们不是 CMake 配置选项。真实资产缺失时
用例同样会跳过。需要比较耗时或生成性能报告时应使用 `Release` 构建，避免把 Debug 开销混入测量结果。

### 测试专用 hooks

`NLS_ENABLE_TEST_HOOKS` 是缓存字符串，支持 `AUTO`、`ON`、`OFF`：

- `AUTO`（默认）：当 `NLS_BUILD_TESTS` 或 `NLS_BUILD_PERFORMANCE_TESTS` 任一开启时启用 hooks；两者都关闭时禁用。
- `ON`：无论是否构建测试目标都强制暴露测试专用 runtime hooks。
- `OFF`：无论测试构建开关为何都禁用 hooks；依赖这些 hooks 的测试可能跳过相应检查。

实现仍接受旧 Boolean 缓存常见的 `ON/TRUE/YES/1` 与 `OFF/FALSE/NO/0` 值。已有构建目录若把该项保存为
Boolean，建议重新配置时传入 `-DNLS_ENABLE_TEST_HOOKS:STRING=AUTO`，迁移到当前三态语义。

如果需要直接运行单元测试可执行文件：

- Windows：`Build/bin/Debug/NullusUnitTests.exe`
- Linux / macOS：`Build/bin/NullusUnitTests`

## CI 行为

GitHub Actions 现在会在正常构建之后继续执行行为验证，覆盖以下平台：

- Windows
- Linux
- macOS

这意味着拉取请求现在需要同时通过：

- 工程构建
- `NullusUnitTests` 中的反射系统与 MetaParser 验证
- `ReflectionTest` 反射冒烟验证

默认 CI 只启用 `NLS_BUILD_TESTS`，不构建或注册 `NullusPerformanceTests`；性能、压力和真实资产负载需按
上述 `performance` 工作流单独运行。

## 说明

- 这套测试接入尽量保持在项目侧完成，避免直接修改第三方源码。
- JSON 解析使用仓库内置的 `ThirdParty/Json/json.hpp`，不再依赖额外 JSON 子模块。
