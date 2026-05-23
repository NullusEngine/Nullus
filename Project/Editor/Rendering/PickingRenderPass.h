#pragma once

#include <cstdint>
#include <optional>
#include <variant>
#include <vector>

#include <Rendering/Entities/Camera.h>

#include <GameObject.h>
#include <SceneSystem/SceneManager.h>
#include <Components/MeshRenderer.h>
#include <Resources/Material.h>
#include <Components/LightComponent.h>
#include <Rendering/BaseSceneRenderer.h>

#include "Core/Context.h"
#include "Rendering/DebugModelRenderer.h"
#include "Rendering/PickingReadbackLifecycle.h"

namespace NLS::Editor::Rendering
{
	/**
	* Draw the scene for GameObject picking
	*/
	class PickingRenderPass : public NLS::Render::Core::ARenderPass
	{
	public:
		using PickingResult =
			std::optional<
            std::variant<Engine::GameObject*>
			>;

		/**
		* Constructor
		* @param p_renderer
		*/
		PickingRenderPass(NLS::Render::Core::CompositeRenderer& p_renderer);

		/**
		* Return the picking result at the given position
		* @param p_x
		* @param p_y
		*/
		PickingResult PickAtRenderCoordinate(uint32_t p_x, uint32_t p_y);
		bool SupportsPickingReadback() const;
        bool HasReadablePickingFrame() const;
        uint64_t GetReadablePickingFrameSerial() const;
        uint64_t GetSubmittedPickingFrameSerial() const;
        std::optional<NLS::Render::Context::RenderPassCommandInput> GetPreparedThreadedPassInput() const;

		bool ManagesOwnRenderPass() const override { return true; }

	private:
        void OnBeginFrame(const NLS::Render::Data::FrameDescriptor& p_frameDescriptor) override;
        virtual void Draw(NLS::Render::Data::PipelineState p_pso) override;
        void PromotePendingFrameIfReadbackAvailable() const;
        PickingResult DecodePickingResult(
            const PickingReadbackLifecycle<Engine::SceneSystem::Scene>::Frame& frame,
            const uint8_t (&pixel)[3]) const;
        void ResetPickingFrameState();
        PickingReadbackLifecycle<Engine::SceneSystem::Scene>::Frame BuildSubmittedReadbackFrame(
            Engine::SceneSystem::Scene& scene,
            uint64_t serial) const;
        bool RenderPickingScene(NLS::Render::Data::PipelineState p_pso);
        std::optional<NLS::Render::Context::RenderPassCommandInput> BuildThreadedPassInput(
            NLS::Render::Data::PipelineState p_pso);
        void CapturePickableModels(
            NLS::Render::Data::PipelineState p_pso,
            Engine::SceneSystem::Scene& p_scene,
            std::vector<NLS::Render::Context::RecordedDrawCommandInput>& outDrawCommands);
        void CapturePickableCameras(
            NLS::Render::Data::PipelineState p_pso,
            Engine::SceneSystem::Scene& p_scene,
            std::vector<NLS::Render::Context::RecordedDrawCommandInput>& outDrawCommands);
        void CapturePickableLights(
            NLS::Render::Data::PipelineState p_pso,
            Engine::SceneSystem::Scene& p_scene,
            std::vector<NLS::Render::Context::RecordedDrawCommandInput>& outDrawCommands);
        void DrawPickableModels(NLS::Render::Data::PipelineState p_pso, Engine::SceneSystem::Scene& p_scene);
        void DrawPickableCameras(NLS::Render::Data::PipelineState p_pso, Engine::SceneSystem::Scene& p_scene);
        void DrawPickableLights(NLS::Render::Data::PipelineState p_pso, Engine::SceneSystem::Scene& p_scene);
        uint32_t RegisterPickableGameObject(Engine::GameObject& actor);

	private:
        DebugModelRenderer m_debugModelRenderer;
		NLS::Render::Buffers::Framebuffer m_gameObjectPickingFramebuffer;
		NLS::Render::Resources::Material m_gameObjectPickingMaterial;
        NLS::Render::Resources::Material m_lightMaterial;
        mutable PickingReadbackLifecycle<Engine::SceneSystem::Scene> m_readbackLifecycle;
        std::optional<NLS::Render::Context::RenderPassCommandInput> m_preparedThreadedPassInput;
        mutable std::vector<Engine::GameObject*> m_submittedPickRegistry;
        uint64_t m_submittedPickingFrameSerial = 0u;
	};
}
