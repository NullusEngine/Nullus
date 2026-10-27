#pragma once
#include "CommonDef.h"
#if defined(NLS_SHARED_LIB)
#if defined(NLS_ANIMATIONCOMPILER_EXPORT)
#define NLS_ANIMATION_COMPILER_API DLLEXPORT
#else
#define NLS_ANIMATION_COMPILER_API DLLIMPORT
#endif
#else
#define NLS_ANIMATION_COMPILER_API
#endif
