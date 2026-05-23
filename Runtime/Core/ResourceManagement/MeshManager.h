#pragma once

#include "Core/ResourceManagement/AResourceManager.h"
#include "CoreDef.h"
#include "Rendering/Resources/Mesh.h"

namespace NLS::Core::ResourceManagement
{
    class NLS_RESOURCE_MANAGEMENT_API MeshManager : public AResourceManager<Render::Resources::Mesh>
    {
    public:
        using Mesh = Render::Resources::Mesh;

        const char* GetResourceTypeName() const override { return "Mesh"; }

        Mesh* CreateResource(const std::string& path) override;
        void DestroyResource(Mesh* resource) override;
        void ReloadResource(Mesh* resource, const std::string& path) override;

        static std::string ResolveResourcePath(const std::string& path);
        static std::string ResolveArtifactResourcePath(const std::string& path);
        static const std::string& ProjectAssetsRoot();
    };
}
