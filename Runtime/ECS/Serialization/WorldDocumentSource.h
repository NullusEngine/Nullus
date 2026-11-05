#pragma once
#include "WorldArchive.h"

namespace NLS::ECS
{
// Independent, read-only input for background parsing. Implementations must
// enforce limits and support concurrent reads. Retain only immutable input /
// content-source ownership; never retain a World, GameObject or VM callback.
class NLS_ECS_API IWorldDocumentSource
{
public:
    virtual ~IWorldDocumentSource() = default;
    virtual WorldDocument Read(ArchiveLimits limits) const = 0;
};
}
