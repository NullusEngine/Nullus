/* ----------------------------------------------------------------------------
** Copyright (c) 2016 Austin Brunkhorst, All Rights Reserved.
**
** Macros.h
** --------------------------------------------------------------------------*/

#pragma once

#if defined(__REFLECTION_PARSER__)

    #define META(...) __attribute__((annotate(#__VA_ARGS__)))
    #define CLASS(class_name, ...) class __attribute__((annotate(#__VA_ARGS__))) class_name
    #define STRUCT(struct_name, ...) struct __attribute__((annotate(#__VA_ARGS__))) struct_name

    // 兼容旧写法（过渡期）
    #define Meta(...) META(__VA_ARGS__)

    #define __META_EXTERNAL(type, guid)       \
        typedef type __META_EXTERNAL__##guid; \

    #define _META_EXTERNAL(type, guid) __META_EXTERNAL(type, guid)

    #define MetaExternal(type) _META_EXTERNAL(type, __COUNTER__)

    #define META_OBJECT

#else 

    #define META(...)
    #define CLASS(class_name, ...) class class_name
    #define STRUCT(struct_name, ...) struct struct_name

    // 兼容旧写法（过渡期）
    #define Meta(...) META(__VA_ARGS__)

    #define MetaExternal(type)

    #define MetaInitialize(initializer)                               \
        {                                                             \
            auto &db = NLS::meta::ReflectionDatabase::Instance( ); \
            initializer;                                              \
        }                                                             \

    // Used in objects to preserve virtual inheritance functionality
    #define META_OBJECT                                  \
        NLS::meta::Type GetType(void) const override  \
        {                                                \
            return typeof( decltype( *this ) );          \
        }                                                \
        NLS::meta::Object *Clone(void) const override \
        {                                                \
            typedef                                      \
            std::remove_const<                           \
                std::remove_reference<                   \
                    decltype( *this )                    \
                >::type                                  \
            >::type ClassType;                           \
            return new ClassType( *this );               \
        }                                                \

#endif