/* ----------------------------------------------------------------------------
** Copyright (c) 2016 Austin Brunkhorst, All Rights Reserved.
**
** FunctionFieldSetter.h
** --------------------------------------------------------------------------*/

#pragma once

#include "FieldSetterBase.h"

namespace NLS
{
    namespace meta
    {
        template<typename ClassType, typename ArgumentType>
        class FunctionFieldSetter : public FieldSetterBase
        {
        public:
            using NonReferenceArgType = typename std::remove_reference<ArgumentType>::type;
            using Signature = void (*)(ClassType &, ArgumentType);

            explicit FunctionFieldSetter(Signature setter)
                : m_setter(setter) { }

            void SetValue(Variant &obj, const Variant &value) override
            {
                NLS_ASSERT( value.IsValid( ), "Setting invalid value." );

                auto &instance = obj.GetValue<ClassType>( );
                m_setter(instance, value.GetValue<NonReferenceArgType>( ));
            }

        private:
            Signature m_setter;
        };
    }
}
