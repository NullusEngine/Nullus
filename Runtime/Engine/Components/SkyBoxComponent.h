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
		/**
		* Constructor
		*/
		SkyBoxComponent();

		/**
		 * @brief Set CubeMap
		 */
        FUNCTION()
		void SetCubeMap(NLS::Render::Resources::TextureCube* cubmap);

		/**
		 * @brief
		 * @return
		 */
        FUNCTION()
		NLS::Render::Resources::Model* GetModel() const { return mModel; }

		/**
		 * @brief 
		 * @return 
		 */
		NLS::Render::Resources::Material* GetMaterial() const { return mMaterial; }

	private:
		NLS::Render::Resources::Model* mModel = nullptr;
		NLS::Render::Resources::Material* mMaterial = nullptr;
	};
}
