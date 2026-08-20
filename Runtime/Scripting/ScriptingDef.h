#pragma once

#include "CommonDef.h"

#ifdef NLS_SHARED_LIB
    #ifdef NLS_SCRIPTING_EXPORT
        #define NLS_SCRIPTING_API DLLEXPORT
    #else
        #define NLS_SCRIPTING_API DLLIMPORT
    #endif
#else
    #define NLS_SCRIPTING_API
#endif
