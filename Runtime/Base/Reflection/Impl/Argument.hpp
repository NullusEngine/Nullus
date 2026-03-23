/* ----------------------------------------------------------------------------
** Copyright (c) 2016 Austin Brunkhorst, All Rights Reserved.
**
** Argument.hpp
** --------------------------------------------------------------------------*/

#pragma once

namespace NLS::meta
{
    template<typename T, typename>
    Argument::Argument(const T &data)
        : m_typeID( NLS_TYPEIDOF( T ) )
        , m_isArray( meta_traits::IsArray<T>::value )
        , m_data( reinterpret_cast<const void*>( std::addressof( data ) ) )
    { }

        ///////////////////////////////////////////////////////////////////////

    template<typename T, typename>
    Argument::Argument(T &data)
        : m_typeID( NLS_TYPEIDOF( T ) )
        , m_isArray( meta_traits::IsArray<T>::value )
        , m_data( reinterpret_cast<const void*>( std::addressof( data ) ) )
    { }

        ///////////////////////////////////////////////////////////////////////

    template<typename T>
    T &Argument::GetValue(void) const
    {
        return *reinterpret_cast<
            typename std::remove_reference< T >::type*
        >(
            const_cast<void *>( m_data )
        );
    }
}
