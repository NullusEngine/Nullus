#pragma once
#include <cstdint>
#include "CommonDef.h"

#ifdef NLS_SHARED_LIB
    #ifdef NLS_BUILD_EXPORT
        #define NLS_BUILD_API DLLEXPORT
    #else
        #define NLS_BUILD_API DLLIMPORT
    #endif
#else
    #define NLS_BUILD_API
#endif
