#include "CookTypes.h"

namespace NLS::Cook
{
NLS::Guid MakeEngineAssetGuid(const std::string_view virtualPath)
{
    return NLS::Guid::NewDeterministic(
        "nullus-engine-asset:" + std::string(virtualPath));
}

bool IsEngineVirtualAssetPath(const std::string_view virtualPath)
{
    return virtualPath.starts_with("Assets/Engine/");
}
} // namespace NLS::Cook
