#pragma once
#include "CommonDef.h"
#ifdef NLS_SHARED_LIB
#ifdef NLS_PLUGINS_EXPORT
#define NLS_PLUGINS_API DLLEXPORT
#else
#define NLS_PLUGINS_API DLLIMPORT
#endif
#else
#define NLS_PLUGINS_API
#endif
