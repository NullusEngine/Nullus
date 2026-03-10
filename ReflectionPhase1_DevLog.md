# Reflection Phase 1 开发记录（2026-03-10）

## 做了什么
1. 修复并收敛 `Tools/ReflectionTest`，改为验证真实链路：
   - `ReflectionDatabase` 初始化触发自动注册；
   - `Type::GetFromName("NLS::meta::TestObject")` 可取到类型；
   - `Object` 克隆与序列化可用。
2. 增强 `Tools/MetaParser`：
   - 支持在单文件中提取多个满足约束的 `Meta()` 类；
   - 识别规则统一到 `NLS::meta::Object` 开发约束；
   - 生成结果排序+去重，保证输出稳定（避免目录遍历顺序导致抖动）。
3. 增加一期约束文档与样例：
   - `Runtime/Base/Reflection/ReflectionPhase1.md`
   - `Runtime/Base/Reflection/ReflectionObjectSample.h`

## 怎么验证
```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build --target NLS_MetaParser_Generate
cmake --build build -j4
./build/bin/ReflectionTest
```

## 结果
- MetaParser 生成成功，输出 `build/Runtime/Base/Generated/MetaGenerated.cpp`。
- 全量构建成功。
- ReflectionTest 运行通过，输出 `=== All tests passed! ===`。
- 过程中发现并修复一个阻塞问题：注册函数在 `ReflectionDatabase` 构造期递归调用 `Instance()` 导致 `recursive_init_error`，已改为构造期传入 `db` 引用注册。
