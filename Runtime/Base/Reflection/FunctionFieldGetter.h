/* ----------------------------------------------------------------------------
** Copyright (c) 2016 Austin Brunkhorst, All Rights Reserved.
**
** FunctionFieldGetter.h
** --------------------------------------------------------------------------*/

#pragma once

#include "FieldGetterBase.h"

namespace NLS
{
    namespace meta
    {
        template<typename ClassType, typename ReturnType>
        class FunctionFieldGetter : public FieldGetterBase
        {
        public:
            using Signature = ReturnType (*)(ClassType &);
            using SignatureConst = ReturnType (*)(const ClassType &);

            explicit FunctionFieldGetter(Signature getter)
                : m_getterConst(reinterpret_cast<SignatureConst>(getter)) { }

            explicit FunctionFieldGetter(SignatureConst getter)
                : m_getterConst(getter) { }

            Variant GetValue(const Variant &obj) override
            {
                auto &instance = obj.GetValue<ClassType>( );
                return m_getterConst(instance);
            }

            Variant GetValueReference(const Variant &obj) override
            {
                return getValueReference(obj);
            }

        private:
            template<typename T = ReturnType>
            Variant getValueReference(
                const Variant &obj,
                typename std::enable_if<
                    std::is_lvalue_reference<T>::value
                >::type * = nullptr
            )
            {
                auto &instance = obj.GetValue<ClassType>( );
                return Variant { m_getterConst(instance), variant_policy::NoCopy( ) };
            }

            template<typename T = ReturnType>
            Variant getValueReference(
                const Variant &obj,
                typename std::enable_if<
                    !std::is_lvalue_reference<T>::value
                >::type * = nullptr
            )
            {
                return GetValue(obj);
            }

            SignatureConst m_getterConst;
        };
    }
}
