#pragma once

#include "Object.h"
#include "Macros.h"
#include "Reflection/TestObject.generated.h"

namespace NLS::meta
{
    // 娴嬭瘯鍙嶅皠瀵硅薄
    CLASS(TestObject) : public NLS::Object
    {
    public:
        GENERATED_BODY()
        TestObject() = default;
        virtual ~TestObject() = default;

    };
}
