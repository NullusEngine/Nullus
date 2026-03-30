# Reflection Phase 1（可用版）开发约束

> Current verified workflow: `Docs/Reflection/ReflectionWorkflow.zh-CN.md` and `Docs/Reflection/ReflectionWorkflow.en.md`

## 目标
- 以当前维护中的 MetaParser 工作流为基础记录一期约束
- 通过 `Tools/MetaParser` 自动生成注册代码（`MetaGenerated.cpp`）
- 构建时自动触发生成并编译进对应模块

## 开发约束（必须满足）
1. 引擎拥有的运行时类型优先使用当前维护中的内联反射宏：
   - `CLASS` / `STRUCT` / `ENUM`
   - `PROPERTY`
   - `FUNCTION`
   - `GENERATED_BODY`
2. 不适合改原始声明体的类型，使用类外反射声明：
   - `MetaExternal`
   - `REFLECT_EXTERNAL`
   - `REFLECT_PRIVATE_*`
3. 反射头文件需位于 `Runtime/` 下（MetaParser 当前扫描范围）。
4. 是否纳入反射，按“消费者驱动”原则判断，而不是按“是否继承某个基类”一刀切。

> 当前维护中的完整规则，请优先以 `Docs/Reflection/` 下的双语文档为准。

## 推荐样例
见：`Runtime/Base/Reflection/ReflectionObjectSample.h`

## 生成链路
- 生成入口：`Runtime/CMakeLists.txt` 中的 `nls_add_meta_generation(...)`
- 当前正式输出：`Runtime/Base/Gen/MetaGenerated.cpp`
- 其他模块同理：`Runtime/<Module>/Gen/MetaGenerated.cpp`
- 编译接入：各模块通过 `target_sources(<module> PRIVATE "${_gen_cpp}" "${_gen_h}")` 自动接入
- 模块级入口当前使用 `LinkReflectionTypes_NLS_*`

## 快速验证
```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build --target NLS_Base -j4
./build/bin/ReflectionTest
```

若输出包含：
- `Found reflection class: ...`
- `=== All tests passed! ===`

即说明“生成 -> 编译 -> 运行”闭环可用。
