/* ----------------------------------------------------------------------------
** Copyright (c) 2016 Austin Brunkhorst, All Rights Reserved.
**
** EnumBase.cpp
** --------------------------------------------------------------------------*/

#include "Precompiled.h"

#include "EnumBase.h"

#include "Variant.h"
#include "Argument.h"

namespace NLS
{
    namespace meta
    {
        EnumBase::EnumBase(const std::string &name, TypeID owner)
            : m_parentType( owner )
            , m_name( name ) { }

        Type EnumBase::GetParentType(void) const
        {
            return m_parentType;
        }

        const std::string &EnumBase::GetName(void) const
        {
            return m_name;
        }
    }
}