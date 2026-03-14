# Nullus 测试说明

本文档说明 Nullus 当前本地开发和 GitHub CI 使用的测试入口。

## 测试模块

- `Tests/Unit/NullusUnitTests` 是正式接入 `CTest` 的单元测试可执行文件。
- `Tools/ReflectionTest` 仍然保留，作为更轻量的反射冒烟验证工具。

`NullusUnitTests` 当前覆盖：

- Base、Core、Engine 关键类型的运行时反射注册验证
- `Runtime/*/Gen/MetaGenerated.h/.cpp` 生成内容验证
- MetaParser 集成回归问题验证，这类问题通常只会在构建阶段暴露

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
