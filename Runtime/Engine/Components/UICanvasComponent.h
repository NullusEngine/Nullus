#pragma once

#include "Components/Component.h"
#include "EngineDef.h"
#include "Reflection/Macros.h"
#include "Engine/UI/UIDocument.h"
#include "Engine/UI/UIInputRouter.h"
#include "Engine/UI/UISignal.h"
#include "Components/UICanvasComponent.generated.h"

#include <cstdint>
#include <algorithm>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace NLS::Engine::Components
{
    CLASS(NLS_ENGINE_API UICanvasComponent, Scriptable, ComponentMenu("UI/Canvas")) : public Component
    {
    public:
        GENERATED_BODY()

        enum class RenderMode : uint8_t
        {
            ScreenOverlay,
            ScreenCamera,
            World
        };

        enum class WorldRenderPath : uint8_t
        {
            Auto,
            Direct,
            RenderTarget
        };

        enum class UpdatePolicy : uint8_t
        {
            EveryFrame,
            OnDemand,
            Manual
        };

        enum class DpiScaleMode : uint8_t
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
        bool LoadDocumentArtifact(const std::string& path, std::vector<UI::UIDocumentDiagnostic>* diagnostics = nullptr);
        bool LoadDocumentJson(std::string_view json, std::vector<UI::UIDocumentDiagnostic>* diagnostics = nullptr);
        void SetDocument(std::shared_ptr<const UI::UIDocument> document);
        // Serialized scene property. The editor stores an Assets-relative path,
        // while explicit artifact loading remains available for packaged builds.
        PROPERTY(documentPath)
        FUNCTION()
        std::string GetDocumentPath() const { return m_documentPath; }
        PROPERTY(documentPath)
        FUNCTION()
        void SetDocumentPath(std::string path);
        [[nodiscard]] const UI::UIDocument* GetDocument() const { return m_document.get(); }
        [[nodiscard]] UI::UIDocumentInstance* GetInstance() const { return m_instance.get(); }

        void Tick(float deltaSeconds, UI::UIVector2 viewportSize, float dpiScale, uint64_t frameId);
        [[nodiscard]] std::shared_ptr<NLS::Render::UI::UIDrawSnapshot> BuildDrawSnapshot(
            uint64_t frameId,
            uint32_t viewportWidth,
            uint32_t viewportHeight,
            float dpiScale) const;

        void SetRenderMode(RenderMode mode) { m_renderMode = mode; }
        RenderMode GetRenderMode() const { return m_renderMode; }
        void SetWorldRenderPath(WorldRenderPath path) { m_worldRenderPath = path; }
        WorldRenderPath GetWorldRenderPath() const { return m_worldRenderPath; }
        [[nodiscard]] WorldRenderPath GetResolvedWorldRenderPath() const;
        void SetSortOrder(int32_t order) { m_sortOrder = order; }
        int32_t GetSortOrder() const { return m_sortOrder; }
        FUNCTION(Scriptable)
        void SetSortLayer(int32_t layer) { m_sortLayer = layer; }
        FUNCTION(Scriptable)
        int32_t GetSortLayer() const { return m_sortLayer; }
        FUNCTION(Scriptable)
        void SetCameraId(uint64_t cameraId) { m_cameraId = cameraId; }
        FUNCTION(Scriptable)
        uint64_t GetCameraId() const { return m_cameraId; }
        void SetReferenceResolution(UI::UIVector2 resolution) { m_referenceResolution = resolution; }
        [[nodiscard]] UI::UIVector2 GetReferenceResolution() const { return m_referenceResolution; }
        void SetUpdatePolicy(UpdatePolicy policy) { m_updatePolicy = policy; }
        UpdatePolicy GetUpdatePolicy() const { return m_updatePolicy; }
        void SetDpiScaleMode(DpiScaleMode mode) { m_dpiScaleMode = mode; }
        [[nodiscard]] DpiScaleMode GetDpiScaleMode() const { return m_dpiScaleMode; }
        FUNCTION(Scriptable)
        void SetDpiMatch(float match) { m_dpiMatch = std::clamp(match, 0.0f, 1.0f); }
        FUNCTION(Scriptable)
        float GetDpiMatch() const { return m_dpiMatch; }
        void SetWorldSize(UI::UIVector2 size) { m_worldSize = size; }
        [[nodiscard]] UI::UIVector2 GetWorldSize() const { return m_worldSize; }
        FUNCTION(Scriptable)
        void SetPixelsPerUnit(float value) { m_pixelsPerUnit = std::max(0.001f, value); }
        FUNCTION(Scriptable)
        float GetPixelsPerUnit() const { return m_pixelsPerUnit; }
        FUNCTION(Scriptable)
        void SetDepthTest(bool enabled) { m_depthTest = enabled; }
        FUNCTION(Scriptable)
        bool GetDepthTest() const { return m_depthTest; }
        FUNCTION(Scriptable)
        void SetDepthWrite(bool enabled) { m_depthWrite = enabled; }
        FUNCTION(Scriptable)
        bool GetDepthWrite() const { return m_depthWrite; }
        FUNCTION(Scriptable)
        void SetAlwaysOnTop(bool enabled) { m_alwaysOnTop = enabled; }
        FUNCTION(Scriptable)
        bool GetAlwaysOnTop() const { return m_alwaysOnTop; }

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
        bool LoadDocumentFile(
            const std::string& path,
            bool updateSerializedPath,
            std::vector<UI::UIDocumentDiagnostic>* diagnostics);
        [[nodiscard]] UI::UIElement* ResolveElementHandle(uint64_t handle) const;

        std::string m_documentPath;
        std::shared_ptr<const UI::UIDocument> m_document;
        std::unique_ptr<UI::UIDocumentInstance> m_instance;
        UI::UIInputRouter m_inputRouter;
        RenderMode m_renderMode = RenderMode::ScreenOverlay;
        WorldRenderPath m_worldRenderPath = WorldRenderPath::Auto;
        UpdatePolicy m_updatePolicy = UpdatePolicy::EveryFrame;
        DpiScaleMode m_dpiScaleMode = DpiScaleMode::ConstantPixelSize;
        float m_dpiMatch = 0.5f;
        int32_t m_sortOrder = 0;
        int32_t m_sortLayer = 0;
        uint64_t m_cameraId = 0u;
        UI::UIVector2 m_referenceResolution { 1920.0f, 1080.0f };
        UI::UIVector2 m_worldSize { 1.0f, 1.0f };
        float m_pixelsPerUnit = 100.0f;
        bool m_depthTest = true;
        bool m_depthWrite = false;
        bool m_alwaysOnTop = false;
        UI::UIVector2 m_lastViewport;
        float m_lastDpiScale = 1.0f;
        uint64_t m_lastFrameId = 0u;
    };
}
