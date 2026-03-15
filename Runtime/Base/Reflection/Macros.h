/* ----------------------------------------------------------------------------
** Copyright (c) 2016 Austin Brunkhorst, All Rights Reserved.
**
** Macros.h
** --------------------------------------------------------------------------*/

#pragma once

#if defined(__REFLECTION_PARSER__)

    #define META(...) __attribute__((annotate(#__VA_ARGS__)))
    #define CLASS(...) META(Reflection)
    #define STRUCT(...) META(Reflection)

    // 兼容旧写法（过渡期）

    #define __META_EXTERNAL(type, guid)       \
        typedef type __META_EXTERNAL__##guid; \

    #define _META_EXTERNAL(type, guid) __META_EXTERNAL(type, guid)

    #define MetaExternal(type) _META_EXTERNAL(type, __COUNTER__)

    #define REFLECT_EXTERNAL(...)
    #define REFLECT_FIELD(...)
    #define REFLECT_METHOD(...)
    #define REFLECT_METHOD_EX(...)
    #define REFLECT_STATIC_METHOD(...)
    #define REFLECT_PRIVATE_FIELD(...)
    #define REFLECT_PRIVATE_METHOD(...)
    #define REFLECT_PRIVATE_METHOD_EX(...)
    #define REFLECT_PRIVATE_STATIC_METHOD(...)
    #define NLS_BODY_MACRO_COMBINE_INNER(A, B, C, D) A##B##C##D
    #define NLS_BODY_MACRO_COMBINE(A, B, C, D) NLS_BODY_MACRO_COMBINE_INNER(A, B, C, D)
    #ifndef CURRENT_FILE_ID
        #define CURRENT_FILE_ID NLS_UNKNOWN_FILE_ID
    #endif
    #define GENERATED_BODY(...)

    #define META_OBJECT

#else 

    #define META(...)
    #define CLASS(...)
    #define STRUCT(...)

    // 兼容旧写法（过渡期）

    #define MetaExternal(type)
    #define REFLECT_EXTERNAL(...)
    #define REFLECT_FIELD(...)
    #define REFLECT_METHOD(...)
    #define REFLECT_METHOD_EX(...)
    #define REFLECT_STATIC_METHOD(...)
    #define REFLECT_PRIVATE_FIELD(...)
    #define REFLECT_PRIVATE_METHOD(...)
    #define REFLECT_PRIVATE_METHOD_EX(...)
    #define REFLECT_PRIVATE_STATIC_METHOD(...)
    #define NLS_BODY_MACRO_COMBINE_INNER(A, B, C, D) A##B##C##D
    #define NLS_BODY_MACRO_COMBINE(A, B, C, D) NLS_BODY_MACRO_COMBINE_INNER(A, B, C, D)
    #ifndef CURRENT_FILE_ID
        #define CURRENT_FILE_ID NLS_UNKNOWN_FILE_ID
    #endif
    #define GENERATED_BODY(...) NLS_BODY_MACRO_COMBINE(CURRENT_FILE_ID, _, __LINE__, _GENERATED_BODY)

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
