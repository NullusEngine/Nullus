#pragma once

#include <variant>

#include <Resources/Material.h>
#include <Rendering/Resources/Mesh.h>
#include <Components/MeshFilter.h>
#include <Components/MeshRenderer.h>

#include "Panels/AViewControllable.h"

namespace NLS::Editor::Panels
{
	/**
	* Provide a view for assets
	*/
	class AssetView : public Editor::Panels::AViewControllable
	{
	public:
		using ViewableResource = std::variant<Render::Resources::Mesh*, Render::Resources::Texture2D*, Render::Resources::Material*>;

		/**
		* Constructor
		* @param p_title
		* @param p_opened
		* @param p_windowSettings
		*/
		AssetView(
			const std::string& p_title,
			bool p_opened,
			const UI::PanelWindowSettings& p_windowSettings
		);

		/**
		 * Returns the scene used by this view
		 */
		virtual Engine::SceneSystem::Scene* GetScene();
        void EnsureRenderer() override;

		/**
		* Defines the resource to preview
		* @parma p_resource
		*/
		void SetResource(ViewableResource p_resource);

		/**
		* Clear any currently viewed resource
		*/
		void ClearResource();

		/**
		* Set the currently viewed resource to the given texture
		* @param p_texture
		*/
        void SetTexture(Render::Resources::Texture2D& p_texture);

		/**
		* Set the currently viewed resource to the given mesh
		* @param p_mesh
		*/
        void SetMesh(Render::Resources::Mesh& p_mesh);

		/**
		* Set the currently viewed resource to the given material
		* @param p_material
		*/
        void SetMaterial(Render::Resources::Material& p_material);

		/**
		* Return the currently previewed resource
		*/
		const ViewableResource& GetResource() const;

	private:
        Render::Resources::Material m_defaultMaterial;
        Render::Resources::Material m_textureMaterial;

        Engine::GameObject* m_assetActor = nullptr;
        Engine::Components::MeshFilter* m_meshFilter = nullptr;
        Engine::Components::MeshRenderer* m_modelRenderer = nullptr;
		ViewableResource m_resource;
        Engine::SceneSystem::Scene m_scene;
	};
}
