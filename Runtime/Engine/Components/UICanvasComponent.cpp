#include "Components/UICanvasComponent.h"

#include <cmath>
#include <utility>

namespace NLS::Engine::Components
{
    namespace
    {
        uint64_t MakeElementHandle(const UI::UIElement& element)
        {
            const auto instanceId = static_cast<uint32_t>(element.GetInstanceID());
            if (instanceId == 0u)
                return 0u;
            return instanceId;
        }
    }

    void UICanvasComponent::SetRenderModeValue(const uint32_t value)
    {
        if (value <= static_cast<uint32_t>(RenderMode::World))
            SetRenderMode(static_cast<RenderMode>(value));
    }

    uint32_t UICanvasComponent::GetRenderModeValue() const
    {
        return static_cast<uint32_t>(m_renderMode);
    }

    void UICanvasComponent::SetWorldRenderPathValue(const uint32_t value)
    {
        if (value <= static_cast<uint32_t>(WorldRenderPath::RenderTarget))
            SetWorldRenderPath(static_cast<WorldRenderPath>(value));
    }

    uint32_t UICanvasComponent::GetWorldRenderPathValue() const
    {
        return static_cast<uint32_t>(m_worldRenderPath);
    }

    uint32_t UICanvasComponent::GetDocumentGeneration() const
    {
        return m_instance ? m_instance->GetGeneration() : 0u;
    }

    uint64_t UICanvasComponent::GetElementHandle(const std::string& name) const
    {
        const auto* element = m_instance ? m_instance->GetElementByName(name) : nullptr;
        if (!element || !m_instance)
            return 0u;
        return (static_cast<uint64_t>(m_instance->GetGeneration()) << 32u) |
            MakeElementHandle(*element);
    }

    UI::UIElement* UICanvasComponent::ResolveElementHandle(const uint64_t handle) const
    {
        if (!m_instance || handle == 0u)
            return nullptr;

        const auto instanceId = static_cast<NLS::InstanceID>(static_cast<uint32_t>(handle & 0xffffffffu));
        const auto generation = static_cast<uint32_t>(handle >> 32u);
        if (instanceId == NLS::InstanceID_None || generation == 0u ||
            generation != m_instance->GetGeneration())
            return nullptr;

        auto* element = NLS::Object::IDToPointer<UI::UIElement>(instanceId);
        // The high word is the document-instance generation.  UIElement has
        // its own subtree lifetime generation, so comparing the two would
        // reject every freshly resolved handle after a document reload.
        if (!element)
            return nullptr;

        for (auto* current = element; current != nullptr; current = current->GetParent())
        {
            if (current == m_instance->GetRoot())
                return element;
        }
        return nullptr;
    }

    bool UICanvasComponent::SetElementText(const uint64_t handle, const std::string& text)
    {
        if (auto* element = ResolveElementHandle(handle))
        {
            element->SetText(text);
            return true;
        }
        return false;
    }

    std::string UICanvasComponent::GetElementText(const uint64_t handle) const
    {
        if (const auto* element = ResolveElementHandle(handle))
            return element->GetText();
        return {};
    }

    bool UICanvasComponent::SetElementValue(const uint64_t handle, const float value)
    {
        if (auto* element = ResolveElementHandle(handle))
        {
            element->SetValue(value);
            return true;
        }
        return false;
    }

    float UICanvasComponent::GetElementValue(const uint64_t handle) const
    {
        if (const auto* element = ResolveElementHandle(handle))
            return element->GetValue();
        return 0.0f;
    }

    bool UICanvasComponent::SetElementChecked(const uint64_t handle, const bool checked)
    {
        if (auto* element = ResolveElementHandle(handle))
        {
            element->SetChecked(checked);
            return true;
        }
        return false;
    }

    bool UICanvasComponent::GetElementChecked(const uint64_t handle) const
    {
        if (const auto* element = ResolveElementHandle(handle))
            return element->IsChecked();
        return false;
    }

    bool UICanvasComponent::FocusElement(const uint64_t handle)
    {
        if (auto* element = ResolveElementHandle(handle))
        {
            m_inputRouter.Focus(element);
            return true;
        }
        return false;
    }

    NLS::Engine::UI::UISignalSubscriptionHandle UICanvasComponent::ConnectSignal(
        std::string name,
        NLS::Engine::UI::UISignalBus::Handler handler)
    {
        return m_instance ? m_instance->Signals().Connect(std::move(name), std::move(handler)) :
            NLS::Engine::UI::UISignalSubscriptionHandle {};
    }

    bool UICanvasComponent::DisconnectSignal(
        const NLS::Engine::UI::UISignalSubscriptionHandle handle)
    {
        return m_instance && m_instance->Signals().Disconnect(handle);
    }

    uint32_t UICanvasComponent::EmitSignal(
        const std::string& name,
        const std::vector<UI::UIValue>& arguments)
    {
        return m_instance ? m_instance->Signals().Emit(name, arguments) : 0u;
    }

    float UICanvasComponent::ComputeEffectiveScale(
        const UI::UIVector2 viewportSize,
        const float dpiScale) const
    {
        const float baseScale = dpiScale > 0.0f ? dpiScale : 1.0f;
        if (m_dpiScaleMode != DpiScaleMode::ScaleWithScreenSize ||
            m_referenceResolution.x <= 0.0f || m_referenceResolution.y <= 0.0f ||
            viewportSize.x <= 0.0f || viewportSize.y <= 0.0f)
        {
            return baseScale;
        }

        const float widthScale = std::max(0.0001f, viewportSize.x / m_referenceResolution.x);
        const float heightScale = std::max(0.0001f, viewportSize.y / m_referenceResolution.y);
        const float match = std::clamp(m_dpiMatch, 0.0f, 1.0f);
        return baseScale * std::exp(
            std::log(widthScale) * (1.0f - match) + std::log(heightScale) * match);
    }

    void UICanvasComponent::OnCreate()
    {
        if (!m_document && !m_documentPath.empty())
            (void)LoadDocument(m_documentPath, nullptr);
        m_inputRouter.SetRoot(m_instance ? m_instance->GetRoot() : nullptr);
    }

    void UICanvasComponent::OnDestroy()
    {
        m_inputRouter.Clear();
        m_instance.reset();
        m_document.reset();
    }

    bool UICanvasComponent::LoadDocument(
        const std::string& path,
        std::vector<UI::UIDocumentDiagnostic>* diagnostics)
    {
        return LoadDocumentFile(path, true, diagnostics);
    }

    bool UICanvasComponent::LoadDocumentResolvedPath(
        const std::string& path,
        std::vector<UI::UIDocumentDiagnostic>* diagnostics)
    {
        return LoadDocumentFile(path, false, diagnostics);
    }

    bool UICanvasComponent::LoadDocumentFile(
        const std::string& path,
        const bool updateSerializedPath,
        std::vector<UI::UIDocumentDiagnostic>* diagnostics)
    {
        auto document = UI::UIDocument::LoadFromFile(path, diagnostics);
        if (!document.has_value())
            return false;
        if (updateSerializedPath)
            m_documentPath = path;
        SetDocument(std::make_shared<UI::UIDocument>(std::move(*document)));
        return true;
    }

    void UICanvasComponent::SetDocumentPath(std::string path)
    {
        if (path.empty())
        {
            m_documentPath.clear();
            SetDocument(nullptr);
            return;
        }

        if (path == m_documentPath && m_document)
            return;

        // Preserve the reference even when a source asset is temporarily
        // unavailable. This lets asset import/reload restore the Canvas later.
        if (!LoadDocument(path, nullptr))
        {
            m_documentPath = std::move(path);
            SetDocument(nullptr);
        }
    }

    bool UICanvasComponent::LoadDocumentArtifact(
        const std::string& path,
        std::vector<UI::UIDocumentDiagnostic>* diagnostics)
    {
        auto document = UI::UIDocument::LoadFromArtifactFile(path, diagnostics);
        if (!document.has_value())
            return false;
        m_documentPath = path;
        SetDocument(std::make_shared<UI::UIDocument>(std::move(*document)));
        return true;
    }

    bool UICanvasComponent::LoadDocumentJson(
        const std::string_view json,
        std::vector<UI::UIDocumentDiagnostic>* diagnostics)
    {
        auto document = UI::UIDocument::LoadFromJson(json, diagnostics);
        if (!document.has_value())
            return false;
        m_documentPath.clear();
        SetDocument(std::make_shared<UI::UIDocument>(std::move(*document)));
        return true;
    }

    void UICanvasComponent::SetDocument(std::shared_ptr<const UI::UIDocument> document)
    {
        if (m_instance && m_document && document &&
            m_document->GetDocumentId() == document->GetDocumentId())
        {
            std::optional<UI::UINodeGuid> focusedGuid;
            if (const auto* focused = m_inputRouter.GetFocusedElement())
                focusedGuid = focused->GetNodeGuid();
            // Reload replaces the element tree. Clear all pointer/focus
            // references before the old Object instances are destroyed, then
            // restore focus by stable NodeGuid on the new tree.
            auto* oldRoot = m_instance->GetRoot();
            m_inputRouter.Clear();
            if (m_instance->Reload(*document))
            {
                m_document = std::move(document);
                m_inputRouter.SetRoot(m_instance->GetRoot());
                if (focusedGuid)
                {
                    if (auto* restored = m_instance->GetElement(*focusedGuid))
                        m_inputRouter.Focus(restored);
                }
                return;
            }
            // A failed reload leaves the old tree intact. Restore the router
            // root so the caller can continue using the existing document.
            m_inputRouter.SetRoot(oldRoot);
            if (focusedGuid)
            {
                if (auto* restored = m_instance->GetElement(*focusedGuid))
                    m_inputRouter.Focus(restored);
            }
        }
        m_inputRouter.Clear();
        m_document = std::move(document);
        m_instance = m_document ? m_document->Instantiate() : nullptr;
        m_inputRouter.SetRoot(m_instance ? m_instance->GetRoot() : nullptr);
        if (m_instance && m_lastViewport.x > 0.0f && m_lastViewport.y > 0.0f)
            m_instance->UpdateLayout(m_lastViewport, m_lastDpiScale);
    }

    UICanvasComponent::WorldRenderPath UICanvasComponent::GetResolvedWorldRenderPath() const
    {
        if (m_worldRenderPath != WorldRenderPath::Auto)
            return m_worldRenderPath;
        return m_instance && m_instance->RequiresRenderTarget()
            ? WorldRenderPath::RenderTarget
            : WorldRenderPath::Direct;
    }

    void UICanvasComponent::Tick(
        const float deltaSeconds,
        const UI::UIVector2 viewportSize,
        const float dpiScale,
        const uint64_t frameId)
    {
        if (!m_instance || !IsRenderable())
            return;
        m_lastViewport = viewportSize;
        m_lastDpiScale = dpiScale > 0.0f ? dpiScale : 1.0f;
        m_lastFrameId = frameId;
        if (m_updatePolicy != UpdatePolicy::Manual)
            m_instance->UpdateLayout(viewportSize, m_lastDpiScale);
        if (m_updatePolicy == UpdatePolicy::EveryFrame)
            m_instance->Tick(deltaSeconds);
    }

    std::shared_ptr<NLS::Render::UI::UIDrawSnapshot> UICanvasComponent::BuildDrawSnapshot(
        const uint64_t frameId,
        const uint32_t viewportWidth,
        const uint32_t viewportHeight,
        const float dpiScale) const
    {
        if (!m_instance || !IsRenderable())
            return nullptr;
        return m_instance->BuildDrawSnapshot(frameId, viewportWidth, viewportHeight, dpiScale);
    }
}
