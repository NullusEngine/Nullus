/* ----------------------------------------------------------------------------
** Copyright (c) 2016 Austin Brunkhorst, All Rights Reserved.
**
** Method.cpp
** --------------------------------------------------------------------------*/

#include "Precompiled.h"

#include "Method.h"

#include "Debug/Assertion.h"

namespace NLS
{
    namespace meta
    {
        Method::Method(void)
            : Invokable( )
            , m_isConst( true )
            , m_classType( Type::Invalid( ) )
            , m_invoker( nullptr ) { }

        const Method &Method::Invalid(void)
        {
            static Method invalid;

            return invalid;
        }

        Type Method::GetClassType(void) const
        {
            return m_classType;
        }

        bool Method::IsValid(void) const
        {
            return m_invoker != nullptr;
        }

        bool Method::IsConst(void) const
        {
            return m_isConst;
        }

        Variant Method::Invoke(
            Variant &instance,
            ArgumentList &arguments
        ) const
        {
        #if defined(_DEBUG)

            NLS_ASSERT( IsValid( ), 
                "Invalid method invoked." 
            );

            NLS_ASSERT( !(instance.IsConst( ) && !m_isConst), 
                "Non-const method invoked on const object." 
            );

            NLS_ASSERT( instance.GetType( ) == m_classType, 
                "Incompatible method invoked with instance." 
            );

        #endif

            return m_invoker->Invoke( instance, arguments );
        }
    }
}
