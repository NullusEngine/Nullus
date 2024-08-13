#pragma once

#include "EngineDef.h"

#include <Rendering/Resources/Model.h>
#include <Rendering/Resources/TextureCube.h>
#include <Rendering/Resources/Material.h>

#include "Components/Component.h"

namespace NLS::Engine::Components
{
	/**
	* A ModelRenderer is necessary in combination with a MaterialRenderer to render a model in the world
	*/
	class NLS_ENGINE_API SkyBoxComponent : public Component
	{
	public:
		/**
		* Constructor
		*/
		SkyBoxComponent();

		/**
		 * @brief Set CubeMap
		 */
		void SetCubeMap(NLS::Render::Resources::TextureCube* cubmap);

		/**
		 * @brief
		 * @return
		 */
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