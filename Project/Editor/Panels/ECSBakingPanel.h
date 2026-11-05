#pragma once
#include <UI/Panels/PanelWindow.h>
#include "ECS/SceneBakingService.h"
#include <array>

namespace NLS::Editor::Panels
{
class ECSBakingPanel final : public UI::PanelWindow
{
public:
    ECSBakingPanel(const std::string& name, bool opened, const UI::PanelWindowSettings& settings,
        ECSAuthoring::SceneBakingService& service, std::filesystem::path assetsRoot,
        std::function<bool(const std::filesystem::path&)> import);
protected:
    void OnBeforeDrawWidgets() override;
private:
    ECSAuthoring::SceneBakingService& m_service;
    std::filesystem::path m_assetsRoot;
    std::function<bool(const std::filesystem::path&)> m_import;
    std::array<char, 1024> m_output {};
    std::string m_exportMessage;
    std::string m_selected;
};
}
