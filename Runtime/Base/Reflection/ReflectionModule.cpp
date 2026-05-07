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
        if (!function)
            return;

            std::scoped_lock lock(GetReflectionModuleMutex());
            auto &modules = GetReflectionModules();
            const auto found = std::find_if(
                modules.begin(),
                modules.end(),
                [function](const ReflectionModuleDescriptor &module)
                {
                    return module.function == function;
                });
            if (found == modules.end())
            {
                modules.push_back({ moduleKey, moduleName, function });
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
