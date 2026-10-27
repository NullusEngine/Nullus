#pragma once

#include "CoreDef.h"

// Engine version string shared by the editor, the runtime, and the packaging
// tools. The top-level CMake project version is injected here so a single
// place changes the product identity.
#ifndef NLS_ENGINE_VERSION_STRING
#define NLS_ENGINE_VERSION_STRING "0.0.0-dev"
#endif

namespace NLS::Core
{
inline constexpr const char* kEngineVersionString = NLS_ENGINE_VERSION_STRING;
}
