#pragma once
#include "CommonDef.h"

#ifdef NLS_SHARED_LIB
    #ifdef OGL_EXPORT
        #define OGL_API DLLEXPORT
    #else
        #define OGL_API DLLIMPORT
    #endif
#else
    #define OGL_API
#endif