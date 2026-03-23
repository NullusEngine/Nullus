/* ----------------------------------------------------------------------------
** Copyright (c) 2016 Austin Brunkhorst, All Rights Reserved.
**
** ReflectionModule.h
** --------------------------------------------------------------------------*/

#pragma once

#include "ReflectionDatabase.h"

namespace NLS::meta
{
    enum class ReflectionRegistrationPhase
    {
        Declare,
        Define
    };

    using ReflectionRegisterFunction = void(*)(ReflectionDatabase &db, ReflectionRegistrationPhase phase);

    class ReflectionModuleRegistry
    {
    public:
        static void Add(ReflectionRegisterFunction function);
        static void RegisterAll(ReflectionDatabase &db);
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
