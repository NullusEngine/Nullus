#pragma once
#include "EngineDef.h"
#include <Core/Assets/SpriteSortingLayers.h>
#include <memory>

namespace NLS::Engine::Assets
{
NLS_ENGINE_API std::shared_ptr<const Core::Assets::SpriteSortingLayers> CaptureSpriteSortingLayers(std::string* error=nullptr);
NLS_ENGINE_API void InvalidateSpriteSortingLayers();
}
