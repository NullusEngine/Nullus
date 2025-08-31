/* ----------------------------------------------------------------------------
** Copyright (c) 2016 Austin Brunkhorst, All Rights Reserved.
**
** TypeConfig.h
** --------------------------------------------------------------------------*/

#pragma once

#include "TypeID.h"
#include "MetaTraits.h"

#include <type_traits>

// Gets the type ID of a given expression
#define typeidof(expr)                                            \
    NLS::meta::TypeIDs<                                        \
        NLS::meta::CleanedType<                                \
            typename NLS::meta_traits::RemoveArray<expr>::type \
        >                                                         \
    >::ID                                                         \

// Converts the expression into a meta::Type instance
#define typeof(expr)                              \
    NLS::meta::Type(                           \
        typeidof( expr ),                         \
        NLS::meta_traits::IsArray<expr>::value \
    )                                             \

// Converts the resulting type of the given expression to a meta::Type instance
#define decltypeof(expr) typeof( decltype( expr ) )

namespace NLS
{
    namespace meta
    {
        template<typename T>
        using CleanedType = 
            typename std::remove_cv< 
                typename std::remove_reference< T >::type 
            >::type;
    }
}