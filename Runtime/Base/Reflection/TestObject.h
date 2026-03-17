#pragma once

#include "Object.h"
#include "Macros.h"
#include "Reflection/TestObject.generated.h"

namespace NLS
{
    namespace meta
    {
        // 测试反射对象
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

            void OnSerialize(Json::object& output) const override
            {
                output["type"] = "TestObject";
            }

            void OnDeserialize(const Json& input) override
            {
                // 测试反序列化
            }
        };

    }
}
