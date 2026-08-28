#pragma once

#include "EngineDef.h"

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace NLS::Engine::UI
{
    struct NLS_ENGINE_API UILocalizationDiagnostic
    {
        std::string path;
        std::string message;
        bool error = true;
    };

    class NLS_ENGINE_API UILocalizationTable final
    {
    public:
        static constexpr uint32_t CurrentSchemaVersion = 1u;

        static std::optional<UILocalizationTable> LoadFromJson(
            std::string_view json,
            std::vector<UILocalizationDiagnostic>* diagnostics = nullptr);
        static std::optional<UILocalizationTable> LoadFromFile(
            const std::filesystem::path& path,
            std::vector<UILocalizationDiagnostic>* diagnostics = nullptr);
        static std::optional<UILocalizationTable> LoadFromArtifactFile(
            const std::filesystem::path& path,
            std::vector<UILocalizationDiagnostic>* diagnostics = nullptr);

        [[nodiscard]] uint32_t GetSchemaVersion() const { return m_schemaVersion; }
        [[nodiscard]] const std::string& GetLocale() const { return m_locale; }
        [[nodiscard]] const std::string& GetFallbackLocale() const { return m_fallbackLocale; }
        void SetFallbackLocale(std::string locale) { m_fallbackLocale = std::move(locale); }

        [[nodiscard]] bool Contains(std::string_view key) const;
        [[nodiscard]] const std::string* Lookup(std::string_view key) const;
        [[nodiscard]] std::string Format(
            std::string_view key,
            const std::unordered_map<std::string, std::string>& arguments = {}) const;

    private:
        uint32_t m_schemaVersion = CurrentSchemaVersion;
        std::string m_locale;
        std::string m_fallbackLocale;
        std::unordered_map<std::string, std::string> m_entries;
    };
}
