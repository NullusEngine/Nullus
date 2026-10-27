#pragma once
#include "CommonDef.h"
#if defined(NLS_SHARED_LIB)
#if defined(NLS_ANIMATION_EXPORT)
#define NLS_ANIMATION_API DLLEXPORT
#else
#define NLS_ANIMATION_API DLLIMPORT
#endif
#else
#define NLS_ANIMATION_API
#endif
