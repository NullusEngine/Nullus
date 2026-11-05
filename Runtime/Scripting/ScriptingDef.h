#pragma once

#include "CommonDef.h"

#if defined(NLS_SHARED_LIB) && !defined(NLS_SCRIPTING_STATIC_DEFINE)
    #ifdef NLS_SCRIPTING_EXPORT
        #define NLS_SCRIPTING_API DLLEXPORT
    #else
        #define NLS_SCRIPTING_API DLLIMPORT
    #endif
#else
    #define NLS_SCRIPTING_API
#endif
