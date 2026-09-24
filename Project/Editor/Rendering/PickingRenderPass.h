#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <unordered_map>
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
        std::optional<NLS::Editor::Panels::HitProxyPickingSignature> GetReadablePickingFrameSignature() const;
        std::optional<NLS::Editor::Panels::HitProxyPickingSignature> GetSubmittedPickingFrameSignature() const;
        bool CanResolvePickingRequest(
            NLS::Editor::Panels::HitProxyPickingRequestKind requestKind,
            uint64_t minimumReadablePickingFrameSerial,
            const NLS::Editor::Panels::HitProxyPickingSignature& requestSignature) const;
        const std::optional<NLS::Render::Context::RenderPassCommandInput>& GetPreparedThreadedPassInput() const;
        std::optional<NLS::Render::Context::RenderPassCommandInput> ConsumePreparedThreadedPassInput();

		bool ManagesOwnRenderPass() const override { return true; }

	private:
        void OnBeginFrame(const NLS::Render::Data::FrameDescriptor& p_frameDescriptor) override;
        virtual void Draw(NLS::Render::Data::PipelineState p_pso) override;
        void PromotePendingFrameIfReadbackAvailable() const;
        PickingResult DecodePickingResult(
            const PickingReadbackLifecycle<Engine::SceneSystem::Scene>::Frame& frame,
            const uint8_t (&pixel)[3]) const;
        PickingResult DecodePickingPickId(
            const PickingReadbackLifecycle<Engine::SceneSystem::Scene>::Frame& frame,
            uint32_t pickId) const;
        void ResetPickingFrameState();
        PickingReadbackLifecycle<Engine::SceneSystem::Scene>::Frame BuildSubmittedReadbackFrame(
            Engine::SceneSystem::Scene& scene,
            uint64_t serial,
            uint64_t readbackGeneration,
            const NLS::Editor::Panels::HitProxyPickingSignature& signature) const;
        NLS::Editor::Panels::HitProxyPickingSignature BuildCurrentPickingSignature(
            Engine::SceneSystem::Scene& scene) const;
        bool CanReuseReadablePickingFrameForSignature(
            const NLS::Editor::Panels::HitProxyPickingSignature& signature) const;
        bool RenderPickingScene(NLS::Render::Data::PipelineState p_pso);
        std::optional<NLS::Render::Context::RenderPassCommandInput> BuildThreadedPassInput(
            NLS::Render::Data::PipelineState p_pso,
            uint64_t readbackGeneration,
            const NLS::Editor::Panels::HitProxyPickingSignature& pickingSignature);
        bool TryReuseCapturedPickingDrawCommands(
            const NLS::Editor::Panels::HitProxyPickingSignature& pickingSignature,
            NLS::Render::Data::PipelineState p_pso,
            bool usesVisibleDrawSources,
            std::vector<NLS::Render::Context::RecordedDrawCommandInput>& outDrawCommands);
        void StoreCapturedPickingDrawCommands(
            const NLS::Editor::Panels::HitProxyPickingSignature& pickingSignature,
            NLS::Render::Data::PipelineState p_pso,
            bool usesVisibleDrawSources,
            const std::vector<NLS::Render::Context::RecordedDrawCommandInput>& drawCommands);
        bool TryReuseCachedPickingPixel(
            const PickingReadbackLifecycle<Engine::SceneSystem::Scene>::Frame& frame,
            uint64_t coordinateKey,
            PickingResult& outResult);
        void StoreCachedPickingPixel(
            const PickingReadbackLifecycle<Engine::SceneSystem::Scene>::Frame& frame,
            uint64_t coordinateKey,
            uint32_t pickId);
        void CapturePickableModels(
            NLS::Render::Data::PipelineState p_pso,
            Engine::SceneSystem::Scene& p_scene,
            std::vector<NLS::Render::Context::RecordedDrawCommandInput>& outDrawCommands);
        void CapturePickableModelSources(
            const std::vector<Engine::Rendering::ScenePickablePrimitiveDrawSource>& sources,
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
        void DrawPickableModelSources(
            const std::vector<Engine::Rendering::ScenePickablePrimitiveDrawSource>& sources);
        void DrawPickableCameras(NLS::Render::Data::PipelineState p_pso, Engine::SceneSystem::Scene& p_scene);
        void DrawPickableLights(NLS::Render::Data::PipelineState p_pso, Engine::SceneSystem::Scene& p_scene);
        uint32_t RegisterPickableGameObject(Engine::GameObject& actor);
        uint32_t RegisterPickableInstance(NLS::InstanceID instanceId);

	private:
        DebugModelRenderer m_debugModelRenderer;
		NLS::Render::Buffers::Framebuffer m_gameObjectPickingFramebuffer;
		NLS::Render::Resources::Material m_gameObjectPickingMaterial;
        NLS::Render::Resources::Material m_lightMaterial;
        mutable PickingReadbackLifecycle<Engine::SceneSystem::Scene> m_readbackLifecycle;
        std::optional<NLS::Render::Context::RenderPassCommandInput> m_preparedThreadedPassInput;
        mutable std::vector<NLS::InstanceID> m_submittedPickRegistry;
        mutable std::unordered_map<NLS::InstanceID, uint32_t> m_submittedPickIds;
        uint64_t m_submittedPickingFrameSerial = 0u;

        /**
         * Everything that can change the picking image without changing the
         * picking signature is captured here so both the cached draw commands
         * and the cached pixel results stay valid together.
         */
        struct PickingCaptureGuards
        {
            uint64_t deviceIdentity = 0u;
            uint64_t pipelineStateBits = 0u;
            const NLS::Render::Resources::Shader* pickingShader = nullptr;
            uint64_t pickingShaderGeneration = 0u;
            const NLS::Render::Resources::Shader* helperShader = nullptr;
            uint64_t helperShaderGeneration = 0u;
            const void* colorAttachmentIdentity = nullptr;
            const NLS::Render::Resources::Mesh* cameraHelperMesh = nullptr;
            const NLS::Render::Resources::Mesh* lightHelperMesh = nullptr;
            float lightBillboardScale = 0.0f;

            bool operator==(const PickingCaptureGuards&) const = default;
        };
        PickingCaptureGuards BuildPickingCaptureGuards(
            NLS::Render::Data::PipelineState p_pso) const;

        /**
         * Captured picking commands own immutable uniform-buffer snapshots. They
         * can be reused while the picked scene, draw sources, view, shader, device
         * and pipeline state are unchanged. Camera and viewport data live in the
         * frame binding, which is refreshed for the current slot. Object-indexed
         * commands stay on the capture path because their binding sets reference
         * per-slot GPU data directly.
         */
        struct CapturedPickingDrawCommands
        {
            NLS::Editor::Panels::HitProxyPickingSignature signature;
            PickingCaptureGuards guards;
            bool usesVisibleDrawSources = false;
            std::vector<NLS::Render::Context::RecordedDrawCommandInput> drawCommands;
            std::vector<NLS::InstanceID> pickRegistry;
            std::unordered_map<NLS::InstanceID, uint32_t> pickIds;
        };
        CapturedPickingDrawCommands m_capturedPickingDrawCommands;

        /**
         * Pixel results decoded from one readable picking image. The image is
         * immutable for a given signature and capture guards, so repeated hover /
         * click requests for the same coordinate reuse a single GPU readback.
         */
        struct CachedPickingPixels
        {
            NLS::Editor::Panels::HitProxyPickingSignature signature;
            PickingCaptureGuards guards;
            bool valid = false;
            std::unordered_map<uint64_t, uint32_t> pickIdsByCoordinate;
        };
        CachedPickingPixels m_cachedPickingPixels;
	};
}
