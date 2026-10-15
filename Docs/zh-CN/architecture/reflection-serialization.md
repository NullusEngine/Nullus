# 反射与序列化

反射同时驱动三件事：编辑器界面、场景与 Prefab 序列化、脚本 API 清单。三者共用一份元数据，因此“能序列化的字段”和“能编辑的字段”默认一致。

## 反射链路

```text
C++ 声明（CLASS / STRUCT / ENUM / PROPERTY / FUNCTION）
    |
    v
Tools/MetaParser 解析
    |
    v
Runtime/<Module>/Gen/ 与 Project/<Target>/Gen/
    |
    v
运行时注册到反射数据库
    |
    +--> Inspector 控件
    +--> 对象图序列化
    +--> 脚本 API 清单
```

不要手工编辑 `Gen/` 目录下的任何文件。

## 写入反射的写法

### 内联类型

```cpp
CLASS(MyType) : public NLS::meta::Object
{
public:
    GENERATED_BODY()

    PROPERTY()
    float speed = 1.0f;
};
```

### 枚举

```cpp
ENUM(MyMode) : uint8_t
{
    A,
    B
};
```

### 访问器属性

标了 `FUNCTION()` 的 `GetXxx` / `SetXxx` 会自动组成一个属性。只有 getter 时属性只读，只有 setter 时属性只写。

### 类外声明

不便改动原始声明体时使用：

```cpp
MetaExternal(MyType)
REFLECT_EXTERNAL(MyType, "MyType")
```

私有成员的类外绑定使用 `REFLECT_PRIVATE_*` 系列。

## 解析路径

- 类型主体中出现 `GENERATED_BODY()` 时走文本解析；
- 顶层 `ENUM()` 同样走文本提取；
- `MetaExternal` / `REFLECT_EXTERNAL` 走类外声明解析；
- `CppAst` 仍然存在于工具链中，但不是当前大多数反射类型主体的解析路径。

## 准入规则

坚持消费者驱动：只有存在真实消费者时才反射。

| 问题 | 结论 |
| --- | --- |
| 编辑器需要查看或编辑它？ | 反射 |
| 序列化需要遍历它的字段？ | 反射 |
| 运行时要通过 `meta::Type` 创建、查询或比较它？ | 反射 |
| 它是别的已反射属性依赖的值类型或枚举？ | 反射 |
| 它只是缓存、辅助实现、临时状态或消费者无法处理的句柄？ | 不反射 |

不要因为类型是 `public` 就默认反射它。

## 运行时注册

每个运行时模块生成自己的 link 函数，例如 `LinkReflectionTypes_NLS_Base`、`LinkReflectionTypes_NLS_Core`、`LinkReflectionTypes_NLS_Engine`。数据库初始化时调用这些函数完成注册。

## 对象图序列化

场景与 Prefab 使用同一套对象图格式：

| 元素 | 表达方式 |
| --- | --- |
| 对象 | GUID 支撑的 `ObjectRecord` |
| 普通状态 | 反射字段 |
| 生命周期归属 | `$owned` 关系 |
| 场景内引用 | 非零 `fileID`、空 `guid`、`type = NonAssetType` |
| 资产引用 | 非零 `fileID`、有效资产 `guid`、资产类型；`filePath` 只是提示 |
| Prefab 差异 | 补丁操作，而非按对象类型的专用载荷 |

旧的 `$ref` 与 `$asset` 形式不再被读取器接受。使用 `$owned` 表达归属，使用 `fileID`/`guid`/`type` 表达引用。

## 读写流程

`ObjectGraphSerializer` 写出反射字段，并在字段之外单独记录图关系。`ObjectGraphInstantiator` 分阶段实例化：

1. 创建已知对象；
2. 应用反射字段；
3. 解析关系；
4. 重建场景缓存。

## 加载策略

- **编辑器策略**：保留未知记录与未解析的资产引用，并记录诊断，让文档可以在不丢失可恢复数据的前提下往返；
- **运行时策略**：遇到未知或非法数据直接失败，不静默丢弃。

这条差异是刻意的：编辑器需要容错，运行时需要确定性。

## 已废弃的旧模型

`GameobjectSerialize` 适配器与 `SerializedSceneData` / `SerializedActorData` / `SerializedComponentData` 载荷模型不属于维护中的运行时场景加载路径。

## 序列化扩展点

状态无法用反射字段表达的组件可以重写：

```cpp
virtual bool SerializeObjectGraphProperties(
    std::vector<NLS::Engine::Serialize::PropertyRecord>& properties) const;

virtual bool DeserializeObjectGraphProperties(
    const std::vector<NLS::Engine::Serialize::PropertyRecord>& properties);
```

引擎拥有图格式，组件拥有自己的载荷，依赖保持单向。脚本组件正是通过这条路径保存脚本资产元数据与字段覆盖。

## 下一步

- [Prefab 工作流](../prefab/workflow.md)：补丁操作如何表达 Override。
- [脚本 API 参考](../scripting/api.md)：清单如何变成两种语言的绑定。
