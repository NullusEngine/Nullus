/* ----------------------------------------------------------------------------
** Copyright (c) 2016 Austin Brunkhorst, All Rights Reserved.
**
** ReflectionModule.cpp
** --------------------------------------------------------------------------*/

#include "Precompiled.h"

#include "ReflectionModule.h"

#include <algorithm>
#include <mutex>
#include <vector>

namespace NLS::meta
{
    namespace
    {
        std::vector<ReflectionModuleDescriptor> &GetReflectionModules()
        {
            static std::vector<ReflectionModuleDescriptor> modules;
            return modules;
        }

        std::recursive_mutex &GetReflectionModuleMutex()
        {
            static std::recursive_mutex mutex;
            return mutex;
        }
    }

    void ReflectionModuleRegistry::Add(ReflectionRegisterFunction function)
    {
        Add(InvalidTypeKey, nullptr, function);
    }

    void ReflectionModuleRegistry::Add(TypeKey moduleKey, const char* moduleName, ReflectionRegisterFunction function)
    {
        Add(moduleKey, moduleName, function, InvalidTypeKey);
    }

    void ReflectionModuleRegistry::Add(TypeKey moduleKey, const char* moduleName, ReflectionRegisterFunction function,
        TypeKey registrationKey)
    {
        if (!function)
            return;

            std::scoped_lock lock(GetReflectionModuleMutex());
            auto &modules = GetReflectionModules();
            const auto found = std::find_if(
                modules.begin(),
                modules.end(),
                [moduleKey, registrationKey, function](const ReflectionModuleDescriptor &module)
                {
                    // Header registrations have different function addresses in each
                    // shared library. Keep their stable identity separate from the
                    // owner module, which may legitimately contain many callbacks.
                    return module.function == function ||
                        (registrationKey != InvalidTypeKey && module.key == moduleKey &&
                         module.registrationKey == registrationKey);
                });
            if (found == modules.end())
            {
                modules.push_back({ moduleKey, moduleName, function, registrationKey });
                if (auto *db = ReflectionDatabase::TryGet())
                {
                    function(*db, ReflectionRegistrationPhase::Declare);
                    function(*db, ReflectionRegistrationPhase::Define);
                }
            }
        }

    void ReflectionModuleRegistry::RegisterAll(ReflectionDatabase &db)
    {
        std::scoped_lock lock(GetReflectionModuleMutex());
        auto &modules = GetReflectionModules();

            for (const auto &module : modules)
            {
                module.function(db, ReflectionRegistrationPhase::Declare);
            }

        for (const auto &module : modules)
        {
            module.function(db, ReflectionRegistrationPhase::Define);
        }
    }

    void ReflectionModuleRegistry::Unload(TypeKey moduleKey)
    {
        if (moduleKey == InvalidTypeKey)
            return;

        std::scoped_lock lock(GetReflectionModuleMutex());
        auto &modules = GetReflectionModules();
        modules.erase(
            std::remove_if(
                modules.begin(),
                modules.end(),
                [moduleKey](const ReflectionModuleDescriptor &module)
                {
                    return module.key == moduleKey;
                }),
            modules.end());

        if (auto *db = ReflectionDatabase::TryGet())
            db->UnloadModule(moduleKey);
    }
}
