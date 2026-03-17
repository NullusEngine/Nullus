/* ----------------------------------------------------------------------------
** Copyright (c) 2016 Austin Brunkhorst, All Rights Reserved.
**
** TypeInfo.h
** --------------------------------------------------------------------------*/

#pragma once

#include "TypeConfig.h"

#include "../Common/Compiler.h"

#include <type_traits>

#define IsMetaDefaultConstructible(x) std::is_default_constructible<x>::value

namespace NLS
{
    namespace meta
    {
        struct TypeData;

        template<typename T>
        struct TypeInfo
        {
            static bool Defined;

            static void Register(TypeID id, TypeData &data, bool beingDefined);

        private:
            template<typename U = T>
            static void addDefaultConstructor(
                TypeData &data, 
                typename std::enable_if<
                    !IsMetaDefaultConstructible(U)
                >::type* = nullptr
            );

            template<typename U = T>
            static void addDefaultConstructor(
                TypeData &data, 
                typename std::enable_if<
                    IsMetaDefaultConstructible(U)
                >::type* = nullptr
            );

            template<typename U = T>
            static void applyTrivialAttributes(TypeData &data, 
                typename std::enable_if< 
                    !std::is_trivial<U>::value 
                >::type* = nullptr
            );

            template<typename U = T>
            static void applyTrivialAttributes(TypeData &data, 
                typename std::enable_if< 
                std::is_trivial<U>::value 
                >::type* = nullptr
            );

            template<typename U = T>
            static void applyArrayAttributes(
                TypeData &data,
                typename std::enable_if<
                    !std::is_void<U>::value
                    && !std::is_abstract<U>::value
                    && std::is_copy_constructible<U>::value
                    && std::is_copy_assignable<U>::value
                >::type* = nullptr
            );

            template<typename U = T>
            static void applyArrayAttributes(
                TypeData &data,
                typename std::enable_if<
                    std::is_void<U>::value
                    || std::is_abstract<U>::value
                    || !std::is_copy_constructible<U>::value
                    || !std::is_copy_assignable<U>::value
                >::type* = nullptr
            );
        };
    }
}

#include "Impl/TypeInfo.hpp"
