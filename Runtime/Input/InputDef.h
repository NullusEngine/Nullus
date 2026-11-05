#pragma once
#include "CommonDef.h"

#ifndef NLS_INPUT_API
#if defined(NLS_SHARED_LIB)
#if defined(NLS_INPUT_EXPORT)
#define NLS_INPUT_API DLLEXPORT
#else
#define NLS_INPUT_API DLLIMPORT
#endif
#else
#define NLS_INPUT_API
#endif
#endif
