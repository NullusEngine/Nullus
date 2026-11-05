#pragma once

#include "Components/Component.h"
#include "EngineDef.h"
#include "Reflection/Macros.h"
#include "Engine/UI/UIDocument.h"
#include "Engine/UI/UIDocumentReference.h"
#include "Engine/UI/UIInputRouter.h"
#include "Engine/UI/UISignal.h"
#include "Math/Vector2.h"
#include "Components/UICanvasComponent.generated.h"

#include <cstdint>
#include <algorithm>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace NLS::Engine::Components
{
    CLASS(NLS_ENGINE_API UICanvasComponent, Scriptable, ComponentMenu("UI/Canvas"), DisallowMultipleComponent) : public Component
    {
    public:
        GENERATED_BODY()

        ENUM(RenderMode) : uint8_t
        {
            ScreenOverlay,
            ScreenCamera,
            World
        };

        ENUM(WorldRenderPath) : uint8_t
        {
            Auto,
            Direct,
            RenderTarget
        };

        ENUM(UpdatePolicy) : uint8_t
        {
            EveryFrame,
            OnDemand,
            Manual
        };

        ENUM(DpiScaleMode) : uint8_t
        {
            ConstantPixelSize,
            ScaleWithScreenSize,
            ConstantPhysicalSize
        };

        UICanvasComponent() = default;
        ~UICanvasComponent() override = default;

        void OnCreate() override;
        void OnDestroy() override;

        bool LoadDocument(const std::string& path, std::vector<UI::UIDocumentDiagnostic>* diagnostics = nullptr);
        // Loads a resolved development source path without replacing the
        // serialized Assets-relative document reference.
        bool LoadDocumentResolvedPath(const std::string& path, std::vector<UI::UIDocumentDiagnostic>* diagnostics = nullptr);
        // Loads a resolved source path and commits the logical Assets-relative
        // reference only after the document has parsed successfully. This is
        // used by editor object pickers so a failed load never clears a valid
        // document or leaves a half-assigned reference behind.
        bool LoadDocumentResolvedPathAsReference(
            const std::string& resolvedPath,
            UI::UIDocumentReference reference,
            std::vector<UI::UIDocumentDiagnostic>* diagnostics = nullptr);
        // Compatibility overload for runtime callers that still only have a
        // logical path. It is deliberately not a reflected property.
        bool LoadDocumentResolvedPathAsReference(
            const std::string& resolvedPath,
            const std::string& logicalPath,
            std::vector<UI::UIDocumentDiagnostic>* diagnostics = nullptr);
        bool LoadDocumentArtifact(const std::string& path, std::vector<UI::UIDocumentDiagnostic>* diagnostics = nullptr);
        bool LoadDocumentJson(std::string_view json, std::vector<UI::UIDocumentDiagnostic>* diagnostics = nullptr);
        void SetDocument(std::shared_ptr<const UI::UIDocument> document);
        FUNCTION()
        UI::UIDocumentReference GetDocumentReference() const { return document; }
        FUNCTION()
        void SetDocumentReference(UI::UIDocumentReference reference);
        // Legacy runtime API. It is no longer a reflected or serialized string
        // field, and exists only for callers that have not moved to references.
        [[nodiscard]] std::string GetDocumentPath() const { return document.GetResourcePath(); }
        void SetDocumentPath(std::string path);
        [[nodiscard]] const UI::UIDocument* GetDocument() const { return m_document.get(); }
        [[nodiscard]] UI::UIDocumentInstance* GetInstance() const { return m_instance.get(); }

        void Tick(float deltaSeconds, UI::UIVector2 viewportSize, float dpiScale, uint64_t frameId);
        [[nodiscard]] std::shared_ptr<NLS::Render::UI::UIDrawSnapshot> BuildDrawSnapshot(
            uint64_t frameId,
            uint32_t viewportWidth,
            uint32_t viewportHeight,
            float dpiScale) const;

        FUNCTION() void SetRenderMode(RenderMode mode) { renderModeValue = mode; }
        FUNCTION() RenderMode GetRenderMode() const { return renderModeValue; }
        FUNCTION(Scriptable)
        void SetTargetDisplay(uint32_t value) { targetDisplay=std::min(value,7u); }
        FUNCTION(Scriptable)
        uint32_t GetTargetDisplay() const { return targetDisplay; }
        FUNCTION() void SetWorldRenderPath(WorldRenderPath path) { worldRenderPathValue = path; }
        FUNCTION() WorldRenderPath GetWorldRenderPath() const { return worldRenderPathValue; }
        [[nodiscard]] WorldRenderPath GetResolvedWorldRenderPath() const;
        void SetSortOrder(int32_t order) { sortOrderScript = order; }
        int32_t GetSortOrder() const { return sortOrderScript; }
        FUNCTION(Scriptable)
        void SetSortLayer(int32_t layer) { sortLayer = layer; }
        FUNCTION(Scriptable)
        int32_t GetSortLayer() const { return sortLayer; }
        FUNCTION(Scriptable)
        void SetCameraId(uint64_t cameraId) { this->cameraId = cameraId; }
        FUNCTION(Scriptable)
        uint64_t GetCameraId() const { return cameraId; }
        void SetReferenceResolution(UI::UIVector2 resolution) { referenceResolution = resolution; }
        [[nodiscard]] UI::UIVector2 GetReferenceResolution() const { return referenceResolution; }
        FUNCTION() void SetUpdatePolicy(UpdatePolicy policy) { updatePolicy = policy; }
        FUNCTION() UpdatePolicy GetUpdatePolicy() const { return updatePolicy; }
        FUNCTION() void SetDpiScaleMode(DpiScaleMode mode) { dpiScaleMode = mode; }
        FUNCTION() DpiScaleMode GetDpiScaleMode() const { return dpiScaleMode; }
        FUNCTION(Scriptable)
        void SetDpiMatch(float match) { dpiMatch = std::clamp(match, 0.0f, 1.0f); }
        FUNCTION(Scriptable)
        float GetDpiMatch() const { return dpiMatch; }
        void SetWorldSize(UI::UIVector2 size) { worldSize = size; }
        [[nodiscard]] UI::UIVector2 GetWorldSize() const { return worldSize; }
        FUNCTION()
        NLS::Maths::Vector2 GetReferenceResolutionInspector() const
        {
            return {referenceResolution.x, referenceResolution.y};
        }
        FUNCTION()
        void SetReferenceResolutionInspector(NLS::Maths::Vector2 resolution)
        {
            referenceResolution = {resolution.x, resolution.y};
        }
        FUNCTION()
        NLS::Maths::Vector2 GetWorldSizeInspector() const
        {
            return {worldSize.x, worldSize.y};
        }
        FUNCTION()
        void SetWorldSizeInspector(NLS::Maths::Vector2 size)
        {
            worldSize = {size.x, size.y};
        }
        FUNCTION(Scriptable)
        void SetPixelsPerUnit(float value) { pixelsPerUnit = std::max(0.001f, value); }
        FUNCTION(Scriptable)
        float GetPixelsPerUnit() const { return pixelsPerUnit; }
        FUNCTION(Scriptable)
        void SetDepthTest(bool enabled) { depthTest = enabled; }
        FUNCTION(Scriptable)
        bool GetDepthTest() const { return depthTest; }
        FUNCTION(Scriptable)
        void SetDepthWrite(bool enabled) { depthWrite = enabled; }
        FUNCTION(Scriptable)
        bool GetDepthWrite() const { return depthWrite; }
        FUNCTION(Scriptable)
        void SetAlwaysOnTop(bool enabled) { alwaysOnTop = enabled; }
        FUNCTION(Scriptable)
        bool GetAlwaysOnTop() const { return alwaysOnTop; }

        // Script-facing enum bridge. The native API keeps strongly typed
        // enums while the portable manifest transports their uint32 values.
        FUNCTION(Scriptable)
        void SetRenderModeValue(uint32_t value);
        FUNCTION(Scriptable)
        uint32_t GetRenderModeValue() const;
        FUNCTION(Scriptable)
        void SetWorldRenderPathValue(uint32_t value);
        FUNCTION(Scriptable)
        uint32_t GetWorldRenderPathValue() const;
        FUNCTION(Scriptable)
        void SetSortOrderScript(int32_t order) { SetSortOrder(order); }
        FUNCTION(Scriptable)
        int32_t GetSortOrderScript() const { return GetSortOrder(); }
        FUNCTION(Scriptable)
        bool LoadDocumentScript(const std::string& path) { return LoadDocument(path, nullptr); }
        FUNCTION(Scriptable)
        uint32_t GetDocumentGeneration() const;

        // Element handles are process-local and deliberately generation
        // checked. They are never serialized into a document or scene.
        FUNCTION(Scriptable)
        uint64_t GetElementHandle(const std::string& name) const;
        FUNCTION(Scriptable)
        bool SetElementText(uint64_t handle, const std::string& text);
        FUNCTION(Scriptable)
        std::string GetElementText(uint64_t handle) const;
        FUNCTION(Scriptable)
        bool SetElementValue(uint64_t handle, float value);
        FUNCTION(Scriptable)
        float GetElementValue(uint64_t handle) const;
        FUNCTION(Scriptable)
        bool SetElementChecked(uint64_t handle, bool checked);
        FUNCTION(Scriptable)
        bool GetElementChecked(uint64_t handle) const;
        FUNCTION(Scriptable)
        bool FocusElement(uint64_t handle);
        NLS::Engine::UI::UISignalSubscriptionHandle ConnectSignal(
            std::string name,
            NLS::Engine::UI::UISignalBus::Handler handler);
        bool DisconnectSignal(NLS::Engine::UI::UISignalSubscriptionHandle handle);
        uint32_t EmitSignal(
            const std::string& name,
            const std::vector<UI::UIValue>& arguments = {});

        [[nodiscard]] float ComputeEffectiveScale(UI::UIVector2 viewportSize, float dpiScale) const;

        [[nodiscard]] UI::UIInputRouter& GetInputRouter() { return m_inputRouter; }
        [[nodiscard]] const UI::UIInputRouter& GetInputRouter() const { return m_inputRouter; }
        [[nodiscard]] bool IsRenderable() const
        {
            return gameobject() == nullptr ? IsSelfEnabled() : IsActiveAndEnabled();
        }

    private:
        // Resolves the reference through the host source resolver (project
        // layout for the editor, asset packs for a built game) and only commits
        // the live document once parsing succeeded.
        bool LoadDocumentReference(
            const UI::UIDocumentReference& reference,
            std::vector<UI::UIDocumentDiagnostic>* diagnostics);
        bool LoadDocumentFile(
            const std::string& path,
            bool updateSerializedPath,
            std::vector<UI::UIDocumentDiagnostic>* diagnostics);
        [[nodiscard]] UI::UIElement* ResolveElementHandle(uint64_t handle) const;

        // Serialized scene property. The common ObjectReference drawer uses
        // this stable asset identity and resource path directly. Public
        // accessors below perform the live-document transition.
        PROPERTY(DisplayName("Document"), Getter(GetDocumentReference), Setter(SetDocumentReference))
        UI::UIDocumentReference document;
        std::shared_ptr<const UI::UIDocument> m_document;
        std::unique_ptr<UI::UIDocumentInstance> m_instance;
        UI::UIInputRouter m_inputRouter;
        PROPERTY(DisplayName("Render Mode"), Getter(GetRenderMode), Setter(SetRenderMode))
        RenderMode renderModeValue = RenderMode::ScreenOverlay;
        PROPERTY(DisplayName("Target Display (0-7)"), Getter(GetTargetDisplay), Setter(SetTargetDisplay))
        uint32_t targetDisplay=0;
        PROPERTY(DisplayName("World Render Path"), Getter(GetWorldRenderPath), Setter(SetWorldRenderPath))
        WorldRenderPath worldRenderPathValue = WorldRenderPath::Auto;
        PROPERTY(DisplayName("Update Policy"), Getter(GetUpdatePolicy), Setter(SetUpdatePolicy))
        UpdatePolicy updatePolicy = UpdatePolicy::EveryFrame;
        PROPERTY(DisplayName("DPI Scale Mode"), Getter(GetDpiScaleMode), Setter(SetDpiScaleMode))
        DpiScaleMode dpiScaleMode = DpiScaleMode::ConstantPixelSize;
        PROPERTY(DisplayName("DPI Match"), Getter(GetDpiMatch), Setter(SetDpiMatch))
        float dpiMatch = 0.5f;
        PROPERTY(DisplayName("Sort Order"), Getter(GetSortOrderScript), Setter(SetSortOrderScript))
        int32_t sortOrderScript = 0;
        PROPERTY(DisplayName("Sort Layer"), Getter(GetSortLayer), Setter(SetSortLayer))
        int32_t sortLayer = 0;
        PROPERTY(DisplayName("Camera Id"), Getter(GetCameraId), Setter(SetCameraId))
        uint64_t cameraId = 0u;
        PROPERTY(DisplayName("Reference Resolution"), Getter(GetReferenceResolutionInspector), Setter(SetReferenceResolutionInspector))
        UI::UIVector2 referenceResolution { 1920.0f, 1080.0f };
        PROPERTY(DisplayName("World Size"), Getter(GetWorldSizeInspector), Setter(SetWorldSizeInspector))
        UI::UIVector2 worldSize { 1.0f, 1.0f };
        PROPERTY(DisplayName("Pixels Per Unit"), Getter(GetPixelsPerUnit), Setter(SetPixelsPerUnit))
        float pixelsPerUnit = 100.0f;
        PROPERTY(DisplayName("Depth Test"), Getter(GetDepthTest), Setter(SetDepthTest))
        bool depthTest = true;
        PROPERTY(DisplayName("Depth Write"), Getter(GetDepthWrite), Setter(SetDepthWrite))
        bool depthWrite = false;
        PROPERTY(DisplayName("Always On Top"), Getter(GetAlwaysOnTop), Setter(SetAlwaysOnTop))
        bool alwaysOnTop = false;
        UI::UIVector2 m_lastViewport;
        float m_lastDpiScale = 1.0f;
        uint64_t m_lastFrameId = 0u;
    };
}
