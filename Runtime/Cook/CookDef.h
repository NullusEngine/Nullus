#pragma once
#include <cstdint>
#include "CommonDef.h"

#ifdef NLS_SHARED_LIB
    #ifdef NLS_COOK_EXPORT
        #define NLS_COOK_API DLLEXPORT
    #else
        #define NLS_COOK_API DLLIMPORT
    #endif
#else
    #define NLS_COOK_API
#endif
