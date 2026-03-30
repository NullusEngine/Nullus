# Nullus 测试说明

本文档说明 Nullus 当前本地开发和 GitHub CI 使用的测试入口。

## 测试模块

- `Tests/Unit/NullusUnitTests` 是正式接入 `CTest` 的单元测试可执行文件。
- `Tools/ReflectionTest` 保留为更轻量的反射冒烟验证工具。

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

Windows：

```powershell
cmake -S . -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Debug --target NullusUnitTests -- /m:1
ctest --test-dir build -C Debug --output-on-failure
```

Linux / macOS：

```bash
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug
cmake --build build
ctest --test-dir build --output-on-failure
```

如果需要直接运行单元测试可执行文件：

- Windows：`Build/bin/Debug/NullusUnitTests.exe`
- Linux / macOS：`Build/bin/NullusUnitTests`

## CI 行为

GitHub Actions 现在会在正常构建之后继续执行 `ctest`，覆盖以下平台：

- Windows
- Linux
- macOS

这意味着拉取请求现在需要同时通过：

- 工程构建
- `NullusUnitTests` 中的反射系统与 MetaParser 验证

## 说明

- 这套测试接入尽量保持在项目侧完成，避免直接修改第三方源码。
- `ThirdParty/json11` 必须指向可正常拉取的子模块提交，否则干净的 CI 拉取会在测试开始前失败。
