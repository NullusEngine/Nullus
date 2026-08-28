#pragma once

#include "Rendering/UI/UiDrawDataSnapshot.h"
#include "Rendering/UI/UIDrawSnapshot.h"

#include <memory>

namespace NLS::Render::UI
{
    // Converts the retained-mode snapshot into the immutable snapshot already
    // consumed by the cross-backend UI overlay renderer. The source snapshot
    // remains untouched and can be kept by the game thread for diagnostics.
    NLS_RENDER_API std::shared_ptr<UiDrawDataSnapshot> AdaptUIDrawSnapshot(
        const UIDrawSnapshot& snapshot);

    NLS_RENDER_API std::shared_ptr<const UiDrawDataSnapshot> AdaptUIDrawSnapshot(
        const std::shared_ptr<const UIDrawSnapshot>& snapshot);
}
