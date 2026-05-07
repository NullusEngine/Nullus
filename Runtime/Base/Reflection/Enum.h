/* ----------------------------------------------------------------------------
** Copyright (c) 2016 Austin Brunkhorst, All Rights Reserved.
**
** Enum.h
** --------------------------------------------------------------------------*/
#pragma once

#include "BaseDef.h"
#include "EnumBase.h"

namespace NLS::meta
{
    class NLS_BASE_API Enum
    {
    public:
        bool IsValid(void) const;

        operator bool(void) const;

        bool operator ==(const Enum &rhs) const;
        bool operator !=(const Enum &rhs) const;

        std::string GetName(void) const;

        Type GetType(void) const;
        Type GetParentType(void) const;
        Type GetUnderlyingType(void) const;

        std::vector<std::string> GetKeys(void) const;
        std::vector<Variant> GetValues(void) const;

        std::string GetKey(const Argument &value) const;
        Variant GetValue(const std::string &key) const;

    private:
        friend struct TypeData;

        Enum(const EnumBase *base);

        std::shared_ptr<const EnumBase> m_base;
    };
}
