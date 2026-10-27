#pragma once

#include <UI/Panels/PanelWindow.h>

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

#include "Build/BuildProfileIO.h"
#include "Build/BuildReport.h"
#include "Properties/PropertyPanel.h"
#include "Properties/SourcePropertyDocument.h"
#include <unordered_map>

namespace NLS::Editor::Panels
{
// Dedicated build configuration panel. Profiles are edited as project data
// (BuildProfiles.json) and saved atomically; the existing Build menu entries
// run the same packaging pipeline through NullusBuild.
class BuildProfilesPanel final : public UI::PanelWindow
{
public:
    BuildProfilesPanel(
        const std::string& p_title,
        bool p_opened,
        const UI::PanelWindowSettings& p_windowSettings);

    void Open();
    void Close();
    bool IsOpen() const { return m_opened; }

    // Reloads BuildProfiles.json from disk. Called when the panel opens and
    // after a build finishes.
    void ReloadProfiles();

    // Exposed for tests: the panel never writes an invalid document.
    static bool ValidateForSave(
        const NLS::Build::BuildProfilesDocument& document,
        std::vector<NLS::Build::BuildDiagnostic>& diagnostics);

protected:
    void _Draw_Impl() override;

private:
    void DrawProfileList();
    void DrawProfileDetails();
    void DrawActions();
    void DrawLastReport();
    void LoadLastReport();
    bool SaveProfiles();

    std::filesystem::path GetProfilesPath() const;
    std::filesystem::path GetProjectRoot() const;

    NLS::Build::BuildProfilesDocument m_document;
    std::vector<NLS::Build::BuildDiagnostic> m_diagnostics;
    std::vector<std::string> m_profileIds;
    std::string m_selectedProfileId;
    std::string m_newProfileId;
    std::string m_statusMessage;
    NLS::Build::BuildReport m_lastReport;
    bool m_hasLastReport = false;
    bool m_opened = false;
    bool m_dirty = false;
    bool m_showAssetRules = false;
    Properties::PropertyPanel m_properties;
    std::string m_activePropertyProfile;
    std::unordered_map<std::string,std::unique_ptr<Properties::SourcePropertyDocument>> m_profileDocuments;
};
} // namespace NLS::Editor::Panels
