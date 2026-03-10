#pragma once

#include "Object.h"
#include "Macros.h"

namespace NLS
{
    namespace meta
    {
        // 测试反射对象
        Meta() class TestObject : public Object
        {
        public:
            TestObject() = default;
            virtual ~TestObject() = default;

            Type GetType() const override
            {
                return typeof(TestObject);
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
