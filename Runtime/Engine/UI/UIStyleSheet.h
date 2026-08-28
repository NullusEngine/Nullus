#pragma once

#include "EngineDef.h"
#include "UICommon.h"

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace NLS::Engine::UI
{
    class UIElement;

    struct NLS_ENGINE_API UIStyleDiagnostic
    {
        std::string path;
        std::string message;
        bool error = true;
    };

    enum class UIStyleState : uint32_t
    {
        None = 0u,
        Hovered = 1u << 0u,
        Pressed = 1u << 1u,
        Focused = 1u << 2u,
        Disabled = 1u << 3u,
        Checked = 1u << 4u,
        Selected = 1u << 5u,
        Invalid = 1u << 6u
    };

    constexpr UIStyleState operator|(UIStyleState lhs, UIStyleState rhs)
    {
        return static_cast<UIStyleState>(
            static_cast<uint32_t>(lhs) | static_cast<uint32_t>(rhs));
    }

    struct NLS_ENGINE_API UIStylePatch
    {
        std::optional<UIColor> backgroundColor;
        std::optional<UIColor> color;
        std::optional<float> opacity;
        std::optional<bool> visible;
        std::optional<bool> enabled;
        std::optional<bool> interactive;

        void ApplyTo(UIStyle& style) const;
    };

    struct NLS_ENGINE_API UIStyleSelector
    {
        std::optional<UIElementType> type;
        std::string className;
        std::string id;
        uint32_t requiredStateMask = 0u;

        [[nodiscard]] int Specificity() const;
        [[nodiscard]] bool Matches(const UIElement& element, uint32_t stateMask) const;
    };

    struct NLS_ENGINE_API UIStyleRule
    {
        UIStyleSelector selector;
        UIStylePatch patch;
        uint32_t sourceOrder = 0u;
    };

    class NLS_ENGINE_API UIStyleSheet final
    {
    public:
        static constexpr uint32_t CurrentSchemaVersion = 1u;

        static std::optional<UIStyleSheet> LoadFromJson(
            std::string_view json,
            std::vector<UIStyleDiagnostic>* diagnostics = nullptr);
        static std::optional<UIStyleSheet> LoadFromFile(
            const std::filesystem::path& path,
            std::vector<UIStyleDiagnostic>* diagnostics = nullptr);
        static std::optional<UIStyleSheet> LoadFromArtifactFile(
            const std::filesystem::path& path,
            std::vector<UIStyleDiagnostic>* diagnostics = nullptr);

        [[nodiscard]] uint32_t GetSchemaVersion() const { return m_schemaVersion; }
        [[nodiscard]] const std::vector<UIStyleRule>& GetRules() const { return m_rules; }

        [[nodiscard]] UIStyle Resolve(
            const UIElement& element,
            UIStyle base = {},
            uint32_t stateMask = 0u) const;
        void Apply(UIElement& root, uint32_t stateMask = 0u) const;

    private:
        uint32_t m_schemaVersion = CurrentSchemaVersion;
        std::vector<UIStyleRule> m_rules;
    };
}
