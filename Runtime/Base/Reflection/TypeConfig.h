/* ----------------------------------------------------------------------------
** Copyright (c) 2016 Austin Brunkhorst, All Rights Reserved.
**
** TypeConfig.h
** --------------------------------------------------------------------------*/

#pragma once

#include "BaseDef.h"
#include "TypeID.h"
#include "TypeKey.h"
#include "MetaTraits.h"

#include <cstdint>
#include <string>
#include <type_traits>
#include <typeinfo>

namespace NLS::meta
{
    class Type;

    template<typename T>
    using CleanedType =
        typename std::remove_cv<
            typename std::remove_reference<T>::type
        >::type;

    NLS_BASE_API Type ResolveTypeByName(const char* name, bool isArray);
    NLS_BASE_API Type ResolveTypeByKey(TypeKey key, bool isArray);
    NLS_BASE_API Type ResolveTypeByID(TypeID id, bool isArray);
    NLS_BASE_API TypeID ResolveTypeIDByName(const char* name);
    NLS_BASE_API TypeID ResolveTypeIDByKey(TypeKey key);
    NLS_BASE_API TypeID ResolveTypeIDByID(TypeID id);

    template<typename T, typename = void>
    struct HasStaticMetaTypeName : std::false_type { };

    template<typename T>
    struct HasStaticMetaTypeName<T, std::void_t<decltype(T::StaticMetaTypeName())>> : std::true_type { };

    template<typename T>
    struct StaticTypeName
    {
        static constexpr const char* Value = nullptr;
    };

    template<typename T>
    constexpr const char* StaticMetaTypeName(T*)
    {
        return StaticTypeName<T>::Value;
    }

    template<typename T>
    constexpr TypeKey StaticMetaTypeKey(T*)
    {
        return InvalidTypeKey;
    }

    template<> struct StaticTypeName<void> { static constexpr const char* Value = "void"; };
    template<> struct StaticTypeName<int> { static constexpr const char* Value = "int"; };
    template<> struct StaticTypeName<unsigned int> { static constexpr const char* Value = "unsigned int"; };
    template<> struct StaticTypeName<int64_t> { static constexpr const char* Value = "int64_t"; };
    template<> struct StaticTypeName<uint64_t> { static constexpr const char* Value = "uint64_t"; };
    template<> struct StaticTypeName<bool> { static constexpr const char* Value = "bool"; };
    template<> struct StaticTypeName<float> { static constexpr const char* Value = "float"; };
    template<> struct StaticTypeName<double> { static constexpr const char* Value = "double"; };
    template<> struct StaticTypeName<std::string> { static constexpr const char* Value = "std::string"; };

    template<typename T>
    const char* GetStaticTypeName()
    {
        if constexpr (HasStaticMetaTypeName<T>::value)
            return T::StaticMetaTypeName();
        else
            return StaticMetaTypeName(static_cast<T*>(nullptr));
    }

    template<typename T, typename = void>
    struct HasStaticMetaTypeKey : std::false_type { };

    template<typename T>
    struct HasStaticMetaTypeKey<T, std::void_t<decltype(T::StaticMetaTypeKey())>> : std::true_type { };

    template<typename T>
    TypeKey GetStaticTypeKey()
    {
        if constexpr (HasStaticMetaTypeKey<T>::value)
            return T::StaticMetaTypeKey();

        if (const auto key = StaticMetaTypeKey(static_cast<T*>(nullptr)); key != InvalidTypeKey)
            return key;

        if (const auto* typeName = GetStaticTypeName<T>())
            return MakeTypeKey(typeName);

        return InvalidTypeKey;
    }

    template<typename T>
    Type ResolveType(bool isArray);

    template<typename T>
    TypeID ResolveTypeID()
    {
        using Clean = CleanedType<typename meta_traits::RemoveArray<T>::type>;
        if constexpr (std::is_pointer<Clean>::value)
        {
            using Pointed = typename std::remove_pointer<Clean>::type;
            using PointedClean = typename std::remove_const<Pointed>::type;
            if (const auto* pointedTypeName = GetStaticTypeName<PointedClean>())
            {
                const auto pointerTypeName =
                    std::is_const<Pointed>::value
                        ? std::string("const ") + pointedTypeName + "*"
                        : std::string(pointedTypeName) + "*";
                return ResolveTypeIDByKey(MakeTypeKey(pointerTypeName.c_str()));
            }
        }

        if (const auto key = GetStaticTypeKey<Clean>(); key != InvalidTypeKey)
            return ResolveTypeIDByKey(key);

        if (const auto id = ResolveTypeIDByKey(MakeTypeKey(typeid(Clean).name())); id != InvalidTypeID)
            return id;

        return ResolveTypeIDByID(TypeIDs<Clean>::ID);
    }
}

// Gets the type ID of a given expression
#define NLS_TYPEIDOF(expr) \
    NLS::meta::ResolveTypeID<expr>()

// Converts the expression into a meta::Type instance
#define NLS_TYPEOF(expr)                          \
    NLS::meta::ResolveType<expr>(NLS::meta_traits::IsArray<expr>::value)

// Converts the resulting type of the given expression to a meta::Type instance
#define NLS_DECLTYPEOF(expr) NLS_TYPEOF( decltype( expr ) )

