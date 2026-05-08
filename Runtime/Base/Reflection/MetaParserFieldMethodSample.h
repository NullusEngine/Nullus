#pragma once

#include "Object.h"
#include "Macros.h"
#include "Reflection/MetaParserFieldMethodSample.generated.h"

namespace NLS::meta
{
    CLASS(MetaParserFieldMethodSample) : public NLS::meta::Object
    {
    public:
        GENERATED_BODY()
        int Value = 7;

        void OnSerialize(Json::object& output) const override
        {
            output["Value"] = Value;
        }

        FUNCTION()
        int GetValue() const
        {
            return Value;
        }

        FUNCTION()
        void SetValue(int v)
        {
            Value = v;
        }
    };
}
