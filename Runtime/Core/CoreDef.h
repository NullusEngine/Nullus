#pragma once
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