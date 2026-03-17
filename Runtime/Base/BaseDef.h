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
