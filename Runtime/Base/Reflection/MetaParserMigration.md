# MetaParser 迁移说明（已切换到 C# 路线）

## 结论（重要）

**C++ MetaParser 路线已移除**。当前工程仅支持：

- C# `Tools/MetaParser/src/Main.cs`
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

## 构建链路（单文件粒度）

`Runtime/Base/CMakeLists.txt` 会扫描 `Runtime/**/*.h|hpp|inl`：

- 仅命中“带反射标记 + 继承 Object”的头文件
- 每个命中头文件生成一个 `*.gen.cpp`
- 无标记头文件不生成产物

生成目录：`build/Runtime/Base/Generated/`

典型产物：

- `MetaGenerated.h`
- `MetaGenerated.cpp`（聚合入口，保持兼容）
- `<header-id>.gen.cpp`（每个命中头文件一个）

聚合注册接口保持：

- `RegisterReflectionTypes(ReflectionDatabase&)`
- `RegisterReflectionTypes()`

---

## 标记机制

解析器定义 `__REFLECTION_PARSER__`，支持以下写法：

1. 用户宏方案（推荐）
   - `__attribute__((annotate(...)))`
   - `CLASS(...) / STRUCT(...)`（在 parser 模式下会展开到 annotate）
2. 旧写法（兼容过渡）
   - `Meta(...)`

并且类型需继承 `NLS::meta::Object`（或 `Object`）。

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
