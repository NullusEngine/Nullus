#pragma once
#include "CommonDef.h"
#if defined(NLS_SHARED_LIB)
#if defined(NLS_PHYSICS_EXPORT)
#define NLS_PHYSICS_API DLLEXPORT
#else
#define NLS_PHYSICS_API DLLIMPORT
#endif
#else
#define NLS_PHYSICS_API
#endif
