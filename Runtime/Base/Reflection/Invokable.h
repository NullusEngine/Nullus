/* ----------------------------------------------------------------------------
** Copyright (c) 2016 Austin Brunkhorst, All Rights Reserved.
**
** Invokable.h
** --------------------------------------------------------------------------*/

#pragma once

#include "BaseDef.h"
#include "InvokableConfig.h"
#include "ArgumentConfig.h"

#include <vector>

namespace NLS::meta
{
    class Type;

    class NLS_BASE_API Invokable
    {
    public:
        Invokable(const std::string &name = "INVALID");

        template<typename ...Types>
        static InvokableSignature CreateSignature(void);

        const InvokableSignature &GetSignature(void) const;

        const std::string &GetName(void) const;

    protected:
        std::string m_name;

        InvokableSignature m_signature;
    };
}

#include "Impl/Invokable.hpp"
