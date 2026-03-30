# Nullus 反射系统审计

## 目标

本文档直接回答本次需求的前 3 个问题：

1. 当前反射系统和代码生成方案还有哪些优化空间
2. 当前项目里哪些地方已经正确使用，哪些地方还没真正用好
3. 还有哪些类、枚举、属性、函数应该注册到反射，以及“什么样的类型应该注册到反射”

最终验证证据会在实现和测试完成后补到本文档末尾。

## 当前方案的真实工作流

### 1. 声明输入层

当前仓库里实际存在 3 类受维护的反射输入：

- **内联反射类型**：`CLASS()` / `STRUCT()` / `ENUM()` + `GENERATED_BODY()`
- **自动属性推断**：同一个类型里成对出现的 `FUNCTION()` 标记 `GetXxx` / `SetXxx`
- **类外反射声明**：`MetaExternal(...)` + `REFLECT_EXTERNAL(...)`

### 2. 解析层

结合 `Tools/MetaParser/src/MetaParserTool.Core.cs`、`MetaParserTool.TextParser.cs`、`MetaParserTool.CppAstParser.cs` 当前代码，仓库里“实际生效”的解析策略是：

- 只要头文件中出现 `GENERATED_BODY()`，**当前主路径就是文本解析**，不是 `CppAst`
- 顶层 `ENUM()` 也走文本级提取
- `MetaExternal(...)` / `REFLECT_EXTERNAL(...)` 走类外声明解析
- 只有“没有 `GENERATED_BODY()`、也没有类外反射声明”的场景，才会尝试 `CppAst` 类/枚举解析

这意味着现有文档里“MetaParser 默认优先走 CppAst，文本路径只是回退”的说法已经不准确，至少对当前仓库里真正使用的反射类主体来说并非如此。

### 3. 生成层

MetaParser 会生成两类输出：

- 每个头对应的 `*.generated.h` / `*.generated.cpp`
- 每个模块对应的 `Gen/MetaGenerated.h` / `Gen/MetaGenerated.cpp`

模块级入口当前实际是：

- `LinkReflectionTypes_NLS_Base`
- `LinkReflectionTypes_NLS_Core`
- `LinkReflectionTypes_NLS_Engine`

并通过 `NLS_META_GENERATED_LINK_FUNCTION` 接到运行时数据库初始化流程里。

这也说明部分旧文档里写的 `RegisterReflectionTypes_NLS_*` 已经过时。

### 4. 运行时注册层

运行时注册流程由 `Runtime/Base/Reflection/ReflectionDatabase.cpp` 驱动：

- 先注册原生基础类型
- 再执行生成出来的模块 link 函数
- 再通过 `ReflectionModuleRegistry::RegisterAll(*this)` 执行静态注册器收集到的类型注册函数

类型注册分成两个阶段：

- `Declare`：分配类型 ID，建立基础 `TypeInfo`
- 非 `Declare`：补字段、方法、枚举值、基类关系等详细信息

## 当前方案还能优化的空间

### A. 文档和真实实现已经出现漂移

- 当前真实主路径是“`GENERATED_BODY()` 反射类走文本解析”，而不是“默认优先 `CppAst`”
- 当前模块入口名是 `LinkReflectionTypes_NLS_*`，不是旧文档中的 `RegisterReflectionTypes_NLS_*`
- 当前 README 里的反射相关内容存在编码问题，已经不适合作为入口文档

### B. 反射覆盖规则没有被整理成“消费者驱动”的准则

当前仓库里其实已经隐含出一条非常清晰的规则：

- 不是“继承了某个基类就应该全反射”
- 而是“只要编辑器、序列化、动态 `meta::Type` 创建/查询确实需要运行时元数据，这个类型就应该反射”

这条准则目前散落在代码和样例里，没有被明确写成团队规则。

### C. 项目里还存在“反射能力已经具备，但消费端没有真正用起来”的情况

最典型的是 `MaterialRenderer`：

- 类型已经是 `CLASS()` + `GENERATED_BODY()`
- 也已经有适合自动属性推断的 getter / setter 组合
- 但这些访问器没有完整接入当前反射属性规则
- `Inspector` 里还专门为它保留了手写 fallback，导致它没有像其它组件一样优先走反射路径

这说明当前问题不只是“有没有功能”，而是“有没有在项目里被正确用起来”。

### D. 测试覆盖面还不够均衡

当前测试已经有价值，但还存在几个空白区：

- 单元测试没有系统覆盖类外序列化结构体反射
- 私有类外绑定主要由 `Tools/ReflectionTest` 冒烟覆盖，单元测试层还不够强
- 自动属性推断、显式属性、类外反射、私有绑定这些“模式”没有被整理成统一测试骨架
- 生成测试较依赖字符串片段，缺少“为什么要测这个片段”的模式化表达

### E. 当前解析/生成路径对维护者不够直观

从代码上看，当前支持的模式其实已经不少：

- 内联类型
- 自动属性
- 显式属性
- 类外字段
- 类外方法
- 类外静态方法
- 私有类外字段和方法

但这些能力分散在不同文件里，新维护者很难快速看懂“什么时候该用哪一种”。

## 当前项目里已经正确使用这套方案的地方

### 1. 组件编辑属性这条链路整体方向是对的

以下类型已经在用“getter / setter -> 反射属性 -> Inspector 消费”的思路：

- `TransformComponent`
- `CameraComponent`
- `LightComponent`
- `MeshRenderer`
- `GameObject`
- `BoundingSphere`
- `EProjectionMode`
- `ELightType`
- `MeshRenderer::EFrustumBehaviour`

这说明“面向编辑器和序列化的稳定状态通过反射暴露”这条主线已经成立。

### 2. 类外反射声明的定位是正确的

以下类型采用 `MetaExternal` / `REFLECT_EXTERNAL` 是合理的：

- `NLS::Maths::Vector3`
- `NLS::Maths::Quaternion`
- `NLS::Engine::Serialize::SerializedComponentData`
- `NLS::Engine::Serialize::SerializedActorData`
- `NLS::Engine::Serialize::SerializedSceneData`
- `NLS::meta::PrivateReflectionExternalSample`

这些类型要么不适合直接改声明体，要么更适合作为“值类型 / 数据结构 / 外部类型”的类外绑定样例。

### 3. 显式属性机制本身用得是对的

当前仓库里已经有两个非常典型的正确范例：

- `GameObject.active`
  - getter 是 `IsSelfActive`
  - setter 是 `SetActive`
  - 不符合 `GetXxx` / `SetXxx` 的自动推断命名
  - 因此显式 `PROPERTY(...)` 是正确做法
- `MeshRenderer.model`
  - 暴露名想叫 `model`
  - 实际访问器是 `GetModelPath` / `SetModelPath`
  - 这时显式 `PROPERTY(name = model, ...)` 也是正确做法

## 当前项目里没有完全用好这套方案的地方

### 1. `MaterialRenderer` 是本轮最明确、也最值得先修的 gap

当前 `MaterialRenderer` 已经有这两组适合反射暴露的状态访问器：

- `GetMaterialPaths()` / `SetMaterialPaths(...)`
- `GetUserMatrixValues()` / `SetUserMatrixValues(...)`

这两组数据同时满足：

- 属于编辑器当前真实会展示和修改的状态
- 类型是当前 Inspector 已经支持的 `NLS::Array<std::string>` 和 `NLS::Array<float>`
- 不是纯内部缓存

但当前类型还没有把它们完整接入反射属性面，`Inspector` 也仍然强制走手写 `MaterialRenderer` fallback。

这是当前项目中“没有正确使用现有方案”的最典型例子。

**本轮落地结果**：

- 已给 `GetMaterialPaths()` / `SetMaterialPaths(...)` 增加 `FUNCTION()` 标记
- 已给 `GetUserMatrixValues()` / `SetUserMatrixValues(...)` 增加 `FUNCTION()` 标记
- 已把这些访问器的数组类型显式写成 `NLS::Array<...>`，让 MetaParser 校验器可以识别
- 已移除 `Inspector` 对 `MaterialRenderer` 的提前短路分支，让它和其它组件一样优先使用反射字段
- 生成结果现在已经补出 `materialPaths` 和 `userMatrixValues` 两个字段

### 2. 文档里对解析路径和入口名的描述已经落后于代码

这会直接误导后续维护者：

- 他们会以为大部分反射类型首先走 `CppAst`
- 他们会以为模块入口还是 `RegisterReflectionTypes_*`

这类认知偏差最终会让问题排查和新用法编写都变慢。

### 3. 测试责任分布还不够清晰

当前仓库同时保留：

- `NullusUnitTests`
- `Tools/ReflectionTest`

这是合理的，但边界还可以更清楚：

- 单元测试应该负责“模式化覆盖”和“精确回归”
- 冒烟工具应该负责“最低成本确认注册没整体断掉”

如果两边只是重复一套类型清单，维护成本会持续上升。

## 消费者矩阵与剩余缺口

### 消费者矩阵

| 消费者 | 主要文件 | 当前对反射的使用方式 | 当前剩余人工路径 | 当前判断 |
| --- | --- | --- | --- | --- |
| Inspector | `Project/Editor/Panels/Inspector.cpp` | 组件优先按 `GetFields()` 驱动 UI 渲染，枚举选项已走反射元数据 | 仍保留字段类型分支，以及“无反射字段”时的最小 generic fallback | 已明显收敛，剩余问题主要是类型覆盖面 |
| 序列化 | `Runtime/Engine/Serialize/GameobjectSerialize.cpp` | 大部分组件 payload 走 `SerializeJson` / `DeserializeJson` | 仍保留 legacy payload 兼容和 `MaterialRenderer` 特殊序列化分支 | 有效但兼容层较厚 |
| `meta::Type` 驱动组件创建 | `Runtime/Engine/GameObject.cpp` | `AddComponent(meta::Type)` 与 `GetComponent(meta::Type)` 都已走类型驱动路径 | 暂无高价值固定组件分支残留 | 当前链路已闭环 |

### 状态分类

| 类型 / 位置 | 消费者 | 事项描述 | 分类 | 原因 |
| --- | --- | --- | --- | --- |
| `Inspector` 通用枚举 UI | Inspector | 已从字段名手写 `projectionMode` / `lightType` / `frustumBehaviour` 选项切换为反射枚举元数据驱动 | `completed` | 已抽出 `InspectorReflectionUtils` 并接回 `DrawReflectedField` |
| `Inspector` 组件 fallback 函数族 | Inspector | 旧的 `DrawTransformFallback` / `DrawCameraFallback` / `DrawLightFallback` / `DrawMeshRendererFallback` / `DrawMaterialRendererFallback` 已移除，仅保留最薄的 generic “No reflected fields” 提示 | `completed` | fallback 范围已压缩到只负责提示“这个组件当前没有可消费的反射字段” |
| 资源指针字段的 Inspector 展示 | Inspector | 通用反射 UI 还不能安全处理资源指针和复杂句柄 | `blocked` | 当前字段绘制器没有统一的资源对象选择与序列化约束 |
| `MaterialRenderer` legacy payload 兼容 | 序列化 | 仍保留 `materials` / `userMatrix` 旧格式兼容和 inline material fallback | `intentional` | 当前代码显式兼容旧场景格式，不能直接移除 |
| 其他组件 legacy deserialize fallback | 序列化 | `ApplyLegacyComponentFallbacks` 仍保留旧键名/旧对象结构兼容 | `intentional` | 这是历史场景兼容层，不是“没接入反射”导致的缺口 |
| `GameObject::GetComponent(meta::Type)` | `meta::Type` 驱动组件查询 | 已从固定组件分支切换为真正的类型驱动查询 | `completed` | 现在按 `GetType()` / `DerivesFrom()` 遍历组件，并修正了模板版 `includeSubType` 语义 |
| `SkyBoxComponent` 资源相关状态 | Inspector / serialization | 类型本身可反射，但资源指针表面还不适合直接泛化到当前通用消费者 | `blocked` | 消费者能力不足，不是声明层能力不足 |
| `Component` 生命周期与内部状态 | 多消费者 | `m_owner`、`m_enabled`、`m_destroyedFromOwner` 等内部状态不该暴露给现有消费者 | `intentional` | 这些成员不属于稳定、面向消费者的元数据面 |

### 本轮后续优化结果

- `Inspector` 的枚举字段绘制已经不再依赖字段名特判，而是通过反射枚举元数据生成下拉选项
- `Inspector` 的 legacy 组件 fallback 函数族已经移除，只保留了最小 generic fallback
- `GameObject::GetComponent(meta::Type)` 已经改为真正的类型驱动查询
- `GameObject::GetComponent<T>(false)` 也已修正为“精确匹配”语义，不再把派生类型误当作 exact match

### 下一批消费者修复 shortlist

1. **重新界定 `ReflectionTest` 与 `NullusUnitTests` 的职责边界**
   - 当前重复度还偏高
   - 但它更适合在下一轮测试重组中做，而不是作为单点代码修复立即做

2. **继续推进 Inspector 的通用字段能力，减少 fallback 函数族**
   - 当前主路径已经走反射
   - 下一步可以继续压缩手写字段 UI 的范围

3. **评估资源指针字段的统一编辑策略**
   - 这是 Inspector 继续泛化的主要阻塞项
   - 不解决它，就很难让 `SkyBoxComponent` 这类对象完全走通用反射 UI

## 什么样的类应该注册到反射

### 应该注册

以下类型应该优先考虑注册到反射：

- **编辑器要读写其状态的类型**
  - 例如 Inspector 需要展示和修改的组件状态
- **序列化要遍历其字段的类型**
  - 例如场景、对象、组件保存与恢复中真实依赖字段遍历的类型
- **运行时要通过 `meta::Type` 动态创建、查找、比较的类型**
  - 例如组件动态创建、基类/派生类查询
- **会作为已反射属性类型出现的值类型或枚举**
  - 例如 `BoundingSphere`、`EProjectionMode`
- **不适合改原始声明体、但又确实需要反射的类型**
  - 例如数学类型、纯数据结构、外部风格封装类型

### 不应该为了“齐全”而注册

以下类型不应该仅仅因为“公开”就注册：

- 纯内部缓存、临时状态、生命周期中间量
- 当前没有任何编辑器/序列化/动态元数据消费者的实现细节类型
- 暴露后消费者也无法安全处理的资源指针和重对象句柄
- 仅用于运行时内部流程、不会以元数据驱动方式访问的辅助方法

## 还有哪些类、枚举、属性、函数应该注册到反射

### 本轮应该补上的高优先级项

#### `NLS::Engine::Components::MaterialRenderer`

应该补反射属性：

- `materialPaths`
  - 来源：`GetMaterialPaths()` / `SetMaterialPaths(...)`
  - 原因：当前编辑器已经在手动编辑这份状态
- `userMatrixValues`
  - 来源：`GetUserMatrixValues()` / `SetUserMatrixValues(...)`
  - 原因：当前编辑器已经在手动编辑这份状态

### 当前应该继续保持为已反射状态的类型

- `TransformComponent`
- `CameraComponent`
- `LightComponent`
- `MeshRenderer`
- `GameObject`
- `Scene`
- `BoundingSphere`
- `EProjectionMode`
- `ELightType`
- `MeshRenderer::EFrustumBehaviour`
- `Vector3` / `Quaternion`
- `SerializedComponentData`
- `SerializedActorData`
- `SerializedSceneData`

### 当前先不建议在本轮扩大到的对象

这些类型不是“永远不该反射”，而是**在当前消费者能力下不宜直接扩大范围**：

- `SkyBoxComponent` 里偏资源句柄/指针的状态
- `Component` 的 `m_owner`、`m_enabled` 这类内部生命周期状态
- `Scene` 的 `FastAccessComponents` 缓存结构
- `GameObject` 生命周期回调和碰撞/触发回调

原因是当前编辑器通用反射 UI 和序列化路径并没有明确消费它们，贸然暴露只会增加噪音。

## 什么样的枚举、属性、函数应该注册

### 枚举

应该注册：

- 会出现在已反射字段里的枚举
- 需要在编辑器里显示离散选项的枚举
- 需要稳定序列化为键名/值的枚举

当前典型正确例子：

- `EProjectionMode`
- `ELightType`
- `MeshRenderer::EFrustumBehaviour`

### 属性

应该注册：

- 稳定、可编辑、可序列化、可通过元数据访问的“状态”
- getter/setter 语义清晰、不会把复杂副作用藏进普通字段读写的状态

优先模式：

1. `GetXxx` / `SetXxx` 且命名对齐时，优先自动属性推断
2. 暴露名与访问器名不一致时，用显式 `PROPERTY(...)`
3. 原始类型不适合改声明体时，用 `REFLECT_EXTERNAL` / `REFLECT_PROPERTY`

不应该注册：

- 纯缓存
- 内部临时值
- 当前消费者处理不了的复杂资源句柄
- 读写具有强副作用但又会被误当成普通字段的接口

### 函数

应该注册：

- 需要被运行时元数据显式查找或调用的函数
- 属性 getter / setter
- 少量对外暴露的稳定运行时操作
- 作为样例覆盖当前生成器能力的关键函数

不应该注册：

- 生命周期回调（除非有明确元数据调用者）
- 纯内部辅助函数
- 过度暴露实现细节的方法

## 结论

当前这套方案**不是不能用，而是已经能用并且大体方向是对的**。真正的问题主要集中在：

1. 文档和真实实现出现漂移
2. 覆盖规则没有被整理成消费者驱动准则
3. 某些已经具备反射条件的类型还没有被项目真正按反射方式消费
4. 测试还没把“仓库里支持的模式”整理成稳定回归面

本轮实现已经解决了其中最关键的一批问题：

- `MaterialRenderer` 的反射覆盖 gap
- 反射测试写法和覆盖面的补全
- 中英文维护文档和 README 入口

剩余更大的后续优化方向仍然包括：

- 是否进一步缩减 `NullusUnitTests` 与 `ReflectionTest` 的职责重叠
- 是否把更多“消费者驱动”的反射准则自动化进 MetaParser 校验
- 是否单独清理 README 更大范围的历史编码问题

这些后续工作已经整理为专门路线图：

- `specs/001-reflection-audit/optimization-roadmap.md`

并且其中一部分已经开始落地：

- `ReflectionTest` 已收敛为模式级哨兵 + 少量消费者冒烟验证
- `Docs/Testing.md` 已明确区分 `NullusUnitTests`、生成测试、`ReflectionTest` 的职责

### 当前测试职责收敛结果

- `NullusUnitTests`
  - 继续承载细粒度运行时断言和生成片段断言
  - 更适合做模式化精确回归
  - 当前已按桶拆为：
    - `ReflectionRuntimeCoreTests.cpp`
    - `ReflectionRuntimeEngineTests.cpp`
    - `MetaParserGenerationModuleTests.cpp`
    - `MetaParserGenerationEngineTests.cpp`
    - `MetaParserGenerationDataTests.cpp`
- `ReflectionTest`
  - 已缩减为“每类模式至少一个哨兵类型”加少量消费者冒烟
  - 当前额外覆盖：
    - `meta::Type` 驱动组件查询
    - Inspector 枚举选项的反射枚举元数据生成

本轮重新验证过：

- `cmake --build build --config Debug --target ReflectionTest -- /m:1`
- `.\Build\bin\Debug\ReflectionTest.exe`

结果：

- `ReflectionTest` 构建通过
- `ReflectionTest` 运行通过
- 冒烟层已经不再承担整份详细反射清单的职责，而是保留模式级和消费者级哨兵覆盖

### 当前工具侧诊断增强结果

MetaParser 已经补上两类更直接的维护者反馈：

- **解析路径诊断**
  - 构建日志现在会为每个反射头文件打印实际解析路线
  - 例如：
    - `text-type-body`
    - `text-top-level-enum`
    - `external-declaration`
    - `cppast`
- **member discovery 摘要**
  - 对走 `text-type-body` 的反射类 / 结构体，构建日志现在会继续打印每个类型的成员发现摘要
  - 摘要会显示：
    - inline field / method
    - explicit property / method
    - auto property
    - rejected / overload-rejected 候选数
- **字段类型错误提示增强**
  - 如果声明里把 `NLS::Array<...>` 写成了未限定的 `Array<...>`，MetaParser 现在会直接提示应改成 `NLS::Array<...>`

本轮真实构建已经看到新的日志输出，例如：

- `CameraComponent.h [routes: text-type-body]`
- `ELightType.h [routes: text-top-level-enum, cppast]`
- `SceneSerializationData.h [routes: external-declaration]`
- `Members NLS::Engine::Components::CameraComponent [fields: inline=0, explicit=0, auto=8, rejected=0, total=8] ...`
- `Members NLS::Engine::SceneSystem::Scene [fields: inline=0, explicit=0, auto=0, rejected=0, total=0] [methods: inline=1, explicit=1, rejected=0, overload-rejected=0, total=2]`

## 最终验证证据

- 已通过正常构建流重新生成 `NLS_Engine` 反射输出：
  - `cmake --build build --config Debug --target ReflectionTest -- /m:1`
- 已确认生成文件 `Runtime/Engine/Gen/Components/MaterialRenderer.generated.cpp` 补出了：
  - `materialPaths`
  - `userMatrixValues`
- 已顺序执行并通过：
  - `.\Build\bin\Debug\ReflectionTest.exe`
  - 结果：`=== All reflection tests passed ===`
- 已手动执行 MetaParser 真实入口并看到新的类型级诊断摘要：
  - `Build/Runtime/Engine/NLS_Engine_run_metaparser_Debug.cmd Build/Runtime/Engine/NLS_Engine.precompile.json`
- 已重新构建并通过：
  - `cmake --build build --config Debug --target NullusUnitTests -- /m:1`
- 已重新执行并通过：
  - `ctest --test-dir build -C Debug --output-on-failure -R NullusUnitTests`
  - 结果：`100% tests passed, 0 tests failed out of 1`

## 未验证范围

- Linux / macOS 反射生成行为没有在这轮实际验证
- Editor 运行时界面没有在这轮启动做交互式手工验证；当前仅完成了代码路径修正和反射生成/冒烟验证
