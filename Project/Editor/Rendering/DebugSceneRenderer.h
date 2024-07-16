#pragma once

#include <Rendering/Entities/Camera.h>
#include <Rendering/Features/DebugShapeRenderFeature.h>

#include "GameObject.h"
#include <Engine/SceneSystem/SceneManager.h>
#include "Components/MeshRenderer.h"
#include <Rendering/Resources/Material.h>
#include "Components/LightComponent.h"
#include <Engine/Rendering/SceneRenderer.h>
#include "Core/GizmoBehaviour.h"
#include "Core/Context.h"

namespace NLS::Editor::Panels { class AView; }

namespace NLS::Editor::Rendering
{
	/**
	* Provide a debug layer on top of the default scene renderer to see "invisible" entities such as
	* lights, cameras, 
	*/
class DebugSceneRenderer : public Engine::Rendering::SceneRenderer
	{
	public:
		struct DebugSceneDescriptor
		{
			Editor::Core::EGizmoOperation gizmoOperation;
			Engine::GameObject* highlightedActor;
            Engine::GameObject* selectedActor;
			std::optional<Editor::Core::GizmoBehaviour::EDirection> highlightedGizmoDirection;
		};

		/**
		* Constructor of the Renderer
		* @param p_driver
		*/
		DebugSceneRenderer(NLS::Render::Context::Driver& p_driver);
	};
}