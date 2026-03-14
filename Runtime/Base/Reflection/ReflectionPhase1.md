# Reflection Phase 1（可用版）开发约束

> Current verified workflow: `ReflectionBindingWorkflow.md` and `Runtime/Base/Reflection/ReflectionBindingWorkflow.md`

## 目标
- 反射对象统一以 `NLS::meta::Object` 为基类。
- 通过 `Tools/MetaParser` 自动生成注册代码（`MetaGenerated.cpp`）。
- 构建时自动触发生成并编译进 `NLS_Base`。

## 开发约束（必须满足）
1. 反射类必须标注 `Meta()`。
2. 反射类必须满足以下其一：
   - 位于 `namespace NLS::meta` 中，继承 `Object`；
   - 任意命名空间中，显式继承 `NLS::meta::Object`。
3. 反射类头文件需位于 `Runtime/` 下（MetaParser 当前扫描范围）。

> 建议：统一写法为“显式继承 `NLS::meta::Object`”，可读性更强、解析更稳定。

## 推荐样例
见：`Runtime/Base/Reflection/ReflectionObjectSample.h`

## 生成链路
- 生成入口：`Runtime/CMakeLists.txt` 中的 `nls_add_meta_generation(...)`
- 当前正式输出：`Runtime/Base/Gen/MetaGenerated.cpp`
- 其他模块同理：`Runtime/<Module>/Gen/MetaGenerated.cpp`
- 编译接入：各模块通过 `target_sources(<module> PRIVATE "${_gen_cpp}" "${_gen_h}")` 自动接入

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
