#pragma once

#include "Object.h"
#include "Macros.h"

namespace NLS::meta
{
    // 新写法：CLASS + 继承 NLS::meta::Object（解析器侧优先走 annotate）
    CLASS(ReflectionObjectSample, Reflection) : public NLS::meta::Object
    {
    public:
        ReflectionObjectSample() = default;
        ~ReflectionObjectSample() override = default;

        Type GetType() const override
        {
            return typeof(ReflectionObjectSample);
        }

        Object* Clone() const override
        {
            return new ReflectionObjectSample(*this);
        }

        void OnSerialize(Json::object& output) const override
        {
            output["type"] = "ReflectionObjectSample";
        }
    };
}
