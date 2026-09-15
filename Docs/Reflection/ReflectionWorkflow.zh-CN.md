# Nullus 反射工作流

这是 Nullus 当前维护中的中文反射使用说明。

English version: [ReflectionWorkflow.en.md](./ReflectionWorkflow.en.md)

## 总览

Nullus 当前维护中的反射路径只有一条：

- 在运行时和项目头文件里声明反射输入
- 正常走 CMake 构建，让 `Tools/MetaParser` 在构建期解析这些声明
- 生成胶水代码到 `Runtime/<Module>/Gen/` 或 `Project/<Target>/Gen/`
- 运行时通过生成出来的模块 link 函数把类型和成员注册进反射数据库

不要手改 `Runtime/*/Gen/` 或 `Project/*/Gen/` 下的任何文件。

## 当前系统的真实行为

### 支持的输入模式

当前仓库真正支持并维护的是这几种写法：

1. 内联反射类型：

```cpp
CLASS(MyType) : public NLS::meta::Object
{
public:
    GENERATED_BODY()
};
```

2. 反射枚举：

```cpp
ENUM(MyMode) : uint8_t
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

1. 字段直接加 `PROPERTY(...)`，属性名就是字段真名
2. 需要封装时，在同一个 `PROPERTY(...)` 里写 `Getter(Name)` / `Setter(Name)`
3. `DisplayName(...)` 只控制编辑器显示名，不改变序列化名
4. 字段改名又要保留旧数据时，写 `FormerlySerializedAs("旧名")`
5. 类型本身不适合改声明时，用类外声明

命名约定：

- 被 `PROPERTY` 标注的字段就是序列化字段，字段名即序列化键；这类反射数据成员不使用 `m_` 前缀（`localPosition`、`skyMode`、`document`），与 `EditorSettings` 里既有的反射结构保持一致。
- 需要封装时，访问器用 `FUNCTION(...)` 单独暴露，字段仍保留原名。
- 字段改名后要保证旧数据仍可读，就写 `FormerlySerializedAs("旧名")`；不要为了迁就旧键而发明一个不存在的字段名。
- 平台宏等无法作为成员名的键（例如 Windows 上的 `near` / `far`）同样用改名加 `FormerlySerializedAs` 处理，`CameraComponent.nearClip` / `farClip` 是先例。

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

### 模式 1：直接字段属性

```cpp
PROPERTY(DisplayName("Local Position"))
Maths::Vector3 localPosition;
```

属性名就是字段真名 `localPosition`。`DisplayName` 只影响编辑器标签，不参与
序列化路径、Prefab override 或 Undo 记录。

### 模式 2：显式访问器

字段需要封装时，把访问器写在同一个 `PROPERTY(...)` 里：

```cpp
PROPERTY(DisplayName("Active"), Getter(GetActive), Setter(SetActive))
bool active;

FUNCTION(Scriptable)
bool GetActive() const;

FUNCTION(Scriptable)
void SetActive(bool value);
```

规则：

- `Getter` / `Setter` 只接受成员函数名，不做 `GetXxx` / `SetXxx` 命名推导。
- 只写 `Getter(...)` 得到只读属性；只写 `Setter(...)` 直接报错。
- 访问器必须 public、非 static，签名匹配且没有重载歧义。
- 访问器自己的 `FUNCTION(...)` 决定它是否同时作为脚本方法暴露。

### 模式 3：字段改名后保留旧数据

```cpp
PROPERTY(DisplayName("Near Clip"), FormerlySerializedAs("near"), Getter(GetNear), Setter(SetNear))
float nearClip = 0.1f;
```

`FormerlySerializedAs` 让旧数据仍然能用旧名解析到该字段；新写入的数据使用
字段真名。

### 模式 4：类外反射声明

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

### 模式 5：私有类外绑定

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

## 编辑器脚本如何使用 SerializedObject

`SerializedObject` / `SerializedProperty` 是编辑器专属能力，玩家脚本程序集里没有它们：

- 唯一规范实现是 native 的 `NLS::Editor::Serialization::SerializedObject`（TypeTree + byte stream + staged snapshot）。
- 编辑器程序集 `Nullus.Editor.dll` 只做句柄包装（源文件 `Managed/Nullus.Managed/EditorSerializedObject.cs`），`Nullus.Managed.csproj` 用 `<Compile Remove>` 把它排除在玩家程序集之外。
- 编辑器启动时 `Context` 在 CoreCLR 就绪后调用 `InstallManagedSerializationBridge`，把 `GetEditorSerializationApiTable()` 交给托管 `Nullus.Editor.EditorSerializationHost.Install`。装好之后 `new SerializedObject(target)` / `FindProperty(...)` / `ApplyModifiedProperties()` 直接可用；没有安装时对象 `IsValid` 为 false，访问会抛 `InvalidOperationException`。
- `Nullus.Editor.dll` 由 CMake 托管构建规则产出到 `build/Managed/<config>/`，与项目脚本程序集同目录，宿主按同目录查找。

| 模式 | 适用场景 | 典型宏 | 当前仓库示例 |
| --- | --- | --- | --- |
| 内联反射类型 | 引擎拥有该类型，且消费者需要稳定运行时元数据 | `CLASS` / `STRUCT` / `GENERATED_BODY` / `FUNCTION` | `TransformComponent`、`CameraComponent`、`GameObject`、`Scene` |
| 反射枚举 | 枚举会出现在反射字段或编辑器选项中 | `ENUM` | `EProjectionMode`、`ELightType`、`MeshRenderer::EFrustumBehaviour` |
| 直接字段属性 | 字段本身就是要暴露的状态 | 字段上写 `PROPERTY(...)` | `TransformComponent.localPosition`、`SkyBoxComponent.skyMode` |
| 显式访问器属性 | 需要封装、转换或副作用 | `PROPERTY(Getter(...), Setter(...))` | `GameObject.active`、`MeshFilter.mesh`、`UICanvasComponent.document` |
| 字段改名迁移 | 字段改名但要读回旧数据 | `PROPERTY(FormerlySerializedAs(\"旧名\"), ...)` | `CameraComponent.nearClip`（旧名 `near`） |
| 类外反射 | 类型本体不适合加内联反射宏 | `MetaExternal` + `REFLECT_EXTERNAL` | `NLS::Maths::Vector3`、`NLS::Maths::Quaternion` |
| 私有类外反射 | 私有成员确实需要被有意识地暴露 | `REFLECT_PRIVATE_*` | `PrivateReflectionExternalSample` |

## 当前仓库里的正确示例

当前内联反射覆盖比较正确的类型包括：

- `TransformComponent`
- `CameraComponent`
- `LightComponent`
- `MeshFilter`
- `MeshRenderer`
- `GameObject`
- `Scene`
- `BoundingSphere`
- `EProjectionMode`
- `ELightType`
- `MeshRenderer::EFrustumBehaviour`

当前类外反射用法比较正确的类型包括：

- `NLS::Maths::Vector3`
- `NLS::Maths::Quaternion`
- `PrivateReflectionExternalSample`

## 反射序列化约定

Object Graph 序列化把反射字段作为维护中的普通属性面。`ObjectGraphSerializer` 会遍历 `object.GetType().GetFields()`，并通过反射 JSON 路径把字段值转换成 Object Graph property。

定义图结构的关系不属于普通反射字段。Scene 对 GameObject 的拥有关系、GameObject 对组件或子物体的拥有关系用 `$owned` 表达。父级对象引用和资产引用使用当前 Object Graph 的对象引用形状：`fileID`、`guid`、`type`，以及可选的 `filePath`。Prefab override 使用 `Runtime/Engine/Serialize` 里的 patch operation 表达。

不要再写旧的 `$ref` 或 `$asset` 对象。`ObjectGraphReader` 会拒绝这些形状；本地对象引用使用空 `guid` 加非零 `fileID`，资产引用必须有有效 `guid`、非零 `fileID` 和资产 `type`。

因此，准备进入场景或预制体持久化的类型，应通过反射字段暴露稳定状态；需要跳过的字段应通过 transient / editor-only / runtime-only 等运行时元数据表达；原始指针默认不应持久化，除非明确建模为 owned、object reference、asset reference 或 transient。旧的 `SerializedSceneData`、`SerializedActorData`、`SerializedComponentData` 和 `GameobjectSerialize` adapter 路径不再是维护中的场景持久化面。

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

- `Runtime/*/Gen/` 和 `Project/*/Gen/` 是生成输出，不能手改
- 结论只能覆盖真实跑过的验证证据
- Windows 上的反射验证不能自动推导 Linux / macOS 一样成立
- 如果某个属性类型不被支持，应该补这个类型的反射支持，而不是在生成代码里临时适配
- 反射值字段支持 `std::vector<T>` 和 `NLS::Array<T>`；MetaParser 也会在 CppAst 解析后规范化本地简写（如 `vector<T>`、`Array<T>`），但源码风格仍优先使用完整限定写法。

## 相关文档

- English guide: [ReflectionWorkflow.en.md](./ReflectionWorkflow.en.md)
- 测试说明: [../Testing.md](../Testing.md)
