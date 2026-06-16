#pragma once

#include "EngineDef.h"

#include <Rendering/Resources/Mesh.h>
#include <Rendering/Resources/TextureCube.h>
#include <Rendering/Resources/Material.h>

#include "Components/Component.h"
#include "Reflection/Macros.h"
#include "Components/SkyBoxComponent.generated.h"

namespace NLS::Engine::Components
{
	/**
	* A MeshRenderer makes a mesh renderable in the world.
	*/
	CLASS(NLS_ENGINE_API SkyBoxComponent) : public Component
	{
    public:
		GENERATED_BODY()
        using Mesh = Render::Resources::Mesh;
        using Material = Render::Resources::Material;
        using TextureCube = Render::Resources::TextureCube;

        ENUM(SkyMode)
        {
            CubeMap = 0,
            Procedural = 1
        };

		/**
		* Constructor
		*/
		SkyBoxComponent();

		/**
		 * @brief Set CubeMap
		 */
        FUNCTION()
		void SetCubeMap(TextureCube* cubmap);

        PROPERTY(skyMode)
        FUNCTION()
        SkyMode GetSkyMode() const { return mSkyMode; }

        PROPERTY(skyMode)
        FUNCTION()
        void SetSkyMode(SkyMode mode);

        /**
		 * @brief
		 * @return
		 */
        FUNCTION()
		Mesh* GetMesh() const { return mMesh; }

		/**
		 * @brief 
		 * @return 
		 */
		Material* GetMaterial() const { return mMaterial; }

	private:
        void ApplySkyMode();

		Mesh* mMesh = nullptr;
		Material* mMaterial = nullptr;
        TextureCube* mCubeMap = nullptr;
        SkyMode mSkyMode = SkyMode::Procedural;
	};
}
