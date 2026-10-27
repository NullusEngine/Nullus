#include "Components/UICanvasComponent.h"

#include "Engine/UI/UIDocumentSourceResolver.h"

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
        return static_cast<uint32_t>(renderModeValue);
    }

    void UICanvasComponent::SetWorldRenderPathValue(const uint32_t value)
    {
        if (value <= static_cast<uint32_t>(WorldRenderPath::RenderTarget))
            SetWorldRenderPath(static_cast<WorldRenderPath>(value));
    }

    uint32_t UICanvasComponent::GetWorldRenderPathValue() const
    {
        return static_cast<uint32_t>(worldRenderPathValue);
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
        if (dpiScaleMode != DpiScaleMode::ScaleWithScreenSize ||
            referenceResolution.x <= 0.0f || referenceResolution.y <= 0.0f ||
            viewportSize.x <= 0.0f || viewportSize.y <= 0.0f)
        {
            return baseScale;
        }

        const float widthScale = std::max(0.0001f, viewportSize.x / referenceResolution.x);
        const float heightScale = std::max(0.0001f, viewportSize.y / referenceResolution.y);
        const float match = std::clamp(dpiMatch, 0.0f, 1.0f);
        return baseScale * std::exp(
            std::log(widthScale) * (1.0f - match) + std::log(heightScale) * match);
    }

    void UICanvasComponent::OnCreate()
    {
        if (!m_document && !document.GetResourcePath().empty())
            (void)LoadDocument(document.GetResourcePath(), nullptr);
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

    bool UICanvasComponent::LoadDocumentResolvedPathAsReference(
        const std::string& resolvedPath,
        UI::UIDocumentReference reference,
        std::vector<UI::UIDocumentDiagnostic>* diagnostics)
    {
        if (reference.IsNull())
            return false;

        // LoadDocumentFile updates the live document only after parsing has
        // succeeded. Commit the serialized reference afterwards so a failed
        // picker assignment leaves both the old document and old path intact.
        if (!LoadDocumentFile(resolvedPath, false, diagnostics))
            return false;

        document = std::move(reference);
        return true;
    }

    bool UICanvasComponent::LoadDocumentResolvedPathAsReference(
        const std::string& resolvedPath,
        const std::string& logicalPath,
        std::vector<UI::UIDocumentDiagnostic>* diagnostics)
    {
        return LoadDocumentResolvedPathAsReference(
            resolvedPath,
            UI::UIDocumentReference({}, logicalPath),
            diagnostics);
    }

    bool UICanvasComponent::LoadDocumentReference(
        const UI::UIDocumentReference& reference,
        std::vector<UI::UIDocumentDiagnostic>* diagnostics)
    {
        const auto sourcePath = UI::ResolveUIDocumentSourcePath(reference);
        if (sourcePath.empty())
        {
            if (diagnostics != nullptr)
            {
                UI::UIDocumentDiagnostic diagnostic;
                diagnostic.path = reference.GetResourcePath();
                diagnostic.message =
                    "UI document source could not be resolved; the reference is kept for a later reload";
                diagnostics->push_back(std::move(diagnostic));
            }
            return false;
        }

        auto loadedDocument = UI::UIDocument::LoadFromFile(sourcePath, diagnostics);
        if (!loadedDocument.has_value())
            return false;
        SetDocument(std::make_shared<UI::UIDocument>(std::move(*loadedDocument)));
        return true;
    }

    bool UICanvasComponent::LoadDocumentFile(
        const std::string& path,
        const bool updateSerializedPath,
        std::vector<UI::UIDocumentDiagnostic>* diagnostics)
    {
        if (!LoadDocumentReference(UI::UIDocumentReference({}, path), diagnostics))
            return false;
        if (updateSerializedPath)
            document = UI::UIDocumentReference({}, path);
        return true;
    }

    void UICanvasComponent::SetDocumentPath(std::string path)
    {
        SetDocumentReference(UI::UIDocumentReference({}, std::move(path)));
    }

    void UICanvasComponent::SetDocumentReference(UI::UIDocumentReference reference)
    {
        if (reference.IsNull())
        {
            document.Reset();
            SetDocument(nullptr);
            return;
        }

        if (reference == document && m_document)
            return;

        // An import or asset database rebuild can temporarily leave the source
        // unavailable. Preserve the stable reference and only clear the live
        // document in that case so a later reload can restore it.
        if (LoadDocumentReference(reference, nullptr))
        {
            document = std::move(reference);
            return;
        }

        document = std::move(reference);
        SetDocument(nullptr);
    }

    bool UICanvasComponent::LoadDocumentArtifact(
        const std::string& path,
        std::vector<UI::UIDocumentDiagnostic>* diagnostics)
    {
        auto loadedDocument = UI::UIDocument::LoadFromArtifactFile(path, diagnostics);
        if (!loadedDocument.has_value())
            return false;
        document = UI::UIDocumentReference({}, path);
        SetDocument(std::make_shared<UI::UIDocument>(std::move(*loadedDocument)));
        return true;
    }

    bool UICanvasComponent::LoadDocumentJson(
        const std::string_view json,
        std::vector<UI::UIDocumentDiagnostic>* diagnostics)
    {
        auto loadedDocument = UI::UIDocument::LoadFromJson(json, diagnostics);
        if (!loadedDocument.has_value())
            return false;
        document.Reset();
        SetDocument(std::make_shared<UI::UIDocument>(std::move(*loadedDocument)));
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
        if (worldRenderPathValue != WorldRenderPath::Auto)
            return worldRenderPathValue;
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
        if (updatePolicy != UpdatePolicy::Manual)
            m_instance->UpdateLayout(viewportSize, m_lastDpiScale);
        if (updatePolicy == UpdatePolicy::EveryFrame)
            m_instance->Tick(deltaSeconds);
        else
            m_instance->RefreshResources();
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
