#include "Properties/PropertyPanel.h"
#include "Properties/PropertyDrawerRegistry.h"
#include "Inspector/PropertyTypeTraits.h"

namespace NLS::Editor::Properties
{
PropertyPanel::PropertyPanel()
    : m_lifetime(std::make_shared<int>())
{
    m_values.SetLifetime(m_lifetime);
}
PropertyPanel::~PropertyPanel() = default;
void PropertyPanel::CancelEdits()
{
    m_values.CancelEdits();
    m_renderer.ClearControlState();
}
Inspector::ReflectedPropertyEdit PropertyPanel::DrawValue(std::string_view id, std::string_view label,
                                                          meta::Variant& value,
                                                          const Inspector::ReflectedPropertyOptions& options)
{
    return m_values.Draw(id, label, value, options);
}
Inspector::PropertyDrawResult PropertyPanel::Draw(Serialization::SerializedObject& object,
                                                  const PropertyPanelOptions& options)
{
    (void)object.UpdateIfRequiredOrScript();
    const auto version = object.GetVersion();
    std::string identity = options.identity;
    for (const auto& target : object.GetTargetHandles())
        identity += '|' + target.resourcePath + '#' + target.assetId + ':' + std::to_string(target.instanceId) + ':' + target.typeName;
    const auto registry = PropertyDrawerRegistry::Get().Version();
    const auto metadata = Inspector::InspectorMetadataRegistry::Get().GetVersion();
    if (identity != m_identity || version.schemaGeneration != m_version.schemaGeneration || version.structureGeneration != m_version.structureGeneration || registry != m_registryVersion || metadata != m_metadataVersion || options.search != m_search)
    {
        if (identity != m_identity)
        {
            if (!m_identity.empty())
                object.DiscardModifiedProperties();
            m_renderer.ClearControlState();
        }
        m_projection = Inspector::InspectorProjectionBuilder::BuildPropertyProjection(object, object.GetTargetSet(), options.search);
        m_identity = identity;
        m_search = options.search;
        m_registryVersion = registry;
        m_metadataVersion = metadata;
    }
    m_version = version;
    auto context = options.context;
    context.object = &object;
    ImGui::PushID(this);
    ImGui::PushID(identity.c_str());
    const auto result = m_renderer.DrawProperties(m_projection, context);
    ImGui::PopID();
    ImGui::PopID();
    return result;
}
} // namespace NLS::Editor::Properties
