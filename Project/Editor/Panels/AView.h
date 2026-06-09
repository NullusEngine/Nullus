#pragma once


#include <deque>
#include <memory>
#include <optional>

#include <UI/Panels/PanelWindow.h>
#include <UI/Widgets/Visual/Image.h>
#include <Rendering/Buffers/Framebuffer.h>
#include <Rendering/Data/FrameInfo.h>
#include <Rendering/Entities/Camera.h>
#include <Rendering/BaseSceneRenderer.h>

namespace NLS::Editor::Panels
{
    struct ViewOverlayCameraMatrices;
}

struct ImDrawListSplitter;

namespace NLS::Editor::Panels
{
    struct ViewOverlayCameraMatrices
    {
        uint64_t frameId = 0u;
        Maths::Matrix4 view = Maths::Matrix4::Identity;
        Maths::Matrix4 projection = Maths::Matrix4::Identity;
    };

	/**
	* Base class for any view
	*/
	class AView : public UI::PanelWindow
	{
	public:
		/**
		* Constructor
		* @param p_title
		* @param p_opened
		* @param p_windowSettings
		*/
		AView(
			const std::string& p_title,
			bool p_opened,
			const UI::PanelWindowSettings& p_windowSettings
		);
        ~AView() override;

		/**
		* Update the view
		* @param p_deltaTime
		*/
		virtual void Update(float p_deltaTime);

		/**
		* Custom implementation of the draw method
		*/
		void _Draw_Impl() override;

		/**
		* Prepare the renderer for rendering
		*/
		virtual void InitFrame();
        virtual void EnsureRenderer();

		/**
		* Render the view
		*/
		void Render();

		/**
		* Draw the frame (m_renderer->Draw() if not overriden)
		* @note You don't need to begin/end frame inside of this method, as this is called after begin, and after end
		*/
		virtual void DrawFrame();

		/**
		* Hook called after the renderer frame has been fully submitted.
		*/
		virtual void AfterRenderFrame();

		/**
		* Returns the camera used by this view
		*/
		virtual NLS::Render::Entities::Camera* GetCamera() = 0;

		/**
		* Returns the scene used by this view
		*/
		virtual Engine::SceneSystem::Scene* GetScene() = 0;

		/**
		* Returns the size of the panel ignoring its titlebar height
		*/
		std::pair<uint16_t, uint16_t> GetSafeSize() const;

		/**
		* Returns the renderer used by this view
		*/
        const Engine::Rendering::BaseSceneRenderer& GetRenderer() const;

		/**
		* Returns the frame info snapshot published after this view's latest successful render.
		*/
		const std::optional<Render::Data::FrameInfo>& GetLastRenderedFrameInfoSnapshot() const;

		/**
		* Returns true if the given mouse position is inside the rendered view content.
		*/
		bool IsMouseWithinView(const Maths::Vector2& mousePosition) const;

		/**
		* Returns true when the previous UI frame drew viewport image bounds that can be used for input.
		*/
		bool HasViewportImageInputBounds() const;

		/**
		* Returns true when the current UI frame has drawn this view's viewport image.
		*/
		bool WasViewportImageDrawnThisFrame() const;

		/**
		* Convert a window-space mouse position to view-local render coordinates.
		*/
		std::optional<Maths::Vector2> GetLocalViewPosition(const Maths::Vector2& mousePosition) const;

	protected:
		void OnBeforeDrawWidgets() override;
		void OnAfterDrawWidgets() override;
        virtual void DrawPreRenderViewportOverlay();
        virtual void DrawViewportOverlay();
        virtual Engine::Rendering::BaseSceneRenderer::SceneDescriptor CreateSceneDescriptor();
		virtual bool RequiresRetiredFrameConsumption() const;
		void SetRequiresRetiredFrameConsumption(bool requiresRetiredFrameConsumption);
        virtual bool RequiresImmediateRetiredFrameReadback() const;
        void SetRequiresImmediateRetiredFrameReadback(bool requiresImmediateRetiredFrameReadback);
        virtual bool RequiresSynchronizedRetiredFramePresentation() const;
		void SyncViewToCurrentContentRegion();
		void Render(uint16_t p_width, uint16_t p_height);
		bool ApplyResolvedViewSize(uint16_t p_width, uint16_t p_height);
        void UpdatePreRenderOverlayCameraMatrices();
        void BeginViewportOverlayDrawListChannels();
        void FinishPreRenderViewportOverlayDrawList();
        void EndViewportOverlayDrawListChannels();
        void MarkViewportImageInputBoundsForLastDraw();
        void UpdateSubmittedOverlayCameraMatrices(
            const ViewOverlayCameraMatrices& submittedMatrices,
            bool threadedRendering,
            bool framePublished,
            uint64_t latestPublishedFrameId,
            uint64_t latestRetiredFrameId);
        Maths::Vector2 GetCurrentViewportImageMin() const;
        Maths::Vector2 GetCurrentViewportImageMax() const;
        ViewOverlayCameraMatrices GetViewportOverlayCameraMatrices() const;
        bool HasViewportImageBounds() const;
        Maths::Vector2 GetViewportImageMin() const;
        Maths::Vector2 GetViewportImageMax() const;

	protected:
		UI::Widgets::Image* m_image;

		Maths::Vector3 m_gridColor = Maths::Vector3 { 0.176f, 0.176f, 0.176f };

		Render::Buffers::Framebuffer m_fbo;
        std::unique_ptr<Engine::Rendering::BaseSceneRenderer> m_renderer;
		std::pair<uint16_t, uint16_t> m_lastResolvedViewSize { 0u, 0u };
		std::optional<std::pair<uint16_t, uint16_t>> m_pendingResolvedViewSize;
        std::optional<ViewOverlayCameraMatrices> m_overlayCameraMatricesForCurrentDraw;
        std::deque<ViewOverlayCameraMatrices> m_submittedOverlayCameraMatrices;
        std::unique_ptr<ImDrawListSplitter> m_viewportOverlayDrawSplitter;
		std::optional<Render::Data::FrameInfo> m_lastRenderedFrameInfo;
        std::optional<uint64_t> m_lastAvailablePublishedFrameCount;
        bool m_viewportOverlayDrawListChannelsActive = false;
		bool m_requiresRetiredFrameConsumption = false;
        bool m_requiresImmediateRetiredFrameReadback = false;
        bool m_resizedViewThisFrame = false;
        bool m_viewportImageAvailableForInput = false;
	};
}
