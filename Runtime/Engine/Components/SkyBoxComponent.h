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

		/**
		* Constructor
		*/
		SkyBoxComponent();

		/**
		 * @brief Set CubeMap
		 */
        FUNCTION()
		void SetCubeMap(TextureCube* cubmap);

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
		Mesh* mMesh = nullptr;
		Material* mMaterial = nullptr;
	};
}
