#pragma once

#include "CommonDef.h"

#ifdef NLS_SHARED_LIB
    #ifdef NLS_BASE_EXPORT
        #define NLS_BASE_API DLLEXPORT
    #else
        #define NLS_BASE_API DLLIMPORT
    #endif
#else
    #define NLS_BASE_API
#endif
#include <cstddef>

// use it in "Basic.h"
#ifndef UBPA_UDREFL_INCLUDE_ALL_STD_NAME
// #define UBPA_UDREFL_INCLUDE_ALL_STD_NAME
#endif // UBPA_UDREFL_INCLUDE_ALL_STD_NAME

namespace NLS::UDRefl {
    static constexpr std::size_t MaxArgNum = 64;
    static_assert(MaxArgNum <= 256 - 2);
} // namespace NLS::UDRefl
