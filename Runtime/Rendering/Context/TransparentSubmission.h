#pragma once
#include "Rendering/Context/RenderScenePackage.h"

namespace NLS::Render::Context
{
// Combine adjacent transparent passes which address the same attachments.
// Retains resource dependencies and compute preparation before the sorted draws.
NLS_RENDER_API void FinalizeTransparentSubmission(RenderScenePackage& package);
}
