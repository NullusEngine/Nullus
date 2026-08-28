#include "UIStyleSheet.h"

#include "Assets/NativeArtifactContainer.h"
#include "UIElement.h"

#include <Json/json.hpp>

#include <algorithm>
#include <fstream>
#include <sstream>

namespace NLS::Engine::UI
{
    namespace
    {
        using Json = nlohmann::json;

        void Diagnostic(
            std::vector<UIStyleDiagnostic>* diagnostics,
            std::string path,
            std::string message)
        {
            if (diagnostics)
                diagnostics->push_back({ std::move(path), std::move(message), true });
        }

        bool ReadColor(const Json& value, UIColor& output)
        {
            if (!value.is_array() || value.size() < 3u)
                return false;
            for (size_t i = 0u; i < 3u; ++i)
                if (!value[i].is_number())
                    return false;
            output.r = value[0].get<float>();
            output.g = value[1].get<float>();
            output.b = value[2].get<float>();
            output.a = value.size() > 3u && value[3].is_number()
                ? value[3].get<float>() : 1.0f;
            return true;
        }

        void ReadPatch(const Json& value, UIStylePatch& output)
        {
            if (!value.is_object())
                return;
            UIColor color;
            if (value.contains("backgroundColor") && ReadColor(value["backgroundColor"], color))
                output.backgroundColor = color;
            if (value.contains("color") && ReadColor(value["color"], color))
                output.color = color;
            if (value.contains("opacity") && value["opacity"].is_number())
                output.opacity = std::clamp(value["opacity"].get<float>(), 0.0f, 1.0f);
            if (value.contains("visible") && value["visible"].is_boolean())
                output.visible = value["visible"].get<bool>();
            if (value.contains("enabled") && value["enabled"].is_boolean())
                output.enabled = value["enabled"].get<bool>();
            if (value.contains("interactive") && value["interactive"].is_boolean())
                output.interactive = value["interactive"].get<bool>();
        }

        uint32_t ParseStateMask(const Json& value)
        {
            uint32_t mask = 0u;
            const auto add = [&mask](std::string_view name)
            {
                if (name == "hovered" || name == "hover") mask |= static_cast<uint32_t>(UIStyleState::Hovered);
                else if (name == "pressed" || name == "active") mask |= static_cast<uint32_t>(UIStyleState::Pressed);
                else if (name == "focused" || name == "focus") mask |= static_cast<uint32_t>(UIStyleState::Focused);
                else if (name == "disabled") mask |= static_cast<uint32_t>(UIStyleState::Disabled);
                else if (name == "checked") mask |= static_cast<uint32_t>(UIStyleState::Checked);
                else if (name == "selected") mask |= static_cast<uint32_t>(UIStyleState::Selected);
                else if (name == "invalid") mask |= static_cast<uint32_t>(UIStyleState::Invalid);
            };
            if (value.is_string())
                add(value.get<std::string>());
            else if (value.is_array())
                for (const auto& state : value)
                    if (state.is_string())
                        add(state.get<std::string>());
            return mask;
        }
    }

    void UIStylePatch::ApplyTo(UIStyle& style) const
    {
        if (backgroundColor) style.backgroundColor = *backgroundColor;
        if (color) style.color = *color;
        if (opacity) style.opacity = *opacity;
        if (visible) style.visible = *visible;
        if (enabled) style.enabled = *enabled;
        if (interactive)
        {
            style.interactive = *interactive;
            style.interactiveSpecified = true;
        }
    }

    int UIStyleSelector::Specificity() const
    {
        return (id.empty() ? 0 : 100) +
            (className.empty() ? 0 : 10) +
            (type.has_value() ? 1 : 0) +
            (requiredStateMask == 0u ? 0 : 10);
    }

    bool UIStyleSelector::Matches(const UIElement& element, const uint32_t stateMask) const
    {
        if (type.has_value() && *type != element.GetElementType())
            return false;
        if (!id.empty() && id != element.GetName())
            return false;
        if (!className.empty() && className != element.GetClassName())
            return false;
        return (stateMask & requiredStateMask) == requiredStateMask;
    }

    std::optional<UIStyleSheet> UIStyleSheet::LoadFromJson(
        const std::string_view jsonText,
        std::vector<UIStyleDiagnostic>* diagnostics)
    {
        Json json;
        try
        {
            json = Json::parse(jsonText.begin(), jsonText.end());
        }
        catch (const std::exception& exception)
        {
            Diagnostic(diagnostics, "$", std::string("invalid JSON: ") + exception.what());
            return std::nullopt;
        }
        if (!json.is_object())
        {
            Diagnostic(diagnostics, "$", "style sheet root must be an object");
            return std::nullopt;
        }

        UIStyleSheet sheet;
        sheet.m_schemaVersion = json.value("version", 0u);
        if (sheet.m_schemaVersion != CurrentSchemaVersion)
        {
			Diagnostic(diagnostics, "$.version", "unsupported .style schema version");
            return std::nullopt;
        }
        if (!json.contains("rules") || !json["rules"].is_array())
        {
            Diagnostic(diagnostics, "$.rules", "rules must be an array");
            return std::nullopt;
        }

        for (size_t index = 0u; index < json["rules"].size(); ++index)
        {
            const auto& source = json["rules"][index];
            if (!source.is_object())
            {
                Diagnostic(diagnostics, "$.rules[" + std::to_string(index) + "]", "rule must be an object");
                return std::nullopt;
            }
            UIStyleRule rule;
            rule.sourceOrder = static_cast<uint32_t>(index);
            if (source.contains("selector") && source["selector"].is_object())
            {
                const auto& selector = source["selector"];
                if (selector.contains("type") && selector["type"].is_string())
                {
                    const auto parsed = ElementTypeFromString(selector["type"].get<std::string>());
                    if (parsed == UIElementType::Unknown)
                    {
                        Diagnostic(diagnostics, "$.rules[" + std::to_string(index) + "].selector.type", "unknown element type");
                        return std::nullopt;
                    }
                    rule.selector.type = parsed;
                }
                if (selector.contains("class") && selector["class"].is_string())
                    rule.selector.className = selector["class"].get<std::string>();
                if (selector.contains("id") && selector["id"].is_string())
                    rule.selector.id = selector["id"].get<std::string>();
                if (selector.contains("state"))
                    rule.selector.requiredStateMask = ParseStateMask(selector["state"]);
            }
            ReadPatch(source.value("style", Json {}), rule.patch);
            sheet.m_rules.push_back(std::move(rule));
        }
        return sheet;
    }

    std::optional<UIStyleSheet> UIStyleSheet::LoadFromFile(
        const std::filesystem::path& path,
        std::vector<UIStyleDiagnostic>* diagnostics)
    {
        std::ifstream input(path, std::ios::binary);
        if (!input)
        {
			Diagnostic(diagnostics, path.string(), "could not open .style style sheet");
            return std::nullopt;
        }
        std::ostringstream contents;
        contents << input.rdbuf();
        return LoadFromJson(contents.str(), diagnostics);
    }

    std::optional<UIStyleSheet> UIStyleSheet::LoadFromArtifactFile(
        const std::filesystem::path& path,
        std::vector<UIStyleDiagnostic>* diagnostics)
    {
        const auto payload = NLS::Core::Assets::ReadNativeArtifactPayloadTextFromFile(
            path,
            NLS::Core::Assets::ArtifactType::UIStyleSheet,
            CurrentSchemaVersion);
        if (!payload.has_value())
        {
            Diagnostic(diagnostics, path.string(), "could not read .style artifact container");
            return std::nullopt;
        }
        return LoadFromJson(payload->payload, diagnostics);
    }

    UIStyle UIStyleSheet::Resolve(
        const UIElement& element,
        UIStyle base,
        const uint32_t stateMask) const
    {
        std::vector<const UIStyleRule*> matches;
        for (const auto& rule : m_rules)
            if (rule.selector.Matches(element, stateMask))
                matches.push_back(&rule);
        std::stable_sort(
            matches.begin(), matches.end(),
            [](const auto* lhs, const auto* rhs)
            {
                if (lhs->selector.Specificity() != rhs->selector.Specificity())
                    return lhs->selector.Specificity() < rhs->selector.Specificity();
                return lhs->sourceOrder < rhs->sourceOrder;
            });
        for (const auto* rule : matches)
            rule->patch.ApplyTo(base);
        return base;
    }

    void UIStyleSheet::Apply(UIElement& root, const uint32_t stateMask) const
    {
        root.SetStyle(Resolve(root, root.GetStyle(), stateMask | root.GetStateMask()));
        for (const auto& child : root.GetChildren())
            if (child)
                Apply(*child, stateMask);
    }
}
