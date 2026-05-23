/* ----------------------------------------------------------------------------
** Copyright (c) 2016 Austin Brunkhorst, All Rights Reserved.
**
** Macros.h
** --------------------------------------------------------------------------*/

#pragma once

#if defined(__REFLECTION_PARSER__)

    #define META(...) __attribute__((annotate(#__VA_ARGS__)))
    #define META_TEXT(text) __attribute__((annotate(text)))
    #define CLASS(typeDecl, ...) class META(Reflection __VA_OPT__(,) __VA_ARGS__) typeDecl
    #define STRUCT(typeDecl, ...) struct META(Reflection __VA_OPT__(,) __VA_ARGS__) typeDecl
    #define ENUM(typeDecl, ...) enum class META(Reflection __VA_OPT__(,) __VA_ARGS__) typeDecl
    #define PROPERTY(...) META_TEXT("Property" __VA_OPT__(", " #__VA_ARGS__))
    #define FUNCTION(...) META_TEXT("Function" __VA_OPT__(", " #__VA_ARGS__))

    #define __META_JOIN_INNER(left, right) left##right
    #define __META_JOIN(left, right) __META_JOIN_INNER(left, right)

    #define NLS_BODY_MACRO_COMBINE_INNER(A, B, C, D) A##B##C##D
    #define NLS_BODY_MACRO_COMBINE(A, B, C, D) NLS_BODY_MACRO_COMBINE_INNER(A, B, C, D)
    #ifndef CURRENT_FILE_ID
        #define CURRENT_FILE_ID NLS_UNKNOWN_FILE_ID
    #endif
    #define GENERATED_BODY(...)

    #define META_OBJECT

#else

    #define META(...)
    #define CLASS(typeDecl, ...) class typeDecl
    #define STRUCT(typeDecl, ...) struct typeDecl
    #define ENUM(typeDecl, ...) enum class typeDecl
    #define PROPERTY(...)
    #define FUNCTION(...)

    #define NLS_BODY_MACRO_COMBINE_INNER(A, B, C, D) A##B##C##D
    #define NLS_BODY_MACRO_COMBINE(A, B, C, D) NLS_BODY_MACRO_COMBINE_INNER(A, B, C, D)
    #ifndef CURRENT_FILE_ID
        #define CURRENT_FILE_ID NLS_UNKNOWN_FILE_ID
    #endif
    #define GENERATED_BODY(...) NLS_BODY_MACRO_COMBINE(CURRENT_FILE_ID, _, __LINE__, _GENERATED_BODY)

    #define MetaInitialize(initializer)                          \
        {                                                        \
            auto &db = NLS::meta::ReflectionDatabase::Instance(); \
            initializer;                                         \
        }                                                        \

    #define META_OBJECT                                      \
        const char* GetObjectTypeName(void) const override   \
        {                                                    \
            return StaticMetaTypeName();                     \
        }                                                    \

#endif
