#pragma once

#include "Reflection/ReflectionModule.h"
#include "Reflection/TypeData.h"
#include "Reflection/TypeInfo.h"

#define NLS_META_EXTERNAL_JOIN_INNER(left, right) left##right
#define NLS_META_EXTERNAL_JOIN(left, right) NLS_META_EXTERNAL_JOIN_INNER(left, right)
#define NLS_META_EXTERNAL_UNIQUE(prefix) NLS_META_EXTERNAL_JOIN(prefix, __COUNTER__)

#define NLS_META_EXTERNAL_TYPE_NAME(type)            \
    inline constexpr const char* StaticMetaTypeName(type*) { return #type; } \
    inline constexpr ::NLS::meta::TypeKey StaticMetaTypeKey(type*) { return ::NLS::meta::HashTypeKey(#type); }

#define NLS_META_EXTERNAL_BEGIN(type)                                      \
    do                                                                     \
    {                                                                      \
        using MetaExternalType = type;                                     \
        constexpr auto metaExternalTypeName = #type;                      \
        constexpr auto metaExternalModuleKey =                            \
            ::NLS::meta::HashTypeKey(__FILE__);                           \
        constexpr auto metaExternalTypeKey =                              \
            ::NLS::meta::HashTypeKey(metaExternalTypeName);               \
        constexpr auto metaExternalPointerTypeKey =                       \
            ::NLS::meta::HashTypeKey(#type "*");                         \
        constexpr auto metaExternalConstPointerTypeKey =                  \
            ::NLS::meta::HashTypeKey("const " #type "*");                \
        auto metaExternalId = db.FindType(metaExternalTypeKey);           \
        if (phase == ::NLS::meta::ReflectionRegistrationPhase::Declare)   \
        {                                                                  \
            if (metaExternalId == ::NLS::meta::InvalidTypeID)             \
            {                                                              \
                metaExternalId =                                          \
                    db.AllocateType(metaExternalTypeKey,                  \
                        metaExternalTypeName, metaExternalModuleKey);     \
            }                                                              \
            if (metaExternalId != ::NLS::meta::InvalidTypeID)             \
            {                                                              \
                auto& metaExternalType = db.types[metaExternalId];        \
                metaExternalType.name = metaExternalTypeName;             \
                ::NLS::meta::TypeInfo<MetaExternalType>::Register(        \
                    metaExternalId, metaExternalType, true);              \
            }                                                              \
            const auto metaExternalPointerId =                            \
                db.AllocateType(metaExternalPointerTypeKey,               \
                    #type "*", metaExternalModuleKey);                   \
            if (metaExternalPointerId != ::NLS::meta::InvalidTypeID)      \
            {                                                              \
                ::NLS::meta::TypeInfo<MetaExternalType*>::Register(       \
                    metaExternalPointerId, db.types[metaExternalPointerId], true); \
            }                                                              \
            const auto metaExternalConstPointerId =                       \
                db.AllocateType(metaExternalConstPointerTypeKey,          \
                    "const " #type "*", metaExternalModuleKey);          \
            if (metaExternalConstPointerId != ::NLS::meta::InvalidTypeID) \
            {                                                              \
                ::NLS::meta::TypeInfo<const MetaExternalType*>::Register( \
                    metaExternalConstPointerId, db.types[metaExternalConstPointerId], true); \
            }                                                              \
            break;                                                         \
        }                                                                  \
        if (metaExternalId == ::NLS::meta::InvalidTypeID)                 \
            break;                                                         \
        auto& metaExternalType = db.types[metaExternalId];

#define NLS_META_EXTERNAL_END()                                           \
    } while (false)

#define NLS_META_EXTERNAL_FIELD(type, field)                              \
    metaExternalType.AddField<MetaExternalType, type>(                    \
        #field, &MetaExternalType::field, &MetaExternalType::field, {})

#define NLS_META_EXTERNAL_FIELD_NAMED(type, name, getter, setter)         \
    metaExternalType.AddField<MetaExternalType, type>(                    \
        name, getter, setter, {})

#define NLS_META_EXTERNAL_METHOD(name, pointer)                           \
    metaExternalType.AddMethod(name, pointer, {})

#define NLS_META_EXTERNAL_STATIC_METHOD(name, pointer)                    \
    metaExternalType.AddStaticMethod<MetaExternalType>(name, pointer, {})

#define NLS_META_EXTERNAL_MODULE_NAMED(functionName, registrarName)       \
    namespace                                                             \
    {                                                                     \
        struct registrarName                                              \
        {                                                                 \
            registrarName()                                               \
            {                                                             \
                ::NLS::meta::ReflectionModuleRegistry::Add(               \
                    ::NLS::meta::HashTypeKey(__FILE__), __FILE__,         \
                    &functionName);                                       \
            }                                                             \
        };                                                                \
        static registrarName NLS_META_EXTERNAL_JOIN(g_, registrarName);   \
    }

#define NLS_META_EXTERNAL_MODULE(functionName)                            \
    NLS_META_EXTERNAL_MODULE_NAMED(functionName, NLS_META_EXTERNAL_UNIQUE(MetaExternalRegistrar_))
