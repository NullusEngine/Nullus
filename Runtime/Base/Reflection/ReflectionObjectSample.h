#pragma once

#include "Object.h"
#include "Macros.h"
#include "Reflection/ReflectionObjectSample.generated.h"

namespace NLS::meta
{
    // 新写法：CLASS + 继承 Unity-style NLS::Object（解析器侧优先走 annotate）
    CLASS(ReflectionObjectSample) : public NLS::Object
    {
    public:
        GENERATED_BODY()
        ReflectionObjectSample() = default;
        ~ReflectionObjectSample() override = default;

    };
}
