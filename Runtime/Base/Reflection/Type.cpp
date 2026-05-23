/* ----------------------------------------------------------------------------
** Copyright (c) 2016 Austin Brunkhorst, All Rights Reserved.
**
** Type.cpp
** --------------------------------------------------------------------------*/

#include "Precompiled.h"

#include "Type.h"
#include "TypeCreator.h"

#include "Variant.h"
#include "Enum.h"

#include "Constructor.h"
#include "Destructor.h"
#include "Field.h"
#include "Method.h"
#include "Function.h"

#include "MetaManager.h"
#include "RuntimeMetaProperties.h"

#include "ReflectionDatabase.h"

#include "Debug/Assertion.h"

#include <cctype>

namespace NLS::meta
{
    namespace
    {
        // make sure we always have a reference to the gDatabase
        #define gDatabase ReflectionDatabase::Instance( )

        std::string NormalizeTypeLookupName(const char* name)
        {
            if (name == nullptr)
                return {};

            std::string normalized = name;

            constexpr const char* prefixes[] = { "class ", "struct ", "enum " };
            bool strippedPrefix = true;
            while (strippedPrefix)
            {
                strippedPrefix = false;
                for (const char* prefix : prefixes)
                {
                    const std::string_view prefixView(prefix);
                    if (normalized.rfind(prefixView, 0) == 0)
                    {
                        normalized.erase(0, prefixView.size());
                        strippedPrefix = true;
                    }
                }
            }

            return normalized;
        }

    }

    Type::Type(void)
        : m_id( InvalidTypeID )
        , m_generation( 0 )
        , m_isArray( false ) { }

        ///////////////////////////////////////////////////////////////////////

        Type::Type(const Type &rhs)
            : m_id( rhs.m_id )
            , m_generation( rhs.m_generation )
            , m_isArray( rhs.m_isArray ) { }

        ///////////////////////////////////////////////////////////////////////

        Type::Type(TypeID id, bool isArray)
            : m_id( id )
            , m_generation( ReflectionDatabase::TryGet( ) ? ReflectionDatabase::TryGet( )->GetGeneration( id ) : 0 )
            , m_isArray( isArray ) { }

        Type::Type(TypeID id, bool isArray, unsigned generation)
            : m_id( id )
            , m_generation( generation )
            , m_isArray( isArray ) { }

        ///////////////////////////////////////////////////////////////////////

        Type::operator bool(void) const
        {
            return m_id != InvalidTypeID;
        }

        ///////////////////////////////////////////////////////////////////////

        Type &Type::operator=(const Type &rhs)
        {
            m_id = rhs.m_id;
            m_generation = rhs.m_generation;
            m_isArray = rhs.m_isArray;

            return *this;
        }

        ///////////////////////////////////////////////////////////////////////

        bool Type::operator<(const Type &rhs) const
        {
            return m_id < rhs.m_id || (m_id == rhs.m_id && m_generation < rhs.m_generation);
        }

        ///////////////////////////////////////////////////////////////////////

        bool Type::operator>(const Type &rhs) const
        {
            return rhs < *this;
        }

        ///////////////////////////////////////////////////////////////////////

        bool Type::operator<=(const Type &rhs) const
        {
            return !(rhs < *this);
        }

        ///////////////////////////////////////////////////////////////////////

        bool Type::operator>=(const Type &rhs) const
        {
            return !(*this < rhs);
        }

        ///////////////////////////////////////////////////////////////////////

        bool Type::operator==(const Type &rhs) const
        {
            return m_id == rhs.m_id && m_isArray == rhs.m_isArray;
        }

        ///////////////////////////////////////////////////////////////////////

        bool Type::operator!=(const Type &rhs) const
        {
            return !(*this == rhs);
        }

        ///////////////////////////////////////////////////////////////////////

        const Type &Type::Invalid(void)
        {
            static const Type invalid { InvalidTypeID };

            return invalid;
        }

        ///////////////////////////////////////////////////////////////////////

        TypeID Type::GetID(void) const
        {
            return IsValid( ) ? m_id : InvalidTypeID;
        }

        TypeKey Type::GetKey(void) const
        {
            return IsValid( ) ? gDatabase.types[ m_id ].key : InvalidTypeKey;
        }

        ///////////////////////////////////////////////////////////////////////

        Type::List Type::GetTypes(void)
        {
            auto count = gDatabase.types.size( );

            List types;

            // skip the first one, as it's reserved for unknown
            for (TypeID i = 1; i < count; ++i)
                types.emplace_back( i );

            return types;
        }

        ///////////////////////////////////////////////////////////////////////

        std::vector<Global> Type::GetGlobals(void)
        {
            std::vector<Global> globals;

            for (auto &global : gDatabase.globals)
                globals.emplace_back( global.second );

            return globals;
        }

        ///////////////////////////////////////////////////////////////////////

        const Global &Type::GetGlobal(const std::string &name)
        {
            return gDatabase.globals[ name ];
        }

        ///////////////////////////////////////////////////////////////////////

        std::vector<Function> Type::GetGlobalFunctions(void)
        {
            std::vector<Function> functions;

            for (auto &overload : gDatabase.globalFunctions)
            {
                for (auto &function : overload.second)
                {
                    functions.emplace_back( function.second );
                }
            }

            return functions;
        }

        ///////////////////////////////////////////////////////////////////////

        const Function &Type::GetGlobalFunction(const std::string &name)
        {
            return gDatabase.GetGlobalFunction( name );
        }

        ///////////////////////////////////////////////////////////////////////

        const Function &Type::GetGlobalFunction(
            const std::string &name, 
            const InvokableSignature &signature
        )
        {
            return gDatabase.GetGlobalFunction( name, signature );
        }

        ///////////////////////////////////////////////////////////////////////

        Type Type::GetFromName(const std::string &name)
        {
            auto search = gDatabase.ids.find( name );

            if (search == gDatabase.ids.end( ))
                return Invalid( );

            return { search->second };
        }

        ///////////////////////////////////////////////////////////////////////

        Type ResolveTypeByName(const char* name, bool isArray)
        {
            return ResolveTypeByID(ResolveTypeIDByName(name), isArray);
        }

        Type ResolveTypeByKey(TypeKey key, bool isArray)
        {
            return ResolveTypeByID(ResolveTypeIDByKey(key), isArray);
        }

        Type ResolveTypeByID(TypeID id, bool isArray)
        {
            return { id, isArray };
        }

        TypeKey MakeTypeKey(const char* stableName)
        {
            const auto normalizedName = NormalizeTypeLookupName(stableName);
            return normalizedName.empty( ) ? InvalidTypeKey : HashTypeKey(normalizedName.c_str( ));
        }

        TypeID ResolveTypeIDByName(const char* name)
        {
            return ResolveTypeIDByKey(MakeTypeKey(name));
        }

        TypeID ResolveTypeIDByKey(TypeKey key)
        {
            if (key == InvalidTypeKey)
                return InvalidTypeID;
            
            auto* db = ReflectionDatabase::TryGet( );
            if (db == nullptr)
                db = &ReflectionDatabase::Instance( );

            return db->FindType(key);
        }

        TypeID ResolveTypeIDByID(TypeID id)
        {
            return id;
        }

        ///////////////////////////////////////////////////////////////////////

        bool Type::ListsEqual(const List &a, const List &b)
        {
            if (a.size( ) != b.size( ))
                return false;

            auto equal = true;

            for (size_t i = 0u, size = a.size( ); equal && i < size; ++i)
                equal = (a[ i ] == b[ i ]);

            return equal;
        }

        ///////////////////////////////////////////////////////////////////////

        bool Type::IsValid(void) const
        {
            auto* db = ReflectionDatabase::TryGet( );
            return db != nullptr && db->IsAlive( m_id, m_generation );
        }

        ///////////////////////////////////////////////////////////////////////

        bool Type::IsPrimitive(void) const
        {
            if (!IsValid( ))
                return false;
            return gDatabase.types[ m_id ].isPrimitive;
        }

        ///////////////////////////////////////////////////////////////////////

        bool Type::IsFloatingPoint(void) const
        {
            if (!IsValid( ))
                return false;
            return gDatabase.types[ m_id ].isFloatingPoint;
        }

        ///////////////////////////////////////////////////////////////////////

        bool Type::IsSigned(void) const
        {
            if (!IsValid( ))
                return false;
            return gDatabase.types[ m_id ].isSigned;
        }

        ///////////////////////////////////////////////////////////////////////

        bool Type::IsEnum(void) const
        {
            if (!IsValid( ))
                return false;
            return gDatabase.types[ m_id ].isEnum;
        }

        ///////////////////////////////////////////////////////////////////////

        bool Type::IsPointer(void) const
        {
            if (!IsValid( ))
                return false;
            return gDatabase.types[ m_id ].isPointer;
        }

        ///////////////////////////////////////////////////////////////////////

        bool Type::IsClass(void) const
        {
            if (!IsValid( ))
                return false;
            return gDatabase.types[ m_id ].isClass;
        }

        ///////////////////////////////////////////////////////////////////////

        bool Type::IsArray(void) const
        {
            return m_isArray;
        }

        ///////////////////////////////////////////////////////////////////////

        std::string Type::GetName(void) const
        {
            if (!IsValid( ))
                return gDatabase.types[ InvalidTypeID ].name;

            auto &name = gDatabase.types[ m_id ].name;

            if (IsArray( ))
                return "Array<" + name + ">";

            return name;
        }

        const MetaManager &Type::GetMeta(void) const
        {
            if (!IsValid( ))
                return gDatabase.types[ InvalidTypeID ].meta;
            return gDatabase.types[ m_id ].meta;
        }

        ///////////////////////////////////////////////////////////////////////

        void Type::Destroy(Variant &instance) const
        {
            if (!IsValid( ))
                return;

            auto &destructor = gDatabase.types[ m_id ].destructor;

            if (destructor.IsValid( ))
                destructor.Invoke( instance );
        }

        ///////////////////////////////////////////////////////////////////////

        Type Type::GetDecayedType(void) const
        {
            if (!IsValid( ))
                return Type::Invalid( );

            if (!IsPointer( ))
                return Type( m_id, false, m_generation );

            std::string typeName = GetName( );
            constexpr std::string_view constPrefix = "const ";
            if (typeName.rfind( constPrefix, 0 ) == 0)
                typeName.erase( 0, constPrefix.size( ) );

            while (!typeName.empty( ) && typeName.back( ) == '*')
            {
                typeName.pop_back( );
                while (!typeName.empty( ) && std::isspace( static_cast<unsigned char>(typeName.back( )) ))
                    typeName.pop_back( );
            }

            return Type::GetFromName( typeName );

        }

        ///////////////////////////////////////////////////////////////////////

        Type Type::GetArrayType(void) const
        {
            if (!IsValid( ))
                return Type::Invalid( );
            return Type( m_id, false );
        }

        ///////////////////////////////////////////////////////////////////////

        const Enum &Type::GetEnum(void) const
        {
            if (!IsValid( ))
                return gDatabase.types[ InvalidTypeID ].enumeration;
            return gDatabase.types[ m_id ].enumeration;
        }

        ///////////////////////////////////////////////////////////////////////

        bool Type::DerivesFrom(const Type &other) const
        {
            if (!IsValid( ) || !other.IsValid( ))
                return false;

            auto &baseClasses = gDatabase.types[ m_id ].baseClasses;

            return baseClasses.find( other ) != baseClasses.end( );
        }

        ///////////////////////////////////////////////////////////////////////

        const Type::Set &Type::GetBaseClasses(void) const
        {
            if (!IsValid( ))
                return gDatabase.types[ InvalidTypeID ].baseClasses;
            return gDatabase.types[ m_id ].baseClasses;
        }

        ///////////////////////////////////////////////////////////////////////

        const Type::Set &Type::GetDerivedClasses(void) const
        {
            if (!IsValid( ))
                return gDatabase.types[ InvalidTypeID ].derivedClasses;
            return gDatabase.types[ m_id ].derivedClasses;
        }

        ///////////////////////////////////////////////////////////////////////

        std::vector<Constructor> Type::GetConstructors(void) const
        {
            if (!IsValid( ))
                return {};

            auto &handle = gDatabase.types[ m_id ].constructors;

            std::vector<Constructor> constructors;

            for (auto &constructor : handle)
                constructors.emplace_back( constructor.second );

            return constructors;
        }

        ///////////////////////////////////////////////////////////////////////

        std::vector<Constructor> Type::GetDynamicConstructors(void) const
        {
            if (!IsValid( ))
                return {};

            auto &handle = gDatabase.types[ m_id ].dynamicConstructors;

            std::vector<Constructor> constructors;

            for (auto &constructor : handle)
                constructors.emplace_back( constructor.second );

            return constructors;
        }

        ///////////////////////////////////////////////////////////////////////

        const Constructor &Type::GetConstructor(
            const InvokableSignature &signature
        ) const
        {
            if (!IsValid( ))
                return Constructor::Invalid( );
            return gDatabase.types[ m_id ].GetConstructor( signature );
        }

        ///////////////////////////////////////////////////////////////////////

        const Constructor &Type::GetDynamicConstructor(
            const InvokableSignature &signature
        ) const
        {
            if (!IsValid( ))
                return Constructor::Invalid( );
            return gDatabase.types[ m_id ].GetDynamicConstructor( signature );
        }

        ///////////////////////////////////////////////////////////////////////

        const Constructor &Type::GetArrayConstructor(void) const
        {
            if (!IsValid( ))
                return Constructor::Invalid( );
            return gDatabase.types[ m_id ].arrayConstructor;
        }

        ///////////////////////////////////////////////////////////////////////

        const Destructor &Type::GetDestructor(void) const
        {
            if (!IsValid( ))
                return Destructor::Invalid( );
            return gDatabase.types[ m_id ].destructor;
        }

        ///////////////////////////////////////////////////////////////////////

        std::vector<Method> Type::GetMethods(void) const
        {
            if (!IsValid( ))
                return {};

            std::vector<Method> methods;

            auto &handle = gDatabase.types[ m_id ].methods;

            for (auto &overload : handle)
            {
                for (auto &method : overload.second)
                {
                    methods.emplace_back( method.second );
                }
            }

            return methods;
        }

        ///////////////////////////////////////////////////////////////////////

        const Method &Type::GetMethod(const std::string &name) const
        {
            if (!IsValid( ))
                return Method::Invalid( );
            return gDatabase.types[ m_id ].GetMethod( name );
        }

        ///////////////////////////////////////////////////////////////////////

        const Method &Type::GetMethod(
            const std::string &name, 
            const InvokableSignature &signature
        ) const
        {
            if (!IsValid( ))
                return Method::Invalid( );
            return gDatabase.types[ m_id ].GetMethod( name, signature );
        }

        ///////////////////////////////////////////////////////////////////////

        std::vector<Function> Type::GetStaticMethods(void) const
        {
            if (!IsValid( ))
                return {};

            std::vector<Function> methods;

            auto &handle = gDatabase.types[ m_id ].staticMethods;

            for (auto &overload : handle)
            {
                for (auto &method : overload.second)
                {
                    methods.emplace_back( method.second );
                }
            }

            return methods;
        }

        ///////////////////////////////////////////////////////////////////////

        const Function &Type::GetStaticMethod(const std::string &name) const
        {
            if (!IsValid( ))
                return Function::Invalid( );
            return gDatabase.types[ m_id ].GetStaticMethod( name );
        }

        ///////////////////////////////////////////////////////////////////////

        const Function &Type::GetStaticMethod(
            const std::string &name, 
            const InvokableSignature &signature
        ) const
        {
            if (!IsValid( ))
                return Function::Invalid( );
            return gDatabase.types[ m_id ].GetStaticMethod( name, signature );
        }

        ///////////////////////////////////////////////////////////////////////

        const std::vector<Field> &Type::GetFields(void) const
        {
            if (!IsValid( ))
                return gDatabase.types[ InvalidTypeID ].fields;

            // @@@TODO: recursively get base class fields

            return gDatabase.types[ m_id ].fields;
        }

        ///////////////////////////////////////////////////////////////////////

        const Field &Type::GetField(const std::string &name) const
        {
            if (!IsValid( ))
                return Field::Invalid( );
            return gDatabase.types[ m_id ].GetField( name );
        }

        ///////////////////////////////////////////////////////////////////////

        std::vector<Global> Type::GetStaticFields(void) const
        {
            if (!IsValid( ))
                return {};
            return gDatabase.types[ m_id ].staticFields;
        }

        ///////////////////////////////////////////////////////////////////////

        const Global &Type::GetStaticField(const std::string &name) const
        {
            if (!IsValid( ))
                return Global::Invalid( );
            return gDatabase.types[ m_id ].GetStaticField( name );
        }

        ///////////////////////////////////////////////////////////////////////

        Json Type::SerializeJson(const Variant &instance, bool invokeHook) const
        {
            NLS_ASSERT(
                *this == instance.GetType( ),
                "Serializing incompatible variant instance.\n"
                "Got '%s', expected '%s'",
                instance.GetType( ).GetName( ).c_str( ),
                GetName( ).c_str( )
            );

            if (m_isArray)
            {
                Json::array array;

                auto wrapper = instance.GetArray( );
                auto size = wrapper.Size( );

                for (size_t i = 0; i < size; ++i)
                {
                    auto value = wrapper.GetValue( i );

                    array.emplace_back( 
                        value.GetType( ).SerializeJson( value, invokeHook ) 
                    );
                }

                return array;
            }

            if (*this == NLS_TYPEOF( bool ))
            {
                return { instance.ToBool( ) };
            }

            auto &meta = GetMeta( );
            auto isEnum = IsEnum( );

            // number, or non-associative enum
            if (IsPrimitive( ) || meta.GetProperty<SerializeAsNumber>( ))
            {
                if (IsFloatingPoint( ) || !IsSigned( ))
                    return { instance.ToDouble( ) };
 
                return { instance.ToInt( ) };
            }

            // associative enum value
            if (isEnum)
            {
                return GetEnum( ).GetKey( instance );
            }

            if (*this == NLS_TYPEOF( std::string ))
            {
                return { instance.ToString( ) };
            }
            
            Json::object object { };

            auto &fields = gDatabase.types[ m_id ].fields;

            for (auto &field : fields)
            {
                auto value = field.GetValue( instance );

                auto json = value.SerializeJson( );

                value.m_base->OnSerialize( const_cast<Json::object&>( json.object_items( ) ) );

                object[ field.GetName( ) ] = json;
            }

            if (invokeHook)
                instance.m_base->OnSerialize( object );

            return object;
        }

        ///////////////////////////////////////////////////////////////////////

        Json Type::SerializeJson(const Variant &instance, SerializationGetterOverride getterOverride, bool invokeHook) const
        {
            NLS_ASSERT(
                *this == instance.GetType( ),
                "Serializing incompatible variant instance.\n"
                "Got '%s', expected '%s'",
                instance.GetType( ).GetName( ).c_str( ),
                GetName( ).c_str( )
            );

            if (IsArray( ))
            {
                Json::array array;

                auto wrapper = instance.GetArray( );
                auto size = wrapper.Size( );

                for (size_t i = 0; i < size; ++i)
                {
                    auto value = wrapper.GetValue( i );

                    array.emplace_back( 
                        value.GetType( ).SerializeJson( value, invokeHook )
                    );
                }

                return array;
            }

            if (*this == NLS_TYPEOF( bool ))
            {
                return { instance.ToBool( ) };
            }

            auto &meta = GetMeta( );
            auto isEnum = IsEnum( );

            // number, or non-associative enum
            if (IsPrimitive( ) || meta.GetProperty<SerializeAsNumber>( ))
            {
                if (IsFloatingPoint( ) || !IsSigned( ))
                    return { instance.ToDouble( ) };
 
                return { instance.ToInt( ) };
            }

            // associative enum value
            if (isEnum)
            {
                return GetEnum( ).GetKey( instance );
            }

            if (*this == NLS_TYPEOF( std::string ))
            {
                return { instance.ToString( ) };
            }
            
            Json::object object { };

            auto &fields = gDatabase.types[ m_id ].fields;

            for (auto &field : fields)
            {
                auto value = getterOverride( instance, field );

                auto json = value.SerializeJson( );

                value.m_base->OnSerialize( const_cast<Json::object&>( json.object_items( ) ) );

                object[ field.GetName( ) ] = json;
            }

            if (invokeHook)
                instance.m_base->OnSerialize( object );

            return object;
        }

        ///////////////////////////////////////////////////////////////////////

        Variant Type::DeserializeJson(const Json &value) const
        {
            auto &ctor = GetConstructor( );

            NLS_ASSERT( ctor.IsValid( ),
                "Serialization requires a default constructor.\nWith type '%s'.",
                GetName( ).c_str( )
            );

            return DeserializeJson( value, ctor );
        }

        ///////////////////////////////////////////////////////////////////////

        Variant Type::DeserializeJson(const Json &value, const Constructor &ctor) const
        {
            // array types get special care
            if (IsArray( ))
            {
                auto nonArrayType = GetArrayType( );
                auto arrayCtor = GetArrayConstructor( );

                NLS_ASSERT( arrayCtor.IsValid( ),
                    "Type '%s' does not have an array constructor.\n"
                    "Makes sure it is enabled with the meta property 'EnableArrayType'.",
                    nonArrayType.GetName( ).c_str( )
                );

                auto instance = arrayCtor.Invoke( );
                auto wrapper = instance.GetArray( );

                size_t i = 0;

                for (auto &item : value.array_items( ))
                {
                    wrapper.Insert( 
                        i++, 
                        nonArrayType.DeserializeJson( item, ctor ) 
                    );
                }

                return instance;
            }
            // we have to handle all primitive types explicitly
            else if (IsPrimitive( ))
            {
                if (*this == NLS_TYPEOF( int ))
                    return { value.int_value( ) };
                else if (*this == NLS_TYPEOF( unsigned int ))
                    return { static_cast<unsigned int>( value.number_value( ) ) };
                else if (*this == NLS_TYPEOF( bool ))
                    return { value.bool_value( ) };
                else if (*this == NLS_TYPEOF( float ))
                    return { static_cast<float>( value.number_value( ) ) };
                else if (*this == NLS_TYPEOF( double ))
                    return { value.number_value( ) };
            }
            else if (IsEnum( ))
            {
                // number literal
                if (value.is_number( ))
                    return { value.int_value( ) };

                // associative value
                auto enumValue = GetEnum( ).GetValue( value.string_value( ) );

                // make sure we can find the key
                if (enumValue.IsValid( ))
                    return enumValue;
                
                // use the default value as we couldn't find the key
                return TypeCreator::Create( *this );
            }
            else if (*this == NLS_TYPEOF( std::string ))
            {
                return { value.string_value( ) };
            }

            // @@@TODO: forward arguments to constructor
            auto instance = ctor.Invoke( );

            DeserializeJson( instance, value );

            return instance;
        }

        ///////////////////////////////////////////////////////////////////////

    void Type::DeserializeJson(Variant &instance, const Json &value) const
    {
        auto &fields = gDatabase.types[ m_id ].fields;

            for (auto &field : fields)
            {
                auto fieldType = field.GetType( );

                NLS_ASSERT( fieldType.IsValid( ),
                    "Unknown type for field '%s' in base type '%s'. Is this type reflected?",
                    fieldType.GetName( ).c_str( ),
                    GetName( ).c_str( )
                );

                const auto fieldData = value[ field.GetName( ) ];

                if (!fieldData.is_null( ))
                {
                    auto fieldValue = fieldType.DeserializeJson( fieldData );

                    fieldValue.m_base->OnDeserialize( fieldData );

                    field.SetValue( instance, fieldValue );
                }
            }

        instance.m_base->OnDeserialize( value );
    }
}
