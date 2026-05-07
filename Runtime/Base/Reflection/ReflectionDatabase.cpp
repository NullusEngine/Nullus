/* ----------------------------------------------------------------------------
** Copyright (c) 2016 Austin Brunkhorst, All Rights Reserved.
**
** ReflectionDatabase.cpp
** --------------------------------------------------------------------------*/

#include "Precompiled.h"

#include "ReflectionDatabase.h"

#include "ReflectionModule.h"
#include "ReflectionDiagnostics.h"
#include "Object.h"
#include "Type.h"

#include "MetaGenerated.h"

#define REGISTER_NATIVE_TYPE(type)                                      \
    {                                                                   \
        auto id = AllocateType(MakeTypeKey( #type ), #type );           \
        auto &handle = types[ id ];                                     \
                                                                        \
        TypeInfo<type>::Register( id, handle, true );                   \
    }                                                 \

#define REGISTER_NATIVE_TYPE_VARIANTS(type) \
    REGISTER_NATIVE_TYPE( type )            \
    REGISTER_NATIVE_TYPE( type* )           \
    REGISTER_NATIVE_TYPE( const type* )     \

#define REGISTER_NATIVE_TYPE_VARIANTS_W_ARRAY(type)         \
    REGISTER_NATIVE_TYPE_VARIANTS( type )                   \
    types[ NLS_TYPEIDOF( type ) ].SetArrayConstructor<type>( ); \

namespace NLS::meta
{
    namespace
    {
        ReflectionDatabase *gReflectionDatabaseInstance = nullptr;
    }

    ReflectionDatabase::ReflectionDatabase(void)
        : types( 1 )
        , m_nextID( 1 )
    {
        gReflectionDatabaseInstance = this;
        NLS_META_GENERATED_LINK_FUNCTION();

            types[ InvalidTypeID ].name = "UNKNOWN";

            // register all of the native type variants explicity, before
            // anything else
            REGISTER_NATIVE_TYPE_VARIANTS( void );
            REGISTER_NATIVE_TYPE_VARIANTS_W_ARRAY( int );
            REGISTER_NATIVE_TYPE_VARIANTS_W_ARRAY( unsigned int );
            REGISTER_NATIVE_TYPE_VARIANTS_W_ARRAY( int64_t );
            REGISTER_NATIVE_TYPE_VARIANTS_W_ARRAY( uint64_t );
            REGISTER_NATIVE_TYPE_VARIANTS_W_ARRAY( bool );
            REGISTER_NATIVE_TYPE_VARIANTS_W_ARRAY( float );
            REGISTER_NATIVE_TYPE_VARIANTS_W_ARRAY( double );
            REGISTER_NATIVE_TYPE_VARIANTS_W_ARRAY( std::string );
            REGISTER_NATIVE_TYPE_VARIANTS( NLS::meta::Object );

            auto &stringType = types[ NLS_TYPEIDOF( std::string ) ];

            // explicitly add default constructors for string

            stringType.AddConstructor<std::string, false, false>( { } );
            stringType.AddConstructor<std::string, false, true>( { } );

            // Register user-defined reflection types discovered by static registrars.
        ReflectionModuleRegistry::RegisterAll(*this);
    }

        ///////////////////////////////////////////////////////////////////////

        ReflectionDatabase::~ReflectionDatabase(void) { }

        ///////////////////////////////////////////////////////////////////////

        ReflectionDatabase &ReflectionDatabase::Instance(void)
        {
            static ReflectionDatabase instance;

            return instance;
        }

        ReflectionDatabase *ReflectionDatabase::TryGet(void)
        {
            return gReflectionDatabaseInstance;
        }

        ///////////////////////////////////////////////////////////////////////

        TypeID ReflectionDatabase::AllocateType(const std::string &name)
        {
            return AllocateType(MakeTypeKey(name.c_str( )), name);
        }

        TypeID ReflectionDatabase::AllocateType(TypeKey key, const std::string &name, TypeKey ownerModuleKey)
        {
            std::scoped_lock lock(mutex);
            if (key == InvalidTypeKey)
                key = MakeTypeKey(name.c_str( ));

            if (auto search = keyedIds.find(key); search != keyedIds.end( ))
                return InvalidTypeID;

            if (auto search = ids.find(name); search != ids.end( ))
                return InvalidTypeID;

            types.emplace_back(key, name, ownerModuleKey);

            auto id = m_nextID++;

            ids[ name ] = id;
            keyedIds[ key ] = id;

            return id;
        }

        TypeID ReflectionDatabase::FindType(TypeKey key) const
        {
            std::scoped_lock lock(mutex);
            auto search = keyedIds.find(key);
            return search != keyedIds.end( ) ? search->second : InvalidTypeID;
        }

        TypeID ReflectionDatabase::FindType(const std::string &name) const
        {
            std::scoped_lock lock(mutex);
            auto search = ids.find(name);
            return search != ids.end( ) ? search->second : InvalidTypeID;
        }

        unsigned ReflectionDatabase::GetGeneration(TypeID id) const
        {
            std::scoped_lock lock(mutex);
            return id < types.size( ) ? types[ id ].generation : 0;
        }

        bool ReflectionDatabase::IsAlive(TypeID id, unsigned generation) const
        {
            std::scoped_lock lock(mutex);
            return id != InvalidTypeID && id < types.size( ) && types[ id ].generation == generation && types[ id ].key != InvalidTypeKey;
        }

        void ReflectionDatabase::UnloadModule(TypeKey moduleKey)
        {
            std::scoped_lock lock(mutex);
            if (moduleKey == InvalidTypeKey)
                return;

            if (!CanUnloadModule(moduleKey))
            {
                ReflectionDiagnostics::Report(
                    ReflectionDiagnosticSeverity::Error,
                    moduleKey,
                    nullptr,
                    nullptr,
                    "module unload blocked",
                    "module still has reflected types referenced by another module"
                );
                return;
            }

            for (TypeID id = 1; id < types.size( ); ++id)
            {
                auto &type = types[ id ];
                if (type.ownerModuleKey != moduleKey)
                    continue;

                ids.erase(type.name);
                keyedIds.erase(type.key);
                type.ResetForUnload( );
            }

            moduleDependencies.erase(moduleKey);
        }

        bool ReflectionDatabase::CanUnloadModule(TypeKey moduleKey) const
        {
            std::scoped_lock lock(mutex);
            if (moduleKey == InvalidTypeKey)
                return false;

            for (const auto &[otherModuleKey, dependencies] : moduleDependencies)
            {
                if (otherModuleKey == moduleKey)
                    continue;

                for (TypeID id = 1; id < types.size( ); ++id)
                {
                    const auto &type = types[ id ];
                    if (type.ownerModuleKey != moduleKey)
                        continue;

                    if (dependencies.find(type.key) != dependencies.end( ))
                        return false;
                }
            }

            return true;
        }

        void ReflectionDatabase::AddDependency(TypeKey ownerModuleKey, TypeKey referencedTypeKey)
        {
            std::scoped_lock lock(mutex);
            if (ownerModuleKey == InvalidTypeKey || referencedTypeKey == InvalidTypeKey)
                return;

            moduleDependencies[ ownerModuleKey ].insert(referencedTypeKey);
        }

        ///////////////////////////////////////////////////////////////////////

        const Function &ReflectionDatabase::GetGlobalFunction(
            const std::string &name
        )
        {
            auto &base = globalFunctions[ name ];

            if (!base.size( ))
                return Function::Invalid( );

            return base.begin( )->second;
        }

        ///////////////////////////////////////////////////////////////////////

    const Function &ReflectionDatabase::GetGlobalFunction(
        const std::string &name,
        const InvokableSignature &signature
    )
    {
        auto &base = globalFunctions[ name ];

            auto search = base.find( signature );

            if (search == base.end( ))
                return Function::Invalid( );

        return search->second;
    }
}
