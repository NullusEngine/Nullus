#pragma once

#include "Object.h"
#include "Macros.h"
#include "Reflection/ReflectionObjectSample.generated.h"

namespace NLS::meta
{
    // 新写法：CLASS + 继承 NLS::meta::Object（解析器侧优先走 annotate）
    CLASS(ReflectionObjectSample) : public NLS::meta::Object
    {
    public:
        GENERATED_BODY()
        ReflectionObjectSample() = default;
        ~ReflectionObjectSample() override = default;

        FUNCTION()
        void OnSerialize(Json::object& output) const override
        {
            output["type"] = "ReflectionObjectSample";
        }
    };
}
