#pragma once

#include "Object.h"
#include "Macros.h"
#include "Reflection/MetaParserFieldMethodSample.generated.h"

namespace NLS::meta
{
    CLASS(MetaParserFieldMethodSample) : public NLS::Object
    {
    public:
        GENERATED_BODY()
        int Value = 7;

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
