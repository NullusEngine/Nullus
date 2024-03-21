#pragma once

// Initial compiler-related stuff to set.
#define NLS_COMPILER_MSVC  1
#define NLS_COMPILER_CLANG 2
#define NLS_COMPILER_GNUC  3

// Initial platform stuff to set.
#define NLS_PLATFORM_WINDOWS 1
#define NLS_PLATFORM_LINUX   2
#define NLS_PLATFORM_MAC_OSX 3
#define NLS_PLATFORM_MAC_IOS 4
#define NLS_PLATFORM_ANDROID 5
#define NLS_PLATFORM_NACL    6
#define NLS_PLATFORM_ORBIS   7
#define NLS_PLATFORM_PS4     8

// Mode
#define NLS_MODE_DEBUG   1
#define NLS_MODE_RELEASE 2


// Compiler type and version recognition
#if defined(_MSC_VER)
    #define NLS_COMPILER NLS_COMPILER_MSVC
    #if _MSC_VER >= 1700
        #define NLS_COMPILER_VERSION 110
    #elif _MSC_VER >= 1600
        #define NLS_COMPILER_VERSION 100
    #elif _MSC_VER >= 1500
        #define NLS_COMPILER_VERSION 90
    #elif _MSC_VER >= 1400
        #define NLS_COMPILER_VERSION 80
    #elif _MSC_VER >= 1300
        #define NLS_COMPILER_VERSION 70
    #endif
#elif defined(__clang__)
    #define NLS_COMPILER         NLS_COMPILER_CLANG
    #define NLS_COMPILER_VERSION (((__clang_major__)*100) + (__clang_minor__ * 10) + __clang_patchlevel__)
    #if __cplusplus >= 202002L
        #define _HAS_CXX20 1
    #elif __cplusplus >= 201703L
        #define _HAS_CXX17 1
    #elif __cplusplus >= 201402L
        #define _HAS_CXX14 1
    #elif __cplusplus >= 199711L
        #define _HAS_CXX11 1
    #endif
#elif defined(__GNUC__)
    #define NLS_COMPILER         NLS_COMPILER_GNUC
    #define NLS_COMPILER_VERSION (((__GNUC__)*100) + (__GNUC_MINOR__ * 10) + __GNUC_PATCHLEVEL__)
    #if __cplusplus >= 202002L
        #define _HAS_CXX20 1
    #elif __cplusplus >= 201703L
        #define _HAS_CXX17 1
    #elif __cplusplus >= 201402L
        #define _HAS_CXX14 1
    #elif __cplusplus >= 199711L
        #define _HAS_CXX11 1
    #endif
#else
    #error "Unknown compiler. Abort! Abort!"
#endif

// Platform recognition
#if defined(_WIN32) || defined(__WIN32__) || defined(WIN32)
    #define NLS_PLATFORM NLS_PLATFORM_WINDOWS
#elif defined(_WIN64) || defined(__WIN64__) || defined(WIN64)
    #define NLS_PLATFORM NLS_PLATFORM_WINDOWS
#elif defined(__APPLE_CC__)
    // Device                                                     Simulator
    // Both requiring OS version 4.0 or greater
    #if __ENVIRONMENT_IPHONE_OS_VERSION_MIN_REQUIRED__ >= 40000 || __IPHONE_OS_VERSION_MIN_REQUIRED >= 40000
        #define NLS_PLATFORM NLS_PLATFORM_MAC_IOS
        #include "unistd.h"
    #else
        #define NLS_PLATFORM NLS_PLATFORM_MAC_OSX
    #endif
#elif defined(__ANDROID__)
    #define NLS_PLATFORM NLS_PLATFORM_ANDROID
    #include <unistd.h>
#elif defined(linux) || defined(__linux) || defined(__linux__)
    #define NLS_PLATFORM NLS_PLATFORM_LINUX
    #include <unistd.h>
    #include <cstdio>
    #include <cstring>
#elif defined(__native_client__)
    #define NLS_PLATFORM NLS_PLATFORM_NACL
#elif defined(ORBIS)
    #define NLS_PLATFORM     NLS_PLATFORM_ORBIS
    #define PS4_DEBUG_MARKER 1
#else
    #error "Couldn't recognize platform"
#endif

#if (NLS_PLATFORM == NLS_PLATFORM_WINDOWS)
    #define DLLEXPORT __declspec(dllexport)
    #define DLLIMPORT __declspec(dllimport)
#elif (NLS_PLATFORM == NLS_PLATFORM_ANDROID) || (NLS_PLATFORM == NLS_PLATFORM_MAC_IOS) || (NLS_PLATFORM == NLS_PLATFORM_LINUX)
    #define DLLEXPORT __attribute__((visibility("default")))
    #define DLLIMPORT __attribute__((visibility("default")))
#else
    #define DLLEXPORT
    #define DLLIMPORT
#endif

#ifndef FORCEINLINE
    #if (_MSC_VER >= 1200)
        #define FORCEINLINE __forceinline
    #else
        #define FORCEINLINE __inline
    #endif
#endif

#if defined(_DEBUG) || defined(DEBUG) || !defined(NDEBUG) || defined(NLS_DEBUG)
    #define NLS_MODE NLS_MODE_DEBUG
#else
    #define NLS_MODE NLS_MODE_RELEASE
#endif

#define NLS_UNUSED(a) (void)a

#if defined(_MSC_VER)
    #define MS_ALIGN(n) __declspec(align(n))
#else
    #ifndef GCC_ALIGN
        #define GCC_ALIGN(n) __attribute__((aligned(n)))
    #endif
#endif

#if (NLS_COMPILER == NLS_COMPILER_MSVC)
    #define _CRT_NON_CONFORMING_SWPRINTFS
    #pragma warning(disable : 4091)
    #pragma warning(disable : 4100)
    #pragma warning(disable : 4201)
    #pragma warning(disable : 4244)
    #pragma warning(disable : 4251)
    #pragma warning(disable : 4458)
    #pragma warning(disable : 6385)
    #pragma warning(disable : 6313) // rapidxml.hpp warning C6313: 运算符不正确: 不能使用按位与来测试零值标志。
    #pragma warning(disable : 4127) // conditional expression is constant
    #pragma warning(disable : 6011)
#endif

#include "NullusTraits.h"
