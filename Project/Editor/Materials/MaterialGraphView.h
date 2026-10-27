#pragma once
#include "MaterialSourceDocument.h"
#include "Properties/PropertyPanel.h"
#include "MaterialCompiler/MaterialGraph.h"

namespace ax::NodeEditor
{
struct EditorContext;
}
namespace NLS::Editor::Materials
{
class MaterialGraphView final
{
public:
    MaterialGraphView();
    ~MaterialGraphView();
    void Draw(SourceTools::MaterialSourceDocument& document);

private:
    void DrawSettings(MaterialCompiler::MaterialGraphAsset& graph, bool& changed);
    void DrawNodeProperties(MaterialCompiler::MaterialGraphAsset& graph, bool& changed);
    Properties::PropertyPanel m_properties;
    Inspector::ReflectedPropertyEdit m_propertyEdit;
    ax::NodeEditor::EditorContext* m_context = nullptr;
    uint64_t m_revision = 0, m_selected = 0;
    std::string m_error;
    std::optional<MaterialCompiler::MaterialGraphAsset> m_workingGraph;
    uint64_t m_sourceRevision = 0;
};
} // namespace NLS::Editor::Materials
