#pragma once

#include <array>
#include <cstdint>
#include "Components/Component.h"
#include "EngineDef.h"
#include "Reflection/Array.h"
#include "Reflection/Macros.h"
#include "Math/Matrix4.h"
#include "Rendering/Geometry/BoundingSphere.h"
#include "Rendering/Resources/Material.h"
#include "Serialize/PPtr.h"
#include "Components/MeshRenderer.generated.h"

namespace NLS::Engine::Components
{
	/**
	* A MeshRenderer makes a MeshFilter renderable and owns material/culling state.
	*/
	CLASS(NLS_ENGINE_API MeshRenderer, ComponentMenu("Rendering/Mesh Renderer")) : public Component
	{
    public:


		GENERATED_BODY()
        static constexpr uint8_t kMaxMaterialCount = 0xFF;
        using Material = Render::Resources::Material;
        using MaterialList = std::array<Material*, kMaxMaterialCount>;

		/**
		* Defines how the model renderer bounding sphere should be interpreted
		*/
        ENUM(EFrustumBehaviour)
		{
			DISABLED = 0,
			CULL_MODEL = 1,
			CULL_MESHES = 2,
			CULL_CUSTOM = 3
		};

		/**
		* Constructor
		* @param p_owner
		*/
		MeshRenderer();
        MeshRenderer(const MeshRenderer& other);
        MeshRenderer& operator=(const MeshRenderer& other);
        ~MeshRenderer() override;

        void OnCreate();

		/**
		* Sets a bounding mode
		* @param p_boundingMode
		*/
        FUNCTION()
		void SetFrustumBehaviour(EFrustumBehaviour p_boundingMode);

		/**
		* Returns the current bounding mode
		*/
        FUNCTION()
		EFrustumBehaviour GetFrustumBehaviour() const;

		/**
		* Returns the custom bounding sphere
		*/
        FUNCTION()
        const Render::Geometry::BoundingSphere& GetCustomBoundingSphere() const;

		/**
		* Sets the custom bounding sphere
		* @param p_boundingSphere
		*/
        FUNCTION()
        void SetCustomBoundingSphere(const Render::Geometry::BoundingSphere& p_boundingSphere);

        /**
         * Fill the renderer material slots with the given material.
         */
        FUNCTION()
        void FillWithMaterial(Material& p_material);

        void SetMaterialAtIndex(uint8_t p_index, Material& p_material);
        void SetResolvedMaterialFromReference(uint8_t p_index, Material& p_material);
        Material* GetMaterialAtIndex(uint8_t p_index);
        void RemoveMaterialAtIndex(uint8_t p_index);
        void RemoveMaterialByInstance(Material& p_instance);
        void RemoveAllMaterials();
        void UpdateMaterialList();
        void SetUserMatrixElement(uint32_t p_row, uint32_t p_column, float p_value);
        float GetUserMatrixElement(uint32_t p_row, uint32_t p_column) const;

        FUNCTION()
        const Maths::Matrix4& GetUserMatrix() const;

        PROPERTY(materials)
        FUNCTION()
        NLS::Array<NLS::Engine::Serialize::PPtr<Material>> GetMaterialReferences() const;

        PROPERTY(materials)
        FUNCTION()
        void SetMaterialReferences(const NLS::Array<NLS::Engine::Serialize::PPtr<Material>>& p_references);
        void SetMaterialObjectIdentifiers(const NLS::Array<NLS::Engine::Serialize::ObjectIdentifier>& p_identifiers);

        NLS::Array<std::string> GetMaterialPaths() const;
        void SetMaterialPaths(const NLS::Array<std::string>& p_paths);
        void SetMaterialPathHints(const NLS::Array<std::string>& p_paths);
        void SetTransientRenderingSuppressed(bool suppressed);
        bool IsTransientRenderingSuppressed() const;
        void FillEmptySlotsWithMaterial(Material& p_material);

        FUNCTION()
        NLS::Array<float> GetUserMatrixValues() const;

        FUNCTION()
        void SetUserMatrixValues(const NLS::Array<float>& p_values);

        const MaterialList& GetMaterials() const;
        uint64_t GetRenderRevision() const;
        Material* ResolveMaterialAtIndex(uint8_t p_index);
        const MaterialList& ResolveMaterials();



	private:
        void MarkRenderStateChanged();
        size_t GetExpectedMaterialSlotCount();
        Material* ResolveMaterialSlot(size_t p_index);

        NLS::Array<NLS::Engine::Serialize::PPtr<Material>> materials;
        MaterialList m_materials;
        std::array<std::string, kMaxMaterialCount> m_materialPaths;
        std::array<std::string, kMaxMaterialCount> m_failedMaterialPaths;
        std::array<std::string, kMaxMaterialCount> m_materialNames;
        Maths::Matrix4 m_userMatrix;
        Render::Geometry::BoundingSphere m_customBoundingSphere = {{}, 1.0f};
		EFrustumBehaviour m_frustumBehaviour = EFrustumBehaviour::CULL_MODEL;
        bool m_transientRenderingSuppressed = false;
        uint64_t m_renderRevision = 1u;
	};
}
