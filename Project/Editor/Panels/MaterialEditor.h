#pragma once
#include <UI/Panels/PanelWindow.h>
#include "Materials/MaterialEditingService.h"
#include "Materials/MaterialGraphView.h"
#include "Properties/PropertyPanel.h"

namespace NLS::Editor::Panels
{
class MaterialEditor final : public NLS::UI::PanelWindow
{
public:
    MaterialEditor(const std::string& title,bool opened,const NLS::UI::PanelWindowSettings& settings,
        std::filesystem::path projectRoot,std::filesystem::path engineRoot);
    ~MaterialEditor() override;
    bool OpenDocument(const std::filesystem::path& path);
protected:
    void OnBeforeDrawWidgets() override;
private:
    void DrawMaterial(Materials::MaterialEditSession& session);
    void DrawParameters(Materials::MaterialEditSession& session);
    void DrawSource(Materials::MaterialSourceEditSession& session);
    Materials::MaterialEditingService m_service;
    NLS::Editor::Properties::PropertyPanel m_parameterEditor;
    std::string m_active;
    std::string m_error;
    std::map<std::string,uint64_t> m_revisions;
    std::map<std::string,double> m_lastChange;
    std::string m_filter;
    std::map<std::string,std::string> m_shaderPaths,m_parentPaths;
    std::map<std::string,std::unique_ptr<Materials::MaterialGraphView>> m_graphViews;
};
}
