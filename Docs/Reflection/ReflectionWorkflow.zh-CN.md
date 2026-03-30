# Nullus 反射工作流

这是 Nullus 当前维护中的中文反射使用说明。

English version: [ReflectionWorkflow.en.md](./ReflectionWorkflow.en.md)

## 总览

Nullus 当前维护中的反射路径只有一条：

- 在运行时头文件里声明反射输入
- 正常走 CMake 构建，让 `Tools/MetaParser` 在构建期解析这些声明
- 生成胶水代码到 `Runtime/<Module>/Gen/`
- 运行时通过生成出来的模块 link 函数把类型和成员注册进反射数据库

不要手改 `Runtime/*/Gen/` 下的任何文件。

## 当前系统的真实行为

### 支持的输入模式

当前仓库真正支持并维护的是这几种写法：

1. 内联反射类型：

```cpp
CLASS() class MyType : public NLS::meta::Object
{
public:
    GENERATED_BODY()
};
```

2. 反射枚举：

```cpp
ENUM() enum class MyMode : uint8_t
{
    A,
    B
};
```

3. `FUNCTION()` 标记的 `GetXxx` / `SetXxx` 自动属性
4. `PROPERTY(...)` 显式属性
5. `MetaExternal(...)` + `REFLECT_EXTERNAL(...)` 类外反射声明
6. `REFLECT_PRIVATE_*` 私有类外绑定

### 解析路径

结合当前 `Tools/MetaParser` 实现，仓库里真正生效的规则是：

- 只要反射类型主体里出现 `GENERATED_BODY()`，当前主路径就是**文本解析**
- 顶层 `ENUM()` 也走文本提取
- `MetaExternal(...)` / `REFLECT_EXTERNAL(...)` 走类外声明解析
- `CppAst` 仍然存在，但它不是当前仓库里大多数反射类主体的主路径

所以旧文档里“默认优先 `CppAst`，文本只是回退”的表述，已经不适用于今天这套维护中的工作流。

### 运行时注册入口

每个运行时模块都会生成自己的 link 函数，例如：

- `LinkReflectionTypes_NLS_Base`
- `LinkReflectionTypes_NLS_Core`
- `LinkReflectionTypes_NLS_Engine`

运行时数据库初始化时会调用生成出来的模块 link 宏，然后再通过反射模块注册器完成详细注册。

## 什么样的类型应该注册到反射

坚持“**消费者驱动**”规则：只有当前仓库里真的有运行时元数据消费者时，类型才值得反射。

当前最主要的消费者是：

- Inspector 这类编辑器读写界面
- 序列化
- `meta::Type` 驱动的动态创建、查询、派生关系判断
- 已反射属性所依赖的值类型

不要因为一个类型是 public，就默认把它反射出来。

### 快速准入规则

只要下面任意一个问题的答案是“是”，通常就应该反射：

| 问题 | 是否建议反射 | 原因 |
| --- | --- | --- |
| 编辑器是否需要查看或编辑它？ | 是 | 反射应提供稳定的元数据面 |
| 序列化是否需要遍历它的字段？ | 是 | 反射字段就是维护中的序列化面 |
| 运行时代码是否通过 `meta::Type` 创建、查询或比较它？ | 是 | 类型需要稳定的运行时元数据 |
| 它是否是别的已反射属性所依赖的值类型或枚举？ | 是 | 已反射属性必须依赖可反射值类型 |
| 它是否只是缓存、辅助实现、临时状态或当前消费者无法处理的资源句柄？ | 通常否 | 这类类型会增加噪音而不会给真实消费者带来收益 |

## 应该反射什么

### 类和结构体

应该反射：

- 编辑器要展示和修改稳定状态的组件或数据结构
- 序列化需要遍历字段的结构体
- 运行时要通过 `meta::Type` 查询、比较、创建的类型
- 已反射字段所依赖的稳定值类型

通常不应该反射：

- 临时缓存
- 内部生命周期状态
- 纯实现细节辅助类型
- 当前消费者还无法安全编辑或序列化的资源指针 / 重对象句柄

### 枚举

以下枚举应该反射：

- 会作为已反射字段类型出现的枚举
- 编辑器需要显示为离散选项的枚举
- 需要稳定序列化名字或做反射查找的枚举

### 属性

属性应该代表稳定、可被消费者理解的状态。

优先模式：

1. `FUNCTION()` 标记的 `GetXxx` / `SetXxx` 自动属性
2. 暴露名和访问器命名不一致时，用 `PROPERTY(...)`
3. 类型本身不适合改声明时，用类外声明

不建议反射：

- 内部缓存值
- 强副作用但看起来像普通字段的接口
- 消费者无法处理的复杂资源指针

### 函数

应该反射：

- 属性 getter / setter
- 有意暴露给运行时元数据访问的少量操作
- 需要做元数据查找的入口函数
- 作为生成器支持样例保留下来的关键函数

不建议反射：

- 生命周期回调
- 纯内部辅助函数
- 没有元数据消费者的实现细节方法

## 典型写法

### 模式 1：自动属性

```cpp
CLASS() class CameraComponent : public Component
{
public:
    GENERATED_BODY()

    FUNCTION()
    void SetFov(float value);

    FUNCTION()
    float GetFov() const;
};
```

因为命名对齐，`GetFov` + `SetFov` 会自动形成属性 `fov`。

### 模式 2：显式属性

当暴露名和访问器命名对不上时，用显式属性：

```cpp
PROPERTY(name = active, getter = IsSelfActive, setter = SetActive)
```

当前仓库的典型例子：

- `GameObject.active`
- `MeshRenderer.model`

### 模式 3：类外反射声明

当类型本身不适合加内联宏时，用：

```cpp
MetaExternal(NLS::Maths::Vector3)

REFLECT_EXTERNAL(
    NLS::Maths::Vector3,
    Fields(
        REFLECT_FIELD(float, x),
        REFLECT_FIELD(float, y),
        REFLECT_FIELD(float, z)
    ),
    Methods(
        REFLECT_METHOD_EX(Length, static_cast<float(NLS::Maths::Vector3::*)() const>(&NLS::Maths::Vector3::Length))
    ),
    StaticMethods(
        REFLECT_STATIC_METHOD(Dot, static_cast<float(*)(const NLS::Maths::Vector3&, const NLS::Maths::Vector3&)>(&NLS::Maths::Vector3::Dot))
    )
)
```

### 模式 4：私有类外绑定

只在确有必要时使用：

```cpp
REFLECT_EXTERNAL(
    NLS::meta::PrivateReflectionExternalSample,
    Fields(
        REFLECT_PRIVATE_FIELD(int, m_hiddenValue)
    ),
    Methods(
        REFLECT_PRIVATE_METHOD(GetHiddenValue)
    )
)
```

它依赖生成辅助访问器，应当是例外情况，而不是默认写法。

## 支持模式矩阵

| 模式 | 适用场景 | 典型宏 | 当前仓库示例 |
| --- | --- | --- | --- |
| 内联反射类型 | 引擎拥有该类型，且消费者需要稳定运行时元数据 | `CLASS` / `STRUCT` / `GENERATED_BODY` / `FUNCTION` | `TransformComponent`、`CameraComponent`、`GameObject`、`Scene` |
| 反射枚举 | 枚举会出现在反射字段或编辑器选项中 | `ENUM` | `EProjectionMode`、`ELightType`、`MeshRenderer::EFrustumBehaviour` |
| 自动属性 | getter / setter 命名能自然形成 `GetXxx` / `SetXxx` | 对两个访问器都加 `FUNCTION` | `fov`、`near`、`far`、`lightType`、`materialPaths` |
| 显式属性 | 暴露名和访问器命名不一致 | `PROPERTY(...)` + 对应访问器 | `GameObject.active`、`MeshRenderer.model` |
| 类外反射 | 类型本体不适合加内联反射宏 | `MetaExternal` + `REFLECT_EXTERNAL` | `NLS::Maths::Vector3`、`SerializedActorData` |
| 私有类外反射 | 私有成员确实需要被有意识地暴露 | `REFLECT_PRIVATE_*` | `PrivateReflectionExternalSample` |

## 当前仓库里的正确示例

当前内联反射覆盖比较正确的类型包括：

- `TransformComponent`
- `CameraComponent`
- `LightComponent`
- `MeshRenderer`
- `MaterialRenderer`
- `GameObject`
- `Scene`
- `BoundingSphere`
- `EProjectionMode`
- `ELightType`
- `MeshRenderer::EFrustumBehaviour`

当前类外反射用法比较正确的类型包括：

- `NLS::Maths::Vector3`
- `NLS::Maths::Quaternion`
- `SerializedComponentData`
- `SerializedActorData`
- `SerializedSceneData`
- `PrivateReflectionExternalSample`

## 验证步骤

必须走正常构建流程，让 MetaParser 以真实开发方式运行。

### Windows

```powershell
cmake -S . -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Debug --target NullusUnitTests ReflectionTest -- /m:1
ctest --test-dir build -C Debug --output-on-failure -R NullusUnitTests
.\build\bin\Debug\ReflectionTest.exe
```

现在 MetaParser 的构建日志会为每个反射头文件输出解析路线，例如：

```text
[MetaParser] Parsing ... [routes: text-type-body, external-declaration]
```

对于走 text parser 的反射类 / 结构体，构建日志现在还会继续输出一行 member discovery 摘要，例如：

```text
[MetaParser] Members NLS::Engine::Components::CameraComponent [fields: inline=0, explicit=0, auto=8, rejected=0, total=8] [methods: inline=17, explicit=0, rejected=0, overload-rejected=0, total=17]
```

可以这样理解这行摘要：

- `inline` 表示直接从类体里发现的已标记反射成员
- `explicit` 表示来自独立 `PROPERTY(...)` / `FUNCTION(...)` 指令的绑定
- `auto` 表示由 `Get*` / `Set*`、`Has*`、`Is*` 这类方法对自动推导出来的属性
- `rejected` / `overload-rejected` 表示解析器看到了候选成员，但按规则故意没有注册进反射

### Linux / macOS

```bash
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug
cmake --build build
ctest --test-dir build --output-on-failure
```

## 已知约束

- `Runtime/*/Gen/` 是生成输出，不能手改
- 结论只能覆盖真实跑过的验证证据
- Windows 上的反射验证不能自动推导 Linux / macOS 一样成立
- 如果某个属性类型不被支持，应该补这个类型的反射支持，而不是在生成代码里临时适配
- 如果声明里写成了未限定的 `Array<...>`，MetaParser 现在会直接提示把它改成 `NLS::Array<...>`

## 相关文档

- English guide: [ReflectionWorkflow.en.md](./ReflectionWorkflow.en.md)
- 测试说明: [../Testing.md](../Testing.md)
