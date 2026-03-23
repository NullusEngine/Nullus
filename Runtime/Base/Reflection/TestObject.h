#pragma once

#include "Object.h"
#include "Macros.h"
#include "Reflection/TestObject.generated.h"

namespace NLS::meta
{
    // 娴嬭瘯鍙嶅皠瀵硅薄
    CLASS() class TestObject : public Object
    {
    public:
        GENERATED_BODY()
        TestObject() = default;
        virtual ~TestObject() = default;

        Type GetType() const override
        {
            return NLS_TYPEOF(TestObject);
        }

        Object* Clone() const override
        {
            return new TestObject(*this);
        }

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
