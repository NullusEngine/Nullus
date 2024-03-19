#include "MeshLoader.h"

using namespace NLS;

APIMeshLoadFunction MeshLoader::apiFunction = nullptr;

void MeshLoader::RegisterAPILoadFunction(const APIMeshLoadFunction& f)
{
    if (apiFunction)
    {
        std::cout << __FUNCTION__ << " replacing previously defined API function." << std::endl;
    }
    apiFunction = f;
}

MeshGeometry* MeshLoader::LoadAPIMesh(const std::string& filename)
{
    if (apiFunction == nullptr)
    {
        std::cout << __FUNCTION__ << " no API Function has been defined!" << std::endl;
        return nullptr;
    }
    return apiFunction(filename);
}
