/* ----------------------------------------------------------------------------
** Copyright (c) 2016 Austin Brunkhorst, All Rights Reserved.
**
** Variant.hpp
** --------------------------------------------------------------------------*/

#pragma once

#include "../VariantContainer.h"
#include "../ObjectWrapper.h"
#include "../ArrayVariantContainer.h"
#include "../Type.h"

namespace NLS::meta
{
    template <typename T>
    Variant::Variant(
        T *data,
        variant_policy::WrapObject,
        typename std::enable_if<
            std::is_base_of<NLS::Object, T>::value
        >::type*
    )
        : m_isConst( std::is_const<T>::value )
        , m_base(nullptr)
    {
        using CleanObjectType = typename std::remove_const<T>::type;
        auto* object = static_cast<NLS::Object*>(
            const_cast<CleanObjectType*>(data));
        Type objectType = object != nullptr ? object->GetType() : Type();
        if (!objectType)
            objectType = NLS_TYPEOF(CleanObjectType);
        m_base = new ::NLS::meta::ObjectWrapper(object, objectType);
    }

        ///////////////////////////////////////////////////////////////////////

        template<typename T>
        Variant::Variant(
            T &data,
            DISABLE_VARIANT
        )
            : m_isConst( std::is_pointer<T>::value && std::is_const<T>::value )
            , m_base( new VariantContainer< CleanedType<T> >( data ) )
        {
        
        }

        ///////////////////////////////////////////////////////////////////////

        template<typename T>
        Variant::Variant(
            T &data,
            variant_policy::NoCopy,
            DISABLE_VARIANT
        )
            : m_isConst( std::is_pointer<T>::value && std::is_const<T>::value )
            , m_base( new VariantContainer< CleanedType<T>& >( data ) )
        {

        }

        ///////////////////////////////////////////////////////////////////////

        template<typename T>
        Variant::Variant(Array<T> &rhs, variant_policy::NoCopy)
            : m_isConst( false )
            , m_base( new ArrayVariantContainer<T, Array<T>, meta_traits::ArrayByReference<T>>( rhs ) )
        {

        }

        template<typename T, typename Allocator>
        Variant::Variant(std::vector<T, Allocator> &rhs, variant_policy::NoCopy)
            : m_isConst( false )
            , m_base( new ArrayVariantContainer<T, std::vector<T, Allocator>, std::add_lvalue_reference_t<std::vector<T, Allocator>>>( rhs ) )
        {

        }

        ///////////////////////////////////////////////////////////////////////

        template<typename T>
        Variant::Variant(
            T &&data,
            DISABLE_VARIANT,
            DISABLE_ARGUMENT,
            DISABLE_CONST
        )
            : m_isConst( false )
            , m_base( 
                new VariantContainer< CleanedType<T> >( 
                    static_cast<T&&>( data ) 
                )
            )
        {

        }

        ///////////////////////////////////////////////////////////////////////

        template<typename T>
        Variant::Variant(Array<T> &rhs)
            : m_isConst( false )
            , m_base( new ArrayVariantContainer<T, Array<T>, meta_traits::ArrayByReference<T>>( rhs ) )
        {

        }

        ///////////////////////////////////////////////////////////////////////

        template<typename T>
        Variant::Variant(const Array<T> &rhs)
            : m_isConst( true )
            , m_base( new ArrayVariantContainer<T, Array<T>, std::add_lvalue_reference_t<const Array<T>>>( rhs ) )
        {

        }

        ///////////////////////////////////////////////////////////////////////

        template<typename T>
        Variant::Variant(Array<T> &&rhs)
            : m_isConst( false )
            , m_base( new ArrayVariantContainer<T, Array<T>, meta_traits::ArrayByValue<T>>( rhs ) )
        {

        }

        ///////////////////////////////////////////////////////////////////////

        template<typename T>
        Variant::Variant(const Array<T> &&rhs)
            : m_isConst( true )
            , m_base( new ArrayVariantContainer<T, Array<T>, const meta_traits::ArrayByValue<T>>( rhs ) )
        {

        }

        ///////////////////////////////////////////////////////////////////////

        template<typename T, typename Allocator>
        Variant::Variant(std::vector<T, Allocator> &rhs)
            : m_isConst( false )
            , m_base( new ArrayVariantContainer<T, std::vector<T, Allocator>, std::add_lvalue_reference_t<std::vector<T, Allocator>>>( rhs ) )
        {

        }

        ///////////////////////////////////////////////////////////////////////

        template<typename T, typename Allocator>
        Variant::Variant(const std::vector<T, Allocator> &rhs)
            : m_isConst( true )
            , m_base( new ArrayVariantContainer<T, std::vector<T, Allocator>, std::add_lvalue_reference_t<const std::vector<T, Allocator>>>( rhs ) )
        {

        }

        ///////////////////////////////////////////////////////////////////////

        template<typename T, typename Allocator>
        Variant::Variant(std::vector<T, Allocator> &&rhs)
            : m_isConst( false )
            , m_base( new ArrayVariantContainer<T, std::vector<T, Allocator>, std::vector<T, Allocator>>( rhs ) )
        {

        }

        ///////////////////////////////////////////////////////////////////////

        template<typename T, typename Allocator>
        Variant::Variant(const std::vector<T, Allocator> &&rhs)
            : m_isConst( true )
            , m_base( new ArrayVariantContainer<T, std::vector<T, Allocator>, const std::vector<T, Allocator>>( rhs ) )
        {

        }

        ///////////////////////////////////////////////////////////////////////

        template<typename T>
        Variant &Variant::operator=(T &&rhs)
        {
            Variant( static_cast<T&&>( rhs ) ).Swap( *this );

            return *this;
        }

        ///////////////////////////////////////////////////////////////////////

        template<typename T>
        T &Variant::GetValue(void) const
        {
            if constexpr (std::is_pointer<T>::value)
            {
                using PointedType = typename std::remove_pointer<T>::type;
                if constexpr (std::is_base_of<NLS::Object, typename std::remove_cv<PointedType>::type>::value)
                {
                    if (auto* objectWrapper = dynamic_cast<ObjectWrapper*>(m_base))
                        return objectWrapper->GetObjectPointerAs<T>();
                }
            }

            return *static_cast<T*>( getPtr( ) );
        }
}
