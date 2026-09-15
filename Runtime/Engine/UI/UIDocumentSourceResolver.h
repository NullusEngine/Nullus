#pragma once

#include <functional>
#include <string>

#include "EngineDef.h"
#include "Engine/UI/UIDocumentReference.h"

namespace NLS::Engine::UI
{
// Hosts install a resolver so a reference stored as a project-relative
// "Assets/..." path can be opened without the runtime UI layer knowing about
// project layout. Returning an empty string keeps the reference unresolved.
using UIDocumentSourcePathResolver = std::function<std::string(const UIDocumentReference&)>;

NLS_ENGINE_API void SetUIDocumentSourcePathResolver(UIDocumentSourcePathResolver resolver);

// Returns the source file to open for the reference: the stored path when it
// already addresses an existing file, otherwise the registered resolver result.
// An empty result means the document cannot be loaded yet; callers keep the
// stable reference so a later reload can still succeed.
[[nodiscard]] NLS_ENGINE_API std::string ResolveUIDocumentSourcePath(
    const UIDocumentReference& reference);

// Shared host helper that maps "Assets/..." and Assets-relative references onto
// the configured project assets folder and rejects paths escaping that folder.
[[nodiscard]] NLS_ENGINE_API std::string ResolveProjectUIDocumentPath(
    const std::string& projectAssetsPath,
    const std::string& resourcePath);
}
