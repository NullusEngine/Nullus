/* ----------------------------------------------------------------------------
** Copyright (c) 2016 Austin Brunkhorst, All Rights Reserved.
**
** ObjectWrapper.cpp
** --------------------------------------------------------------------------*/

#include "Precompiled.h"

#include "ObjectWrapper.h"
#include "Type.h"

namespace NLS::meta
{
    ObjectWrapper::ObjectWrapper(NLS::Object* instance, Type type)
        : m_object(instance)
        , m_typeID(type.GetID())
        , m_typeIsArray(type.IsArray())
    {
    }

    Type ObjectWrapper::GetType(void) const
    {
        return ResolveTypeByID(m_typeID, m_typeIsArray);
    }

    void *ObjectWrapper::GetPtr(void) const
    {
        return m_object;
    }

    int ObjectWrapper::ToInt(void) const
    {
        return int();
    }

    bool ObjectWrapper::ToBool(void) const
    {
        return bool();
    }

    float ObjectWrapper::ToFloat(void) const
    {
        return float();
    }

    double ObjectWrapper::ToDouble(void) const
    {
        return double();
    }

    std::string ObjectWrapper::ToString(void) const
    {
        return std::string();
    }

    VariantBase *ObjectWrapper::Clone(void) const
    {
        return new ObjectWrapper(m_object, GetType());
    }
}
