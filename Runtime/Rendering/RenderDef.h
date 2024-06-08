#pragma once
#include "CommonDef.h"

#ifdef NLS_SHARED_LIB
    #ifdef NLS_RENDER_EXPORT
        #define NLS_RENDER_API DLLEXPORT
    #else
        #define NLS_RENDER_API DLLIMPORT
    #endif
#else
    #define NLS_RENDER_API
#endif