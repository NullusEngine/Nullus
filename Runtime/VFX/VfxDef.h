#pragma once
#include "CommonDef.h"

#if defined(NLS_SHARED_LIB)
#if defined(NLS_VFX_EXPORT)
#define NLS_VFX_API DLLEXPORT
#else
#define NLS_VFX_API DLLIMPORT
#endif
#else
#define NLS_VFX_API
#endif
