#pragma once
#include <cstdint>
#include "CommonDef.h"

#ifdef NLS_SHARED_LIB
    #ifdef NLS_CORE_EXPORT
        #define NLS_CORE_API DLLEXPORT
    #else
        #define NLS_CORE_API DLLIMPORT
    #endif
#else
    #define NLS_CORE_API
#endif

#ifdef NLS_SHARED_LIB
    #ifdef NLS_RESOURCE_MANAGEMENT_EXPORT
        #define NLS_RESOURCE_MANAGEMENT_API DLLEXPORT
    #else
        #define NLS_RESOURCE_MANAGEMENT_API DLLIMPORT
    #endif
#else
    #define NLS_RESOURCE_MANAGEMENT_API
#endif
