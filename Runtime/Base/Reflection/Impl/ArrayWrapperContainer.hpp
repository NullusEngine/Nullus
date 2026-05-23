/* ----------------------------------------------------------------------------
** Copyright (c) 2016 Austin Brunkhorst, All Rights Reserved.
**
** ArrayWrapperContainer.hpp
** --------------------------------------------------------------------------*/

#pragma once

#include "../Argument.h"
#include "../Variant.h"

namespace NLS::meta
{
    template<typename T, typename ContainerType>
    ArrayWrapperContainer<T, ContainerType>::ArrayWrapperContainer(ContainerType &a)
        : m_array( a )
    {
    }

        template<typename T, typename ContainerType>
        Variant ArrayWrapperContainer<T, ContainerType>::GetValue(size_t index)
        {
            return { m_array[ index ] };
        }

        template<typename T, typename ContainerType>
        void ArrayWrapperContainer<T, ContainerType>::SetValue(size_t index, const Argument &value)
        {
            if constexpr (std::is_assignable_v<T&, T>)
                m_array.at( index ) = value.GetValue<T>( );
        }

        template<typename T, typename ContainerType>
        void ArrayWrapperContainer<T, ContainerType>::Insert(size_t index, const Argument &value)
        {
            if constexpr (std::is_copy_constructible_v<T>)
                m_array.insert( m_array.begin( ) + index, value.GetValue<T>( ) );
        }

        template<typename T, typename ContainerType>
        void ArrayWrapperContainer<T, ContainerType>::InsertDefault(size_t index)
        {
            if constexpr (std::is_default_constructible_v<T> && std::is_copy_constructible_v<T>)
                m_array.insert( m_array.begin( ) + index, T {} );
        }

        template<typename T, typename ContainerType>
        void ArrayWrapperContainer<T, ContainerType>::Remove(size_t index)
        {
            m_array.erase( m_array.begin( ) + index );
        }

        template<typename T, typename ContainerType>
        void ArrayWrapperContainer<T, ContainerType>::Resize(size_t size)
        {
            if constexpr (std::is_default_constructible_v<T>)
                m_array.resize( size );
        }

        template<typename T, typename ContainerType>
        size_t ArrayWrapperContainer<T, ContainerType>::Size(void) const
        {
            return m_array.size( );
        }

        template<typename T, typename ContainerType>
        bool ArrayWrapperContainer<T, ContainerType>::CanSetValue(void) const
        {
            return std::is_assignable_v<T&, T>;
        }

        template<typename T, typename ContainerType>
        bool ArrayWrapperContainer<T, ContainerType>::CanInsert(void) const
        {
            return std::is_copy_constructible_v<T>;
        }

        template<typename T, typename ContainerType>
        bool ArrayWrapperContainer<T, ContainerType>::CanInsertDefault(void) const
        {
            return std::is_default_constructible_v<T> && std::is_copy_constructible_v<T>;
        }

        template<typename T, typename ContainerType>
        bool ArrayWrapperContainer<T, ContainerType>::CanRemove(void) const
        {
            return true;
        }

        template<typename T, typename ContainerType>
        bool ArrayWrapperContainer<T, ContainerType>::CanResize(void) const
        {
            return std::is_default_constructible_v<T>;
        }
}
