# Nullus 反射绑定工作流

本文档说明当前仓库中已经接入并验证通过的反射生成流程。

## 构建流程

- `Tools/MetaParser` 是当前维护中的反射代码生成器。
- 运行时模块会通过 [Runtime/CMakeLists.txt](D:/VSProject/Nullus/Runtime/CMakeLists.txt) 里的 `nls_add_meta_generation(...)` 在正式编译前调用 MetaParser。
- 生成文件统一写入 `Runtime/<Module>/Gen/MetaGenerated.h/.cpp`。
- 在 Windows 上，生成步骤会先准备好 MetaParser 运行所需的 `libclang` 运行时，再启动生成器。

## 注册模型

现在每个运行时模块都会生成自己独立的反射注册入口。

例如：

- `RegisterReflectionTypes_NLS_Base`
- `RegisterReflectionTypes_NLS_Core`
- `RegisterReflectionTypes_NLS_Engine`

模块启动代码通过 `NLS_META_GENERATED_REGISTER_FUNCTION` 调用本模块对应的生成注册函数，从而避免不同模块之间在链接阶段发生符号冲突。

## 当前已覆盖的引擎类型

当前 Engine 侧由 MetaParser 覆盖的类型包括：

- `NLS::Engine::Components::Component`
- `NLS::Engine::Components::TransformComponent`
- `NLS::Engine::Components::CameraComponent`
- `NLS::Engine::Components::LightComponent`
- `NLS::Engine::Components::MeshRenderer`
- `NLS::Engine::Components::MaterialRenderer`
- `NLS::Engine::Components::SkyBoxComponent`
- `NLS::Engine::GameObject`
- `NLS::Engine::SceneSystem::Scene`

## 解析说明

MetaParser 默认优先使用 `CppAst`。不过目前仍保留了文本回退解析路径，用来处理少数在 Windows 上会触发 `CppAst` 容器异常的头文件。

当前会走回退路径的文件有：

- `Runtime/Engine/Components/MeshRenderer.h`
- `Runtime/Engine/GameObject.h`
- `Runtime/Engine/SceneSystem/Scene.h`

这些头文件虽然没有走完整的 `CppAst` 路径，但仍然可以生成可用的类型和方法绑定，并且已经被测试覆盖。

## 验证方式

当前仓库保留两层反射验证：

- `Tools/ReflectionTest`，用于更聚焦的反射注册冒烟验证
- `Tests/Unit/NullusUnitTests`，作为本地开发和 GitHub Actions 正式使用的 `CTest` 入口

`NullusUnitTests` 会链接：

- `NLS_Base`
- `NLS_Core`
- `NLS_Engine`

它会检查：

- 类型是否完成注册
- 关键方法是否完成注册
- 关键字段是否完成注册
- 组件继承关系是否完成注册
- MetaParser 生成的绑定内容是否符合预期

## 已验证命令

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

期望输出：

```text
100% tests passed
```
