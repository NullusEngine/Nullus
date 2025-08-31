/* ----------------------------------------------------------------------------
** Copyright (c) 2016 Austin Brunkhorst, All Rights Reserved.
**
** Function.cpp
** --------------------------------------------------------------------------*/

#include "Precompiled.h"

#include "Function.h"

#include "Debug/Assertion.h"

namespace NLS
{
    namespace meta
    {
        Function::Function(void)
            : Invokable( )
            , m_parentType( Type::Invalid( ) )
            , m_invoker( nullptr ) { }

        const Function &Function::Invalid(void)
        {
            static Function invalid;

            return invalid;
        }

        Type Function::GetParentType(void) const
        {
            return m_parentType;
        }

        bool Function::IsValid(void) const
        {
            return m_invoker != nullptr;
        }

        Variant Function::InvokeVariadic(ArgumentList &arguments) const
        {
        #if defined(_DEBUG)

            NLS_ASSERT( IsValid( ), "Invalid function invocation." );

        #endif
        
            return m_invoker->Invoke( arguments );
        }
    }
}
