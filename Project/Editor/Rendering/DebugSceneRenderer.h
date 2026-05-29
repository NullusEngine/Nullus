#pragma once

#include <Rendering/Entities/Camera.h>

#include "GameObject.h"
#include <Engine/SceneSystem/SceneManager.h>
#include "Components/MeshFilter.h"
#include "Components/MeshRenderer.h"
#include <Rendering/Resources/Material.h>
#include "Components/LightComponent.h"
#include <Engine/Rendering/DeferredSceneRenderer.h>
#include "Core/Context.h"

namespace NLS::Editor::Panels { class AView; }

namespace NLS::Editor::Rendering
{
	/**
	* Provide a debug layer on top of the default scene renderer to see "invisible" entities such as
	* lights, cameras, 
	*/
class DebugSceneRenderer : public Engine::Rendering::DeferredSceneRenderer
	{
	public:
		struct DebugSceneDescriptor
		{
			Engine::GameObject* highlightedGameObject;
            Engine::GameObject* selectedGameObject;
            bool requestPickingFrame = false;
		};

		/**
		* Constructor of the Renderer
		* @param p_driver
		*/
		DebugSceneRenderer(NLS::Render::Context::Driver& p_driver);

	protected:
        std::optional<NLS::Render::Context::FrameSnapshot> BuildFrameSnapshot(
			const NLS::Render::Data::FrameDescriptor& frameDescriptor) const override;
        NLS::Render::Context::PreparedRenderSceneBuilder BuildPreparedRenderSceneBuilder(
            const NLS::Render::Context::FrameSnapshot& snapshot) const override;
        void OnThreadedFramePublished(uint64_t publishedFrameId) override;
        void OnThreadedFramePublishFailed() override;
	};
}
