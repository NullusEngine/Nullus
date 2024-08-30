#pragma once

#include "CommonDef.h"

#ifdef NLS_SHARED_LIB
    #ifdef NLS_UI_EXPORT
        #define NLS_UI_API DLLEXPORT
    #else
        #define NLS_UI_API DLLIMPORT
    #endif
#else
    #define NLS_UI_API
#endif