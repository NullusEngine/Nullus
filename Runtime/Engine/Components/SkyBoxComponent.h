#pragma once

#include "EngineDef.h"

#include <Rendering/Resources/Model.h>
#include <Rendering/Resources/TextureCube.h>
#include <Rendering/Resources/Material.h>

#include "Components/Component.h"
#include "Reflection/Macros.h"
#include "Components/SkyBoxComponent.generated.h"

namespace NLS::Engine::Components
{
	/**
	* A ModelRenderer is necessary in combination with a MaterialRenderer to render a model in the world
	*/
	CLASS() class NLS_ENGINE_API SkyBoxComponent : public Component
	{
    public:
		GENERATED_BODY()
        using Model = Render::Resources::Model;
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
		Model* GetModel() const { return mModel; }

		/**
		 * @brief 
		 * @return 
		 */
		Material* GetMaterial() const { return mMaterial; }

	private:
		Model* mModel = nullptr;
		Material* mMaterial = nullptr;
	};
}
