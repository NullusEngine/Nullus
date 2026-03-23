/* ----------------------------------------------------------------------------
** Copyright (c) 2016 Austin Brunkhorst, All Rights Reserved.
**
** ReflectionModule.cpp
** --------------------------------------------------------------------------*/

#include "Precompiled.h"

#include "ReflectionModule.h"

#include <algorithm>
#include <vector>

namespace NLS::meta
{
    namespace
    {
        std::vector<ReflectionRegisterFunction> &GetReflectionRegisterFunctions()
        {
            static std::vector<ReflectionRegisterFunction> functions;
            return functions;
        }
    }

    void ReflectionModuleRegistry::Add(ReflectionRegisterFunction function)
    {
        if (!function)
            return;

            auto &functions = GetReflectionRegisterFunctions();
            if (std::find(functions.begin(), functions.end(), function) == functions.end())
            {
                functions.push_back(function);
                if (auto *db = ReflectionDatabase::TryGet())
                {
                    function(*db, ReflectionRegistrationPhase::Declare);
                    function(*db, ReflectionRegistrationPhase::Define);
                }
            }
        }

    void ReflectionModuleRegistry::RegisterAll(ReflectionDatabase &db)
    {
        auto &functions = GetReflectionRegisterFunctions();

            for (auto function : functions)
            {
                function(db, ReflectionRegistrationPhase::Declare);
            }

        for (auto function : functions)
        {
            function(db, ReflectionRegistrationPhase::Define);
        }
    }
}
