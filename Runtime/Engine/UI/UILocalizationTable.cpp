#include "UILocalizationTable.h"

#include "Assets/NativeArtifactContainer.h"
#include <Json/json.hpp>

#include <fstream>
#include <sstream>

namespace NLS::Engine::UI
{
    namespace
    {
        using Json = nlohmann::json;

        void AddDiagnostic(
            std::vector<UILocalizationDiagnostic>* diagnostics,
            std::string path,
            std::string message)
        {
            if (diagnostics)
                diagnostics->push_back({ std::move(path), std::move(message), true });
        }

        std::string ReplaceArguments(
            std::string value,
            const std::unordered_map<std::string, std::string>& arguments)
        {
            for (const auto& [name, replacement] : arguments)
            {
                const std::string token = "{" + name + "}";
                size_t offset = 0u;
                while ((offset = value.find(token, offset)) != std::string::npos)
                {
                    value.replace(offset, token.size(), replacement);
                    offset += replacement.size();
                }
            }
            return value;
        }
    }

    std::optional<UILocalizationTable> UILocalizationTable::LoadFromJson(
        const std::string_view jsonText,
        std::vector<UILocalizationDiagnostic>* diagnostics)
    {
        Json json;
        try
        {
            json = Json::parse(jsonText.begin(), jsonText.end());
        }
        catch (const std::exception& exception)
        {
            AddDiagnostic(diagnostics, "$", std::string("invalid JSON: ") + exception.what());
            return std::nullopt;
        }
        if (!json.is_object())
        {
            AddDiagnostic(diagnostics, "$", "localization root must be an object");
            return std::nullopt;
        }

        UILocalizationTable table;
        table.m_schemaVersion = json.value("version", 0u);
        if (table.m_schemaVersion != CurrentSchemaVersion)
        {
			AddDiagnostic(diagnostics, "$.version", "unsupported .loc schema version");
            return std::nullopt;
        }
        table.m_locale = json.value("locale", std::string {});
        table.m_fallbackLocale = json.value("fallbackLocale", std::string {});
        const auto entries = json.value("entries", Json {});
        if (!entries.is_object())
        {
            AddDiagnostic(diagnostics, "$.entries", "entries must be an object");
            return std::nullopt;
        }
        for (const auto& [key, value] : entries.items())
        {
            if (!value.is_string())
            {
                AddDiagnostic(
                    diagnostics,
                    "$.entries." + key,
                    "localization entries must be strings in schema v1");
                return std::nullopt;
            }
            table.m_entries.emplace(key, value.get<std::string>());
        }
        return table;
    }

    std::optional<UILocalizationTable> UILocalizationTable::LoadFromFile(
        const std::filesystem::path& path,
        std::vector<UILocalizationDiagnostic>* diagnostics)
    {
        std::ifstream input(path, std::ios::binary);
        if (!input)
        {
			AddDiagnostic(diagnostics, path.string(), "could not open .loc localization table");
            return std::nullopt;
        }
        std::ostringstream contents;
        contents << input.rdbuf();
        return LoadFromJson(contents.str(), diagnostics);
    }

    std::optional<UILocalizationTable> UILocalizationTable::LoadFromArtifactFile(
        const std::filesystem::path& path,
        std::vector<UILocalizationDiagnostic>* diagnostics)
    {
        const auto payload = NLS::Core::Assets::ReadNativeArtifactPayloadTextFromFile(
            path,
            NLS::Core::Assets::ArtifactType::LocalizationTable,
            CurrentSchemaVersion);
        if (!payload.has_value())
        {
            AddDiagnostic(diagnostics, path.string(), "could not read .loc artifact container");
            return std::nullopt;
        }
        return LoadFromJson(payload->payload, diagnostics);
    }

    bool UILocalizationTable::Contains(const std::string_view key) const
    {
        return m_entries.find(std::string(key)) != m_entries.end();
    }

    const std::string* UILocalizationTable::Lookup(const std::string_view key) const
    {
        const auto iterator = m_entries.find(std::string(key));
        return iterator == m_entries.end() ? nullptr : &iterator->second;
    }

    std::string UILocalizationTable::Format(
        const std::string_view key,
        const std::unordered_map<std::string, std::string>& arguments) const
    {
        const auto* value = Lookup(key);
        return value == nullptr ? std::string(key) : ReplaceArguments(*value, arguments);
    }
}
