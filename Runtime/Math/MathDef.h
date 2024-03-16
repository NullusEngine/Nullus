#pragma once
#include "CommonDef.h"

#ifdef NLS_SHARED_LIB
    #ifdef NLS_MATH_EXPORT
        #define NLS_MATH_API DLLEXPORT
    #else
        #define NLS_MATH_API DLLIMPORT
    #endif
#else
    #define NLS_MATH_API
#endif