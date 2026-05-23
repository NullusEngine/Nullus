/* ----------------------------------------------------------------------------
** Copyright (c) 2016 Austin Brunkhorst, All Rights Reserved.
**
** ArrayVariantContainer.hpp
** --------------------------------------------------------------------------*/

#pragma once

#include "../ArrayWrapper.h"

namespace NLS::meta
{
    template<typename T, typename ContainerType, typename StorageType>
    ArrayVariantContainer<T, ContainerType, StorageType>::ArrayVariantContainer(StorageType &rhs)
        : m_array( rhs )
    {
    }

        template<typename T, typename ContainerType, typename StorageType>
        Type ArrayVariantContainer<T, ContainerType, StorageType>::GetType(void) const
        {
            return NLS_TYPEOF( ContainerType );
        }

        template<typename T, typename ContainerType, typename StorageType>
        void *ArrayVariantContainer<T, ContainerType, StorageType>::GetPtr(void) const
        {
            return const_cast<void*>(
                reinterpret_cast<const void*>( 
                    std::addressof( m_array )
                )
            );
        }

        template<typename T, typename ContainerType, typename StorageType>
        int ArrayVariantContainer<T, ContainerType, StorageType>::ToInt(void) const
        {
            return int( );
        }

        template<typename T, typename ContainerType, typename StorageType>
        bool ArrayVariantContainer<T, ContainerType, StorageType>::ToBool(void) const
        {
            return bool( );
        }

        template<typename T, typename ContainerType, typename StorageType>
        float ArrayVariantContainer<T, ContainerType, StorageType>::ToFloat(void) const
        {
            return float( );
        }

        template<typename T, typename ContainerType, typename StorageType>
        double ArrayVariantContainer<T, ContainerType, StorageType>::ToDouble(void) const
        {
            return double( );
        }

        template<typename T, typename ContainerType, typename StorageType>
        std::string ArrayVariantContainer<T, ContainerType, StorageType>::ToString(void) const
        {
            return std::string( );
        }

        template<typename T, typename ContainerType, typename StorageType>
        bool ArrayVariantContainer<T, ContainerType, StorageType>::IsArray(void) const
        {
            return true;
        }

        template<typename T, typename ContainerType, typename StorageType>
        ArrayWrapper ArrayVariantContainer<T, ContainerType, StorageType>::GetArray(void) const
        {
            if constexpr (std::is_const_v<std::remove_reference_t<StorageType>>)
                return ArrayWrapper( static_cast<const ContainerType&>( m_array ) );
            else
                return ArrayWrapper( const_cast<ContainerType&>( m_array ) );
        }

        template<typename T, typename ContainerType, typename StorageType>
        VariantBase *ArrayVariantContainer<T, ContainerType, StorageType>::Clone(void) const
        {
            return new ArrayVariantContainer<T, ContainerType, StorageType>( const_cast<StorageType&>( m_array ) );
        }
}
