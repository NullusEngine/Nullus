/* ----------------------------------------------------------------------------
** Copyright (c) 2016 Austin Brunkhorst, All Rights Reserved.
**
** ReflectionDatabase.h
** --------------------------------------------------------------------------*/

#pragma once

#include "BaseDef.h"
#include "TypeData.h"
#include "TypeInfo.h"

#include <functional>
#include <vector>
#include <mutex>
#include <unordered_map>
#include <unordered_set>

namespace NLS::meta
{
    class NLS_BASE_API ReflectionDatabase
    {
    public:
        using MissingTypeReporter = std::function<void(const char* subject, const char* missingType)>;

        ReflectionDatabase(void);
        ReflectionDatabase(const ReflectionDatabase &) = delete;
        ReflectionDatabase &operator=(const ReflectionDatabase &) = delete;

        ~ReflectionDatabase(void);

            std::vector<TypeData> types;

            std::unordered_map<std::string, TypeID> ids;
            std::unordered_map<TypeKey, TypeID> keyedIds;
            std::unordered_map<TypeKey, std::unordered_set<TypeKey>> moduleDependencies;

            std::unordered_map<std::string, Global> globals;
            
            std::unordered_map<
                std::string, 
                InvokableOverloadMap<Function>
            > globalFunctions;

            ////////////////////////////////////////////////////////////////////////
            ////////////////////////////////////////////////////////////////////////

            static ReflectionDatabase &Instance(void);
            static ReflectionDatabase *TryGet(void);

            ////////////////////////////////////////////////////////////////////////
            ////////////////////////////////////////////////////////////////////////

            TypeID AllocateType(const std::string &name);
            TypeID AllocateType(TypeKey key, const std::string &name, TypeKey ownerModuleKey = InvalidTypeKey);
            TypeID FindType(TypeKey key) const;
            TypeID FindType(const std::string &name) const;
            unsigned GetGeneration(TypeID id) const;
            bool IsAlive(TypeID id, unsigned generation) const;
            void UnloadModule(TypeKey moduleKey);
            bool CanUnloadModule(TypeKey moduleKey) const;
            void AddDependency(TypeKey ownerModuleKey, TypeKey referencedTypeKey);
            Type ResolveRegisteredType(
                TypeKey ownerModuleKey,
                const char* name,
                const MissingTypeReporter& reportMissingType);
            Type ResolveRegisteredArrayFieldType(
                TypeKey ownerModuleKey,
                const char* elementName,
                const MissingTypeReporter& reportMissingType);
            Type ResolveRegisteredPPtrFieldType(
                TypeKey ownerModuleKey,
                const char* elementName,
                const MissingTypeReporter& reportMissingType);
            Type ResolveRegisteredPPtrArrayFieldType(
                TypeKey ownerModuleKey,
                const char* elementName,
                const MissingTypeReporter& reportMissingType);

            mutable std::recursive_mutex mutex;

            ////////////////////////////////////////////////////////////////////////
            ////////////////////////////////////////////////////////////////////////

            // Function Getter, Function Setter
            template<typename GlobalType, typename GetterType, typename SetterType>
            void AddGlobal(
                const std::string &name,
                GetterType getter,
                SetterType setter,
                const MetaManager::Initializer &meta
            );

            // Function Getter, Raw Setter
            template<typename GlobalType, typename GetterType>
            void AddGlobal(
                const std::string &name,
                GetterType getter,
                GlobalType *globalSetter,
                const MetaManager::Initializer &meta
            );

            // Raw Getter, Function Setter
            template<typename GlobalType, typename SetterType>
            void AddGlobal(
                const std::string &name,
                GlobalType *globalGetter,
                SetterType setter,
                const MetaManager::Initializer &meta
            );

            // Raw Getter, Raw Setter
            template<typename GlobalType>
            void AddGlobal(
                const std::string &name,
                GlobalType *globalGetter,
                GlobalType *globalSetter,
                const MetaManager::Initializer &meta
            );

            ////////////////////////////////////////////////////////////////////////
            ////////////////////////////////////////////////////////////////////////

            template<typename FunctionType>
            void AddGlobalFunction(
                const std::string &name, 
                FunctionType globalFunction,
                const MetaManager::Initializer &meta
            );

            const Function &GetGlobalFunction(const std::string &name);

            const Function &GetGlobalFunction(
                const std::string &name, 
                const InvokableSignature &signature
            );

    private:
        TypeID m_nextID;
    };
}

#include "Impl/ReflectionDatabase.hpp"
