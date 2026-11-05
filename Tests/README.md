# 测试入口

普通逻辑、编辑器、ECS 与脚本插件测试统一编译到 `NullusUnitTests`。
运行某个功能的测试时使用 GoogleTest 过滤器，不再为同一份源码创建独立项目：

```powershell
Tools/BuildCleanEnvironment.cmd cmake --build build --config Debug --target NullusUnitTests -- /m:1
build/bin/Debug/NullusUnitTests.exe --gtest_filter="ECS*:SceneSelectionTests.*"
```

以下入口有独立的构建或运行要求：

| 目标 | 保留原因 |
| --- | --- |
| `NullusGpuTests` | 集中运行 `*GpuTests.cpp`，隔离 GPU 设备与执行器的生命周期 |
| `NullusBuildTests` | 打包 CI 使用的无 Editor 链接依赖入口 |
| `NullusPropertyControlsTests` | 验证属性控件只依赖 UI，不依赖 Engine、Editor、反射或 Undo |
| `NullusPluginTestsStatic` / `NullusPluginTestsShared` | 验证两种链接方式和真实插件 DLL 的 ABI |
| `NullusPerformanceTests` | 长耗时、性能与压力验证，仅在 `NLS_BUILD_PERFORMANCE_TESTS=ON` 时构建 |

普通测试、GPU、打包和属性控件的源码各有唯一归属，不在多个测试程序中重复编译。
ECS 的 Editor 资产用例通过 `NLS_ECS_ASSET_EDITOR_TESTS` 加入总测试；示例程序继续独立保留。

```powershell
Tools/BuildCleanEnvironment.cmd cmake --build build --config Debug --target NullusUnitTests NullusGpuTests NullusBuildTests NullusPropertyControlsTests -- /m:1
ctest --test-dir build -C Debug --output-on-failure -R "^(NullusUnitTests|NullusGpuTests|NullusBuildTests|NullusPropertyControlsTests)$"
```

原来 `NullusPhysicsTests`、`NullusUIDesignerTests`、`NullusSceneSelectionTests` 等专用构建目标已合并。
历史验证记录保留当时的命令；当前构建与运行方式以本页为准。查看可用的用例名：

```powershell
build/bin/Debug/NullusUnitTests.exe --gtest_list_tests
```

`Tests/Fixtures` 文件夹中的 ABI DLL、Probe 子进程、C# 程序集是测试输入或辅助进程。
它们必须生成独立产物，由对应测试目标自动构建。SDK 和示例插件打包任务放在 `Plugins`，
图标生成任务放在 `Tools/Brand`，第三方库放在 `ThirdParty`。
