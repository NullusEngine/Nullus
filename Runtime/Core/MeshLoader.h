#pragma once
#include "CoreDef.h"

#include <functional>
#include "RHI/MeshGeometry.h"

namespace NLS
{
typedef std::function<MeshGeometry*(const std::string& filename)> APIMeshLoadFunction;

class NLS_CORE_API MeshLoader
{
public:
    static void RegisterAPILoadFunction(const APIMeshLoadFunction& f);

    static MeshGeometry* LoadAPIMesh(const std::string& filename);

protected:
    static APIMeshLoadFunction apiFunction;
};
} // namespace NLS
