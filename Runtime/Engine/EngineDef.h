#pragma once
#include"CommonDef.h"

#ifdef NLS_SHARED_LIB
#ifdef NLS_ENGINE_EXPORT
#define NLS_ENGINE_API DLLEXPORT
#else
#define NLS_ENGINE_API DLLIMPORT
#endif
#else
#define NLS_ENGINE_API
#endif