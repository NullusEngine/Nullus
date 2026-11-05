#pragma once
#include "CommonDef.h"
#ifdef NLS_SHARED_LIB
#ifdef NLS_PACKAGES_EXPORT
#define NLS_PACKAGES_API DLLEXPORT
#else
#define NLS_PACKAGES_API DLLIMPORT
#endif
#else
#define NLS_PACKAGES_API
#endif
