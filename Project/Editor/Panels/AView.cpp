#include "Panels/AView.h"
#include "Panels/ViewFrameLifecycle.h"
#include "Core/EditorActions.h"
#include "Rendering/Context/DriverAccess.h"
#include "Rendering/Core/RendererStats.h"
#include "Rendering/FrameGraph/ExternalResourceBridge.h"
#include "Profiling/Profiler.h"
#include "ServiceLocator.h"
#include "UI/UIManager.h"
#include "ImGui/imgui.h"
#include <algorithm>
#include <limits>

using namespace NLS;

namespace
{
    std::optional<Render::Context::ThreadedFrameTelemetry> TryGetAvailableThreadedFrameTelemetry(
        Render::Context::Driver* driver)
    {
        if (driver == nullptr ||
            !Render::Context::DriverRendererAccess::IsThreadedRenderingEnabled(*driver))
        {
            return std::nullopt;
        }

        return Render::Context::DriverRendererAccess::TryGetThreadedFrameTelemetry(*driver);
    }

}

Editor::Panels::AView::AView
(
	const std::string& p_title,
	bool p_opened,
	const UI::PanelWindowSettings& p_windowSettings
) : PanelWindow(p_title, p_opened, p_windowSettings),
    m_viewportOverlayDrawSplitter(std::make_unique<ImDrawListSplitter>())
{
	m_image = &CreateWidget<UI::Widgets::Image>(m_fbo.GetOrCreateExplicitColorView("Editor.AView.Output"), Maths::Vector2{ 0.f, 0.f });
	m_image->flipVertically =
        NLS::Core::ServiceLocator::Contains<UI::UIManager>() &&
        NLS_SERVICE(UI::UIManager).ShouldFlipPresentedRenderTargetVertically();
	panelSettings.scrollable = false;
}

Editor::Panels::AView::~AView() = default;

void Editor::Panels::AView::Update(float p_deltaTime)
{
	(void)p_deltaTime;
}

void Editor::Panels::AView::_Draw_Impl()
{
    NLS_SERVICE(UI::UIManager).PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
	UI::PanelWindow::_Draw_Impl();
    NLS_SERVICE(UI::UIManager).PopStyleVar();
}

void Editor::Panels::AView::OnBeforeDrawWidgets()
{
    {
        NLS_PROFILE_NAMED_SCOPE("AView::SyncViewToCurrentContentRegion");
	    SyncViewToCurrentContentRegion();
    }
    {
        NLS_PROFILE_NAMED_SCOPE("AView::DrawPreRenderOverlay");
        UpdatePreRenderOverlayCameraMatrices();
        BeginViewportOverlayDrawListChannels();
        DrawPreRenderViewportOverlay();
        FinishPreRenderViewportOverlayDrawList();
    }
    {
        NLS_PROFILE_NAMED_SCOPE("AView::RenderView");
	    Render(m_lastResolvedViewSize.first, m_lastResolvedViewSize.second);
    }
}

void Editor::Panels::AView::OnAfterDrawWidgets()
{
    NLS_PROFILE_NAMED_SCOPE("AView::DrawViewportOverlay");
    DrawViewportOverlay();
    EndViewportOverlayDrawListChannels();
}

void Editor::Panels::AView::InitFrame()
{
	m_renderer->AddDescriptor<Engine::Rendering::BaseSceneRenderer::SceneDescriptor>(
		CreateSceneDescriptor()
	);
}

void Editor::Panels::AView::EnsureRenderer()
{
}

void Editor::Panels::AView::Render()
{
	auto [winWidth, winHeight] = GetSafeSize();
	Render(winWidth, winHeight);
}

void Editor::Panels::AView::SyncViewToCurrentContentRegion()
{
    m_resizedViewThisFrame = false;

	const ImVec2 availableContentRegion = ImGui::GetContentRegionAvail();
	const float clampedWidth = std::clamp(
		availableContentRegion.x,
		0.0f,
		static_cast<float>(std::numeric_limits<uint16_t>::max()));
	const float clampedHeight = std::clamp(
		availableContentRegion.y,
		0.0f,
		static_cast<float>(std::numeric_limits<uint16_t>::max()));

	const auto winWidth = static_cast<uint16_t>(clampedWidth);
	const auto winHeight = static_cast<uint16_t>(clampedHeight);

    std::pair<uint16_t, uint16_t> requestedSize { winWidth, winHeight };
    auto* driver = Render::Context::TryGetLocatedDriver();
    const bool threadedRendering =
        driver != nullptr &&
        Render::Context::DriverRendererAccess::IsThreadedRenderingEnabled(*driver);
    auto currentTelemetry = TryGetAvailableThreadedFrameTelemetry(driver);

    if (threadedRendering &&
        (Editor::Panels::ShouldDeferRetirementAwareViewResizeUntilTelemetryAvailable(
                requestedSize,
                m_lastResolvedViewSize,
                RequiresRetiredFrameConsumption(),
                currentTelemetry.has_value()) ||
            (currentTelemetry.has_value() &&
                Editor::Panels::ShouldDeferRetirementAwareViewResize(
                    requestedSize,
                    m_lastResolvedViewSize,
                    RequiresRetiredFrameConsumption(),
                    currentTelemetry.value()))))
    {
        m_pendingResolvedViewSize = requestedSize;
        m_image->size = Maths::Vector2(
            static_cast<float>(m_lastResolvedViewSize.first),
            static_cast<float>(m_lastResolvedViewSize.second));
        return;
    }

    if (m_pendingResolvedViewSize.has_value())
        requestedSize = m_pendingResolvedViewSize.value();

    if (!ApplyResolvedViewSize(requestedSize.first, requestedSize.second))
    {
        m_image->size = Maths::Vector2(
            static_cast<float>(m_lastResolvedViewSize.first),
            static_cast<float>(m_lastResolvedViewSize.second));
        return;
    }
    m_image->size = Maths::Vector2(
        static_cast<float>(m_lastResolvedViewSize.first),
        static_cast<float>(m_lastResolvedViewSize.second));
}

void Editor::Panels::AView::Render(const uint16_t p_width, const uint16_t p_height)
{
	NLS_PROFILE_NAMED_SCOPE(name.c_str());
	auto camera = GetCamera();
	auto scene = GetScene();

	if (p_width > 0 && p_height > 0 && camera && scene)
	{
        {
            NLS_PROFILE_NAMED_SCOPE("AView::EnsureRenderer");
            EnsureRenderer();
        }
        if (m_renderer == nullptr)
            return;

        Render::Context::ThreadedFrameTelemetry beforeTelemetry {};
        auto* driver = Render::Context::TryGetLocatedDriver();
        const bool threadedRendering =
            driver != nullptr &&
            Render::Context::DriverRendererAccess::IsThreadedRenderingEnabled(*driver);
        const auto beforeTelemetrySnapshot = TryGetAvailableThreadedFrameTelemetry(driver);
        if (beforeTelemetrySnapshot.has_value())
        {
            beforeTelemetry = beforeTelemetrySnapshot.value();
            m_lastAvailablePublishedFrameCount = beforeTelemetry.publishedFrameCount;
        }
        {
            NLS_PROFILE_NAMED_SCOPE("AView::InitFrame");
		    InitFrame();
        }

		Render::Data::FrameDescriptor frameDescriptor;
		frameDescriptor.renderWidth = p_width;
		frameDescriptor.renderHeight = p_height;
		frameDescriptor.camera = camera;
		const auto& clearColor = camera->GetClearColor();
		m_fbo.SetOptimizedColorClearValue(clearColor.x, clearColor.y, clearColor.z, 1.0f);
		m_image->textureView = m_fbo.GetOrCreateExplicitColorView("Editor.AView.Output");
        NLS::Render::FrameGraph::SetExternalSceneOutputFramebuffer(frameDescriptor, &m_fbo);

        {
            NLS_PROFILE_NAMED_SCOPE("AView::RendererBeginFrame");
		    m_renderer->BeginFrame(frameDescriptor);
        }
        ViewOverlayCameraMatrices submittedOverlayMatrices;
        submittedOverlayMatrices.view = camera->GetViewMatrix();
        submittedOverlayMatrices.projection = camera->GetProjectionMatrix();
		{
			NLS_PROFILE_NAMED_SCOPE("AView::DrawFrame");
			DrawFrame();
		}
        {
            NLS_PROFILE_NAMED_SCOPE("AView::RendererEndFrame");
		    m_renderer->EndFrame();
        }
        if (m_renderer->IsFrameInfoValid())
        {
            m_lastRenderedFrameInfo = m_renderer->GetFrameInfo();
        }
        std::optional<Render::Context::ThreadedFrameTelemetry> postRenderDrainTelemetry;
        if (Editor::Panels::ShouldDrainAfterRetirementAwareViewRender(
            RequiresRetiredFrameConsumption(),
            RequiresImmediateRetiredFrameReadback(),
            m_resizedViewThisFrame,
            RequiresSynchronizedRetiredFramePresentation()))
        {
            if (auto* driver = Render::Context::TryGetLocatedDriver();
                driver != nullptr && Render::Context::DriverRendererAccess::IsThreadedRenderingEnabled(*driver))
            {
                // Immediate readback consumers need the just-submitted offscreen frame retired.
                // Texture-only consumers can present the latest available texture without a CPU drain.
                {
                    NLS_PROFILE_NAMED_SCOPE("AView::DrainThreadedRendering");
                    if (Render::Context::DriverRendererAccess::TryDrainThreadedRendering(*driver))
                    {
                        postRenderDrainTelemetry = TryGetAvailableThreadedFrameTelemetry(driver);
                        if (postRenderDrainTelemetry.has_value() && m_lastRenderedFrameInfo.has_value())
                        {
                            Render::Core::RendererStats::ApplyThreadedFrameTelemetry(
                                postRenderDrainTelemetry.value(),
                                m_lastRenderedFrameInfo.value());
                        }
                    }
                }
            }
        }
        bool framePublished = !threadedRendering;
        uint64_t latestPublishedFrameId = 0u;
        uint64_t latestRetiredFrameId = 0u;
        if (threadedRendering && driver != nullptr)
        {
            const auto afterTelemetrySnapshot = postRenderDrainTelemetry.has_value()
                ? postRenderDrainTelemetry
                : TryGetAvailableThreadedFrameTelemetry(driver);
            if (afterTelemetrySnapshot.has_value())
            {
                const auto& afterTelemetry = afterTelemetrySnapshot.value();
                framePublished = Editor::Panels::DidThreadedFramePublishAdvance(
                    beforeTelemetrySnapshot,
                    m_lastAvailablePublishedFrameCount,
                    afterTelemetry);
                latestPublishedFrameId = afterTelemetry.latestPublishedFrameId;
                latestRetiredFrameId = afterTelemetry.latestRetiredFrameId;
                m_lastAvailablePublishedFrameCount = afterTelemetry.publishedFrameCount;
            }
            else if (beforeTelemetrySnapshot.has_value())
            {
                latestPublishedFrameId = beforeTelemetry.latestPublishedFrameId;
                latestRetiredFrameId = beforeTelemetry.latestRetiredFrameId;
            }
        }
        {
            NLS_PROFILE_NAMED_SCOPE("AView::UpdateSubmittedOverlayCameraMatrices");
            UpdateSubmittedOverlayCameraMatrices(
                submittedOverlayMatrices,
                threadedRendering,
                framePublished,
                latestPublishedFrameId,
                latestRetiredFrameId);
        }
        {
            NLS_PROFILE_NAMED_SCOPE("AView::AfterRenderFrame");
		    AfterRenderFrame();
        }
	}
}

void Editor::Panels::AView::DrawFrame()
{
	m_renderer->DrawFrame();
}

void Editor::Panels::AView::AfterRenderFrame()
{
}

void Editor::Panels::AView::DrawPreRenderViewportOverlay()
{
}

void Editor::Panels::AView::DrawViewportOverlay()
{
}

bool Editor::Panels::AView::RequiresRetiredFrameConsumption() const
{
    return m_requiresRetiredFrameConsumption;
}

void Editor::Panels::AView::SetRequiresRetiredFrameConsumption(const bool requiresRetiredFrameConsumption)
{
    m_requiresRetiredFrameConsumption = requiresRetiredFrameConsumption;
}

bool Editor::Panels::AView::RequiresImmediateRetiredFrameReadback() const
{
    return m_requiresImmediateRetiredFrameReadback;
}

void Editor::Panels::AView::SetRequiresImmediateRetiredFrameReadback(
    const bool requiresImmediateRetiredFrameReadback)
{
    m_requiresImmediateRetiredFrameReadback = requiresImmediateRetiredFrameReadback;
}

bool Editor::Panels::AView::RequiresSynchronizedRetiredFramePresentation() const
{
    return false;
}

bool Editor::Panels::AView::ApplyResolvedViewSize(const uint16_t p_width, const uint16_t p_height)
{
    m_resizedViewThisFrame = m_lastResolvedViewSize.first != p_width ||
        m_lastResolvedViewSize.second != p_height;

    if (m_resizedViewThisFrame)
    {
        auto* driver = Render::Context::TryGetLocatedDriver();
        const bool threadedRendering =
            driver != nullptr &&
            Render::Context::DriverRendererAccess::IsThreadedRenderingEnabled(*driver);
        const auto currentTelemetry = TryGetAvailableThreadedFrameTelemetry(driver);
        if (threadedRendering &&
            (Editor::Panels::ShouldDeferRetirementAwareViewResizeUntilTelemetryAvailable(
                    { p_width, p_height },
                    m_lastResolvedViewSize,
                    RequiresRetiredFrameConsumption(),
                    currentTelemetry.has_value()) ||
                (currentTelemetry.has_value() &&
                    Editor::Panels::ShouldDeferRetirementAwareViewResize(
                        { p_width, p_height },
                        m_lastResolvedViewSize,
                        RequiresRetiredFrameConsumption(),
                        currentTelemetry.value()))))
        {
            m_pendingResolvedViewSize = { p_width, p_height };
            m_resizedViewThisFrame = false;
            return false;
        }

        if (m_image != nullptr && m_image->textureView != nullptr &&
            NLS::Core::ServiceLocator::Contains<UI::UIManager>())
        {
            NLS_SERVICE(UI::UIManager).ReleaseTextureViewHandle(m_image->textureView);
        }

        if (m_image != nullptr)
            m_image->textureView.reset();
    }

    m_lastResolvedViewSize = { p_width, p_height };
    m_fbo.Resize(p_width, p_height);
    m_image->textureView = m_fbo.GetOrCreateExplicitColorView("Editor.AView.Output");
    m_pendingResolvedViewSize.reset();
    return true;
}

void Editor::Panels::AView::UpdatePreRenderOverlayCameraMatrices()
{
    auto* camera = GetCamera();
    if (camera == nullptr)
        return;

    if (m_lastResolvedViewSize.first > 0u && m_lastResolvedViewSize.second > 0u)
        camera->CacheMatrices(m_lastResolvedViewSize.first, m_lastResolvedViewSize.second);

    auto* driver = Render::Context::TryGetLocatedDriver();
    const bool threadedRendering =
        driver != nullptr &&
        Render::Context::DriverRendererAccess::IsThreadedRenderingEnabled(*driver);
    const bool delayOverlayMatrices = Editor::Panels::ShouldDelayRetirementAwareViewOverlayMatrices(
        RequiresRetiredFrameConsumption(),
        RequiresImmediateRetiredFrameReadback(),
        threadedRendering);
    if (delayOverlayMatrices && m_overlayCameraMatricesForCurrentDraw.has_value())
        return;

    ViewOverlayCameraMatrices currentMatrices;
    currentMatrices.view = camera->GetViewMatrix();
    currentMatrices.projection = camera->GetProjectionMatrix();
    m_overlayCameraMatricesForCurrentDraw = currentMatrices;
}

void Editor::Panels::AView::BeginViewportOverlayDrawListChannels()
{
    auto* drawList = ImGui::GetWindowDrawList();
    if (drawList == nullptr || m_viewportOverlayDrawSplitter == nullptr)
        return;

    m_viewportOverlayDrawSplitter->Split(drawList, 2);
    m_viewportOverlayDrawSplitter->SetCurrentChannel(drawList, 1);
    m_viewportOverlayDrawListChannelsActive = true;
}

void Editor::Panels::AView::FinishPreRenderViewportOverlayDrawList()
{
    if (!m_viewportOverlayDrawListChannelsActive || m_viewportOverlayDrawSplitter == nullptr)
        return;

    if (auto* drawList = ImGui::GetWindowDrawList())
        m_viewportOverlayDrawSplitter->SetCurrentChannel(drawList, 0);
}

void Editor::Panels::AView::EndViewportOverlayDrawListChannels()
{
    if (!m_viewportOverlayDrawListChannelsActive || m_viewportOverlayDrawSplitter == nullptr)
        return;

    if (auto* drawList = ImGui::GetWindowDrawList())
        m_viewportOverlayDrawSplitter->Merge(drawList);

    m_viewportOverlayDrawListChannelsActive = false;
}

void Editor::Panels::AView::UpdateSubmittedOverlayCameraMatrices(
    const ViewOverlayCameraMatrices& submittedMatrices,
    const bool threadedRendering,
    const bool framePublished,
    const uint64_t latestPublishedFrameId,
    const uint64_t latestRetiredFrameId)
{
    const bool delayOverlayMatrices = Editor::Panels::ShouldDelayRetirementAwareViewOverlayMatrices(
        RequiresRetiredFrameConsumption(),
        RequiresImmediateRetiredFrameReadback(),
        threadedRendering);
    if (framePublished)
    {
        auto storedMatrices = submittedMatrices;
        storedMatrices.frameId = threadedRendering ? latestPublishedFrameId : 0u;
        m_submittedOverlayCameraMatrices.push_back(storedMatrices);
        constexpr size_t kMaxOverlayCameraMatrixHistory = 8u;
        while (m_submittedOverlayCameraMatrices.size() > kMaxOverlayCameraMatrixHistory)
            m_submittedOverlayCameraMatrices.pop_front();
    }

    if (!delayOverlayMatrices)
    {
        if (framePublished && !m_submittedOverlayCameraMatrices.empty())
            m_overlayCameraMatricesForCurrentDraw = m_submittedOverlayCameraMatrices.back();
        return;
    }

    if (m_submittedOverlayCameraMatrices.empty())
    {
        m_overlayCameraMatricesForCurrentDraw = submittedMatrices;
        return;
    }

    auto selectedMatrices = m_submittedOverlayCameraMatrices.front();
    for (const auto& candidateMatrices : m_submittedOverlayCameraMatrices)
    {
        if (candidateMatrices.frameId > latestRetiredFrameId)
            break;
        selectedMatrices = candidateMatrices;
    }
    m_overlayCameraMatricesForCurrentDraw = selectedMatrices;
}

Editor::Panels::ViewOverlayCameraMatrices Editor::Panels::AView::GetViewportOverlayCameraMatrices() const
{
    if (m_overlayCameraMatricesForCurrentDraw.has_value())
        return m_overlayCameraMatricesForCurrentDraw.value();

    if (!m_submittedOverlayCameraMatrices.empty())
        return m_submittedOverlayCameraMatrices.back();

    return {};
}

Maths::Vector2 Editor::Panels::AView::GetCurrentViewportImageMin() const
{
    return {
        ImGui::GetCursorScreenPos().x,
        ImGui::GetCursorScreenPos().y
    };
}

Maths::Vector2 Editor::Panels::AView::GetCurrentViewportImageMax() const
{
    const auto imageMin = GetCurrentViewportImageMin();
    return {
        imageMin.x + static_cast<float>(m_lastResolvedViewSize.first),
        imageMin.y + static_cast<float>(m_lastResolvedViewSize.second)
    };
}

std::pair<uint16_t, uint16_t> Editor::Panels::AView::GetSafeSize() const
{
	if (m_lastResolvedViewSize.first > 0u || m_lastResolvedViewSize.second > 0u)
		return m_lastResolvedViewSize;

	constexpr float kTitleBarHeight = 25.0f; // <--- this takes into account the imgui window title bar
	const auto& size = GetSize();
	return {
		static_cast<uint16_t>(size.x),
		static_cast<uint16_t>(std::max(0.0f, size.y - kTitleBarHeight)) // <--- clamp to prevent the output size to be negative
	}; 
}

const Engine::Rendering::BaseSceneRenderer& Editor::Panels::AView::GetRenderer() const
{
	return *m_renderer.get();
}

const std::optional<Render::Data::FrameInfo>& Editor::Panels::AView::GetLastRenderedFrameInfoSnapshot() const
{
    return m_lastRenderedFrameInfo;
}

bool Editor::Panels::AView::IsMouseWithinView(const Maths::Vector2& mousePosition) const
{
    if (m_image == nullptr || !m_image->HasLastDrawBounds())
        return false;

    const auto& min = m_image->GetLastDrawMin();
    const auto& max = m_image->GetLastDrawMax();
    return mousePosition.x >= min.x && mousePosition.x < max.x &&
        mousePosition.y >= min.y && mousePosition.y < max.y;
}

std::optional<Maths::Vector2> Editor::Panels::AView::GetLocalViewPosition(const Maths::Vector2& mousePosition) const
{
    if (m_image == nullptr || !m_image->HasLastDrawBounds() || !IsMouseWithinView(mousePosition))
        return std::nullopt;

    const auto& min = m_image->GetLastDrawMin();
    const auto& max = m_image->GetLastDrawMax();
    Maths::Vector2 localPosition {
        mousePosition.x - min.x,
        mousePosition.y - min.y
    };

    localPosition.x = std::clamp(localPosition.x, 0.0f, std::max(0.0f, max.x - min.x));
    localPosition.y = std::clamp(localPosition.y, 0.0f, std::max(0.0f, max.y - min.y));
    return localPosition;
}

bool Editor::Panels::AView::HasViewportImageBounds() const
{
    return m_image != nullptr && m_image->HasLastDrawBounds();
}

Maths::Vector2 Editor::Panels::AView::GetViewportImageMin() const
{
    return HasViewportImageBounds() ? m_image->GetLastDrawMin() : Maths::Vector2 {};
}

Maths::Vector2 Editor::Panels::AView::GetViewportImageMax() const
{
    return HasViewportImageBounds() ? m_image->GetLastDrawMax() : Maths::Vector2 {};
}

Engine::Rendering::BaseSceneRenderer::SceneDescriptor Editor::Panels::AView::CreateSceneDescriptor()
{
	auto scene = GetScene();

	NLS_ASSERT(scene, "No scene assigned to this view!");

	return {
		*scene
	};
}
