#pragma once

#include "Object.h"
#include "Macros.h"
#include "Reflection/MetaParserFieldMethodSample.generated.h"

namespace NLS::meta
{
    CLASS() class MetaParserFieldMethodSample : public NLS::meta::Object
    {
    public:
        GENERATED_BODY()
        int Value = 7;

        Type GetType() const override
        {
            return typeof(MetaParserFieldMethodSample);
        }

        Object* Clone() const override
        {
            return new MetaParserFieldMethodSample(*this);
        }

        void OnSerialize(Json::object& output) const override
        {
            output["Value"] = Value;
        }

        int GetValue() const
        {
            return Value;
        }

        void SetValue(int v)
        {
            Value = v;
        }
    };
}
