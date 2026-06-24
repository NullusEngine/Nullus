#pragma once

#include <cstdint>
#include <memory>
#include <string>

#include "Components/Component.h"
#include "EngineDef.h"
#include "Eventing/Event.h"
#include "Reflection/Macros.h"
#include "Rendering/Resources/Mesh.h"
#include "Serialize/PPtr.h"
#include "Components/MeshFilter.generated.h"

namespace NLS::Engine::Components
{
    CLASS(NLS_ENGINE_API MeshFilter, ComponentMenu("Mesh/Mesh Filter")) : public Component
    {
    public:
        GENERATED_BODY()
        using Mesh = NLS::Render::Resources::Mesh;

        MeshFilter();
        MeshFilter(const MeshFilter& other);
        MeshFilter& operator=(const MeshFilter& other);
        ~MeshFilter() override;

        void SetMesh(Mesh* p_mesh);
        void SetResolvedMeshFromReference(Mesh* p_mesh);
        void SetResolvedTransientMeshFromReference(std::shared_ptr<Mesh> p_mesh);
        bool HasResolvedTransientMesh() const;
        Mesh* ResolveMesh();
        uint64_t GetRenderRevision() const;
        void SetMeshPath(const std::string& p_path);

        PROPERTY(mesh)
        FUNCTION()
        NLS::Engine::Serialize::PPtr<Mesh> GetMeshReference() const;
        PROPERTY(mesh)
        FUNCTION()
        void SetMeshReference(NLS::Engine::Serialize::PPtr<Mesh> p_reference);
        void SetMeshObjectIdentifier(const NLS::Engine::Serialize::ObjectIdentifier& p_identifier);

        std::string GetModelPath() const;
        void SetModelPath(const std::string& p_path);
        void SetModelPathHint(const std::string& p_path);

    private:
        void NotifyMeshChanged();

        NLS::Engine::Serialize::PPtr<Mesh> mesh;
        std::shared_ptr<Mesh> m_transientMesh;
        std::string m_meshPath;
        std::string m_failedMeshPath;
        uint64_t m_renderRevision = 1u;
    };
}
