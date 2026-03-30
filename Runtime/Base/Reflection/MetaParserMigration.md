# MetaParser 迁移说明（已切换到 C# 路线）

> Current verified workflow: `Docs/Reflection/ReflectionWorkflow.zh-CN.md` and `Docs/Reflection/ReflectionWorkflow.en.md`

## 结论（重要）

**C++ MetaParser 路线已移除**。当前工程仅支持：

- C# `Tools/MetaParser/src/MetaParserTool.Core.cs`
- `MetaParser.csproj`
- CppAst（libclang）
- T4 模板 `Templates/MetaGenerated.cpp.tt`

不再构建、调用或依赖 `MetaParserLegacy`（C++ 可执行）。

---

## 新依赖

1. **.NET SDK 8.0+**（必须）
2. CppAst.NET（由 `MetaParser.csproj` 引用）
3. T4 模板文件：
   - `Tools/MetaParser/src/Templates/MetaGenerated.cpp.tt`

> 若缺少 dotnet，CMake 配置阶段会直接失败并给出错误，不再回退到 C++ 解析器。

---

## 构建链路（当前正式路径）

运行时模块通过 `Runtime/CMakeLists.txt` 中的 `nls_add_meta_generation(...)` 接入 C# MetaParser：

- 扫描各模块 `Runtime/<Module>/` 下的头文件
- 命中当前维护中的反射输入模式，包括内联反射类型、反射枚举和类外反射声明
- 生成聚合产物 `MetaGenerated.h / MetaGenerated.cpp`
- 生成结果直接写入模块源码树下的正式目录：`Runtime/<Module>/Gen/`

当前正式产物目录示例：

- `Runtime/Base/Gen/MetaGenerated.h`
- `Runtime/Base/Gen/MetaGenerated.cpp`
- `Runtime/Core/Gen/MetaGenerated.cpp`
- `Runtime/Engine/Gen/MetaGenerated.cpp`

说明：

- 旧的 `build/Runtime/Base/Generated/` 属于迁移过程中的历史/中间产物，不再视为当前正式输出目录
- 当前构建实际编译的是 `Runtime/<Module>/Gen/*`

当前模块级 link 入口保持为：

- `LinkReflectionTypes_NLS_Base`
- `LinkReflectionTypes_NLS_Core`
- `LinkReflectionTypes_NLS_Engine`

运行时通过 `NLS_META_GENERATED_LINK_FUNCTION` 接入本模块的生成注册入口。

---

## 标记机制

解析器定义 `__REFLECTION_PARSER__`，支持以下写法：

1. 当前维护中的内联反射宏
   - `CLASS(...)`
   - `STRUCT(...)`
   - `ENUM(...)`
   - `PROPERTY(...)`
   - `FUNCTION(...)`
   - `GENERATED_BODY(...)`
2. 类外反射声明
   - `MetaExternal(...)`
   - `REFLECT_EXTERNAL(...)`
   - `REFLECT_PRIVATE_*`
3. 旧写法
   - `Meta(...)`

注意：

- 当前仓库里带 `GENERATED_BODY()` 的反射类型主体主要走维护中的文本解析路径
- 不再应该把“CppAst-first”当成这类类型主体的默认口径

---

## 标准构建流程

```bash
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug
cmake --build build --target ReflectionTest -j4
./build/bin/ReflectionTest
```

---

## 常见排障

### 1) `dotnet: not found` 或 CMake 报缺少 dotnet

安装 .NET 8 SDK 后重新配置：

```bash
# Ubuntu / WSL (Microsoft feed)
wget https://packages.microsoft.com/config/ubuntu/24.04/packages-microsoft-prod.deb -O packages-microsoft-prod.deb
sudo dpkg -i packages-microsoft-prod.deb
sudo apt-get update
sudo apt-get install -y dotnet-sdk-8.0

dotnet --info
```

然后：

```bash
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug
```

### 2) 某头文件未生成 `.gen.cpp`

检查：

- 是否存在标记（annotate / CLASS / STRUCT / Meta）
- 类型是否继承 `Object`
- 头文件是否位于 `Runtime/` 下

### 3) 修改模板未生效

`Runtime/Base/CMakeLists.txt` 已把 `Templates/MetaGenerated.cpp.tt` 加到生成依赖。
重新 `cmake --build` 即可触发对应头文件的再生成。
