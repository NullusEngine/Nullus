#pragma once
#include"CommonDef.h"

#ifdef NLS_SHARED_LIB
#ifdef NLS_PLATFORM_EXPORT
#define NLS_PLATFORM_API DLLEXPORT
#else
#define NLS_PLATFORM_API DLLIMPORT
#endif
#else
#define NLS_PLATFORM_API
#endif