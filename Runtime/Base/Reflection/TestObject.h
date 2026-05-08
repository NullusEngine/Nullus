#pragma once

#include "Object.h"
#include "Macros.h"
#include "Reflection/TestObject.generated.h"

namespace NLS::meta
{
    // 娴嬭瘯鍙嶅皠瀵硅薄
    CLASS(TestObject) : public Object
    {
    public:
        GENERATED_BODY()
        TestObject() = default;
        virtual ~TestObject() = default;

        FUNCTION()
        void OnSerialize(Json::object& output) const override
        {
            output["type"] = "TestObject";
        }

        FUNCTION()
        void OnDeserialize(const Json& input) override
        {
            // 娴嬭瘯鍙嶅簭鍒楀寲
        }
    };
}
