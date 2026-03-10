#include "Reflection/Type.h"
#include "Reflection/ReflectionDatabase.h"

#include <iostream>

int main()
{
    std::cout << "=== Reflection System Test ===" << std::endl;

    // 触发数据库单例构造（构造中会调用 MetaParser 生成代码的注册函数）
    auto& db = NLS::meta::ReflectionDatabase::Instance();
    (void)db;

    const auto testObjectType = NLS::meta::Type::GetFromName("NLS::meta::TestObject");
    if (!testObjectType.IsValid())
    {
        std::cerr << "[FAIL] Type NLS::meta::TestObject was not registered." << std::endl;
        return 1;
    }

    const auto sampleType = NLS::meta::Type::GetFromName("NLS::meta::ReflectionObjectSample");
    if (!sampleType.IsValid())
    {
        std::cerr << "[FAIL] Type NLS::meta::ReflectionObjectSample was not registered." << std::endl;
        return 1;
    }

    std::cout << "✓ Reflected type found: " << testObjectType.GetName() << std::endl;
    std::cout << "✓ Reflected type found: " << sampleType.GetName() << std::endl;
    std::cout << "=== All tests passed! ===" << std::endl;

    return 0;
}
