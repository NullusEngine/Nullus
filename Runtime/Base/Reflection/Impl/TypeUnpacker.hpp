/* ----------------------------------------------------------------------------
** Copyright (c) 2016 Austin Brunkhorst, All Rights Reserved.
**
** TypeUnpacker.hpp
** --------------------------------------------------------------------------*/

#pragma once

namespace NLS
{
    namespace meta
    {
        template<class... Types>
        struct TypeUnpacker { };

        template<>
        struct TypeUnpacker<>
        {
            static void Apply(Type::List &types) { }
        };

        template<class First, class... Types>
        struct TypeUnpacker<First, Types...>
        {
            static void Apply(Type::List &types)
            {
                types.emplace_back( typeof( First ) );

                TypeUnpacker<Types...>::Apply( types );
            }
        };
    }
}