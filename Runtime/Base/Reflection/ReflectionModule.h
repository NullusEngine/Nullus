/* ----------------------------------------------------------------------------
** Copyright (c) 2016 Austin Brunkhorst, All Rights Reserved.
**
** ReflectionModule.h
** --------------------------------------------------------------------------*/

#pragma once

#include "BaseDef.h"
#include "ReflectionDatabase.h"

namespace NLS::meta
{
    enum class ReflectionRegistrationPhase
    {
        Declare,
        Define
    };

    using ReflectionRegisterFunction = void(*)(ReflectionDatabase &db, ReflectionRegistrationPhase phase);

    struct ReflectionModuleDescriptor
    {
        TypeKey key = InvalidTypeKey;
        const char* name = nullptr;
        ReflectionRegisterFunction function = nullptr;
        TypeKey registrationKey = InvalidTypeKey;
    };

    class NLS_BASE_API ReflectionModuleRegistry
    {
    public:
        static void Add(ReflectionRegisterFunction function);
        static void Add(TypeKey moduleKey, const char* moduleName, ReflectionRegisterFunction function);
        static void Add(TypeKey moduleKey, const char* moduleName, ReflectionRegisterFunction function,
            TypeKey registrationKey);
        static void RegisterAll(ReflectionDatabase &db);
        static void Unload(TypeKey moduleKey);
    };

    template<ReflectionRegisterFunction Function>
    class AutoReflectionModuleRegistrar
    {
    public:
        AutoReflectionModuleRegistrar()
        {
            ReflectionModuleRegistry::Add(Function);
        }
    };
}
