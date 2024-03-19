#pragma once
#include "CommonDef.h"

#ifdef NLS_SHARED_LIB
    #ifdef NLS_VULKAN_EXPORT
        #define VULKAN_API DLLEXPORT
    #else
        #define VULKAN_API DLLIMPORT
    #endif
#else
    #define VULKAN_API
#endif
