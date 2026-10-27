#include "MaterialEditor.h"
#include <UI/Controls/TextControl.h>
#include <algorithm>
#include <imgui.h>
#include <imgui_stdlib.h>
#include "Core/EditorActions.h"
#include "Core/ServiceLocator.h"
#include "Panels/AssetView.h"
#include "Rendering/Resources/Shader.h"
#include "Inspector/ReflectedEditHost.h"
#include "Math/ExternalReflection.h"
#include <Math/Matrix4.h>

namespace NLS::Editor::Panels
{
namespace
{
Undo::EditorUndoService* UndoService()
{return NLS::Core::ServiceLocator::Contains<Undo::EditorUndoService>()?&NLS::Core::ServiceLocator::Get<Undo::EditorUndoService>():nullptr;}
class MaterialPropertyEditHost final : public NLS::Editor::Inspector::IReflectedEditHost
{
public:
    MaterialPropertyEditHost(Materials::MaterialEditingService& service, Materials::MaterialEditSession& session)
        : m_service(service), m_session(session) {}
    std::function<bool()> preview;
    bool BeginPropertyEdit(std::string_view label) override { return m_session.document.BeginEdit(std::string(label)); }
    bool PreviewPropertyEdit() override { return preview ? preview() : true; }
    bool CommitPropertyEdit() override { return m_session.document.EndEdit(); }
    bool CancelPropertyEdit() override
    {
        const bool restored = m_session.document.EndEdit(false);
        m_session.previewUpdates.clear();
        (void)m_service.RequestPreview(m_session);
        return restored;
    }
    bool IsPropertyEditOpen() const override { return m_session.document.IsEditing(); }

private:
    Materials::MaterialEditingService& m_service;
    Materials::MaterialEditSession& m_session;
};
}
MaterialEditor::MaterialEditor(const std::string& title,bool opened,const NLS::UI::PanelWindowSettings& settings,
    std::filesystem::path projectRoot,std::filesystem::path engineRoot)
    :PanelWindow(title,opened,settings),m_service(std::move(projectRoot),std::move(engineRoot),UndoService()) {}

MaterialEditor::~MaterialEditor()
{
}

bool MaterialEditor::OpenDocument(const std::filesystem::path& path)
{
    if(path.extension()==".mat") {if(!m_service.Open(path,m_error))return false;}
    else if(!m_service.OpenSource(path,m_error))return false;
    m_active=std::filesystem::absolute(path).lexically_normal().generic_string();
    Open();Focus();return true;
}

void MaterialEditor::OnBeforeDrawWidgets()
{
    if (!m_error.empty()) ImGui::TextWrapped("%s",m_error.c_str());
    if (m_service.Sessions().empty() && m_service.SourceSessions().empty())
    {ImGui::TextUnformatted("Create a material in the Asset Browser, or open a .mat asset.");return;}
    if (ImGui::BeginTabBar("MaterialDocuments"))
    {
        for (const auto& [key,session] : m_service.Sessions())
        {
            if(session->sourcePreview)continue;
            auto label=session->document.Path().filename().string();
            if (session->document.IsDirty()) label+=" *";
            label+="###"+key;
            const auto flags=key==m_active?ImGuiTabItemFlags_SetSelected:ImGuiTabItemFlags_None;
            if (ImGui::BeginTabItem(label.c_str(),nullptr,flags))
            {m_active.clear();DrawMaterial(*session);ImGui::EndTabItem();}
        }
        for(const auto& [key,session]:m_service.SourceSessions())
        {
            const auto label=session->document.Path().filename().string()+(session->document.IsDirty()?" *":"")+"###source:"+key;
            if(ImGui::BeginTabItem(label.c_str(),nullptr,key==m_active?ImGuiTabItemFlags_SetSelected:ImGuiTabItemFlags_None))
            {m_active.clear();DrawSource(*session);ImGui::EndTabItem();}
        }
        ImGui::EndTabBar();
    }
}

void MaterialEditor::DrawSource(Materials::MaterialSourceEditSession& session)
{
    auto& document=session.document;
    const auto key=document.Path().generic_string();
    if(ImGui::Button("Save source"))
    {if(!document.Save())session.error=document.Error();else session.error.clear();}
    ImGui::SameLine();if(ImGui::Button("Compile / Preview"))m_service.PreviewSource(session);
    ImGui::SameLine();if(ImGui::Button("Apply to Scene"))m_service.ApplySource(session);
    ImGui::SameLine();if(ImGui::Button("Save and Apply") && document.Save())m_service.ApplySource(session);
    ImGui::SameLine();if(ImGui::Button("Revert Working"))
    {(void)document.EndEdit();(void)document.Apply(document.SavedSource(),"Revert material source");}
    ImGui::SameLine();if(auto* undo=UndoService())
    {if(ImGui::Button("Undo"))(void)undo->RequestUndo();ImGui::SameLine();if(ImGui::Button("Redo"))(void)undo->RequestRedo();}
    const auto now=ImGui::GetTime();
    if(m_revisions[key]!=document.Revision()){m_revisions[key]=document.Revision();m_lastChange[key]=now;}
    if(session.preview && !document.IsEditing() && now-m_lastChange[key]>0.5)m_service.PreviewSource(session);
    ImGui::Text("%s | %s",document.IsDirty()?"Unsaved":"Saved",document.Asset()==session.appliedSource?"Applied":"Unapplied");
    if(!session.error.empty())ImGui::TextWrapped("%s",session.error.c_str());
    if(session.preview)
    {
        if(!session.preview->error.empty())ImGui::TextWrapped("%s",session.preview->error.c_str());
        if(session.preview->preview && ImGui::Button("Show Live Preview"))
        {auto& view=EDITOR_PANEL(NLS::Editor::Panels::AssetView,"Asset View");view.SetOwnedMaterial(session.preview->preview);view.Open();}
    }
    if(ImGui::BeginTabBar("Source editors"))
    {
        if(document.Path().extension()!=".shader" && ImGui::BeginTabItem("Nodes"))
        {auto& view=m_graphViews[key];if(!view)view=std::make_unique<Materials::MaterialGraphView>();view->Draw(document);ImGui::EndTabItem();}
        if(ImGui::BeginTabItem(document.Path().extension()==".shader"?"ShaderLab / HLSL":"Source JSON"))
        {
            auto text=document.Asset();
            const bool changed=UI::Controls::Text("##Source",text,true,{},ImGui::GetContentRegionAvail(),ImGuiInputTextFlags_AllowTabInput).changed;
            if(ImGui::IsItemActivated())(void)document.BeginEdit("Edit material source");
            if(changed){if(!document.IsEditing())(void)document.BeginEdit("Edit material source");(void)document.PreviewEdit(std::move(text));}
            if(ImGui::IsItemDeactivatedAfterEdit())(void)document.EndEdit();
            ImGui::EndTabItem();
        }
        ImGui::EndTabBar();
    }
}

void MaterialEditor::DrawMaterial(Materials::MaterialEditSession& session)
{
    const auto key=session.document.Path().generic_string();
    const auto now=ImGui::GetTime();
    if (m_revisions[key]!=session.document.Revision())
    {m_revisions[key]=session.document.Revision();m_lastChange[key]=now;session.failedSource.clear();}
    if (!session.document.IsEditing() && now-m_lastChange[key]>=0.3) m_service.RequestPreview(session);
    if (ImGui::Button("Save")) m_service.Save(session);
    ImGui::SameLine();
    ImGui::BeginDisabled(!session.compiled || session.pending.valid());
    if (ImGui::Button("Apply to Scene")) m_service.ApplyToScene(session);
    ImGui::SameLine();
    if (ImGui::Button("Save and Apply") && m_service.Save(session)) m_service.ApplyToScene(session);
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (ImGui::Button("Compile")) {session.failedSource.clear();session.compiled.reset();m_service.RequestPreview(session);}
    ImGui::SameLine();
    auto* undo=UndoService();
    ImGui::BeginDisabled(!undo || !undo->CanUndo());if (ImGui::Button("Undo") && undo) (void)undo->RequestUndo();ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::BeginDisabled(!undo || !undo->CanRedo());if (ImGui::Button("Redo") && undo) (void)undo->RequestRedo();ImGui::EndDisabled();
    ImGui::SameLine();
    if (ImGui::Button("Revert Working")) (void)session.document.DiscardWorking();
    ImGui::SameLine();ImGui::BeginDisabled(session.pendingCook.valid() || !session.document.IsApplied());
    if(ImGui::Button("Publish resources"))m_service.PublishResources(session);ImGui::EndDisabled();
    if(!session.lastRelease.empty())ImGui::TextWrapped("Published: %s",session.lastRelease.string().c_str());
    ImGui::Text("%s | %s | %s",session.document.IsDirty()?"Unsaved":"Saved",
        session.document.IsApplied()?"Applied":"Unapplied",session.pending.valid()?"Compiling":"Ready");
    if (!session.error.empty()) ImGui::TextWrapped("%s",session.error.c_str());
    if (session.preview && ImGui::Button("Show Live Preview"))
    {auto& view=EDITOR_PANEL(NLS::Editor::Panels::AssetView,"Asset View");view.SetOwnedMaterial(session.preview);view.Open();}
    ImGui::Separator();
    m_parameterEditor.SetLifetime(session.document.Lifetime());
    const auto editField = [&](const std::string& id, const std::string& label, auto& value, auto&& publish, const NLS::Editor::Inspector::ReflectedPropertyOptions& options = NLS::Editor::Inspector::ReflectedPropertyOptions{})
    {
        const auto edit = m_parameterEditor.Draw(id, label, value, options);
        MaterialPropertyEditHost host(m_service, session);
        host.preview = publish;
        if (!NLS::Editor::Inspector::ApplyReflectedEdit(host, edit, "Edit " + label))
            m_error = session.document.Error();
    };
    const auto& asset=session.document.Asset();
    ImGui::TextWrapped("Shader: %s",session.compiled?session.compiled->resolved.shader.path.c_str():asset.shader.path.c_str());
    if(session.compiled && ImGui::Button("Edit shader / graph"))OpenDocument(m_service.ResolvePath(session.compiled->resolved.shader.path));
    if(!session.compiled && !asset.shader.Empty() && ImGui::Button("Use current shader dependency"))
    {const auto source="file:"+std::filesystem::absolute(m_service.ResolvePath(asset.shader.path)).lexically_normal().generic_string();
        session.workingDependencies.insert(source);session.failedSource.clear();m_service.RequestPreview(session);}
    if (!asset.parent.Empty()) ImGui::TextWrapped("Parent: %s",asset.parent.path.c_str());
    if (ImGui::CollapsingHeader("Material source"))
    {
        auto [shader,inserted]=m_shaderPaths.try_emplace(key,asset.shader.path);
        auto [parent,parentInserted]=m_parentPaths.try_emplace(key,asset.parent.path);
        const auto shaderEdit = m_parameterEditor.Draw("ShaderSource", "Shader asset", shader->second);
        if (shaderEdit.canceled)
            shader->second = asset.shader.path;
        if (shaderEdit.completed && !shaderEdit.canceled && shader->second != asset.shader.path)
        {
            auto candidate=asset;candidate.parent={};candidate.shader=m_service.ResolveReference(shader->second);
            (void)session.document.Apply(std::move(candidate),"Change material shader");
        }
        const auto parentEdit = m_parameterEditor.Draw("ParentSource", "Parent material", parent->second);
        if (parentEdit.canceled)
            parent->second = asset.parent.path;
        if (parentEdit.completed && !parentEdit.canceled && parent->second != asset.parent.path && !parent->second.empty())
        {
            auto candidate=asset;candidate.shader={};candidate.parent=m_service.ResolveReference(parent->second,"material:main");
            (void)session.document.Apply(std::move(candidate),"Change parent material");
        }
        ImGui::TextUnformatted("Enter an asset path and finish editing to select a shader or parent.");
    }
    (void)m_parameterEditor.Draw("ParameterSearch", "Search parameters", m_filter);
    DrawParameters(session);
    if (ImGui::CollapsingHeader("Static switches"))
    {
        auto candidate=session.document.Asset();
        for (auto& [name,enabled] : candidate.staticParameters)
            editField("Static:" + name, name, enabled, [&]
                      { return session.document.Apply(candidate, "Set static switch"); });
        static std::string keyword;
        (void)m_parameterEditor.Draw("NewKeyword", "Keyword", keyword);
        if (ImGui::Button("Add switch") && !keyword.empty())
        {candidate.staticParameters[keyword]=true;(void)session.document.Apply(candidate,"Add static switch");keyword.clear();}
    }
    if (session.compiled && ImGui::CollapsingHeader("Dependencies"))
        for (const auto& [identity,source] : session.compiled->dependencies.sources)
        {
            if (identity=="material:"+asset.identity.Key() || identity.find("Generated")!=std::string::npos) continue;
            bool selected=session.workingDependencies.contains(identity);
            if (m_parameterEditor.Draw("Dependency:" + identity, "Use current version", selected).changed)
            {if(selected)session.workingDependencies.insert(identity);else session.workingDependencies.erase(identity);
                session.compiled.reset();session.failedSource.clear();break;}
            ImGui::SameLine();ImGui::TextUnformatted(identity.c_str());
        }
    if(ImGui::CollapsingHeader("Variant budget"))
    {
        int32_t budget = static_cast<int32_t>(session.variantBudget);
        NLS::Editor::Inspector::ReflectedPropertyOptions options;
        options.metadata.emplace();
        options.metadata->min = 1;
        options.metadata->max = 4096;
        if (m_parameterEditor.Draw("VariantBudget", "Maximum requested variants", budget, options).changed)
            session.variantBudget = static_cast<size_t>(budget);
    }
    if(ImGui::CollapsingHeader("Render state"))
    {
        auto candidate=session.document.Asset();
        bool doubleSided = candidate.renderStateOverrides.contains("doubleSided") && candidate.renderStateOverrides.at("doubleSided") == "true";
        bool depthWrite=!candidate.renderStateOverrides.contains("depthWrite") || candidate.renderStateOverrides.at("depthWrite")=="true";
        editField("DoubleSided", "Double sided", doubleSided, [&]
                  {candidate.renderStateOverrides["doubleSided"]=doubleSided?"true":"false";return session.document.Apply(candidate,"Edit material render state"); });
        editField("DepthWrite", "Write depth", depthWrite, [&]
                  {candidate.renderStateOverrides["depthWrite"]=depthWrite?"true":"false";return session.document.Apply(candidate,"Edit material render state"); });
        if (ImGui::Button("Inherit render state"))
        {
            candidate.renderStateOverrides.clear();
            (void)session.document.Apply(candidate, "Inherit render state");
        }
    }
    if(session.preview && session.preview->GetShader() && ImGui::CollapsingHeader("Samplers"))
    {
        auto candidate = session.document.Asset();
        for(const auto& property:session.preview->GetShader()->GetReflection().properties)
        {
            if(property.kind!=NLS::Render::Resources::ShaderResourceKind::Sampler || property.bindingSpace!=2)continue;
            ImGui::PushID(property.name.c_str());
            if(ImGui::TreeNode(property.name.c_str()))
            {
                auto sampler=session.preview->GetSamplerOverride(property.name)?*session.preview->GetSamplerOverride(property.name):NLS::Render::RHI::SamplerDesc{};
                if(const auto local=candidate.samplerOverrides.find(property.name);local!=candidate.samplerOverrides.end())sampler=local->second;
                const auto drawChoice = [&](const char* label, auto& selected, std::initializer_list<const char*> labels)
                {
                    int32_t value = static_cast<int32_t>(selected);
                    NLS::Editor::Inspector::ReflectedPropertyOptions options;
                    int32_t index = 0;
                    for (const auto* text : labels)
                    {
                        NLS::Editor::Inspector::InspectorPropertyChoice choice{std::to_string(index), text};
                        choice.typedValue = index++;
                        options.choices.push_back(std::move(choice));
                    }
                    editField(property.name + ":" + label, label, value, [&]
                              {selected=static_cast<std::remove_reference_t<decltype(selected)>>(value);candidate.samplerOverrides[property.name]=sampler;return session.document.Apply(candidate,"Edit material sampler"); },
                              options);
                };
                drawChoice("Wrap U", sampler.wrapU, {"Clamp", "Repeat", "Mirror", "Border"});
                drawChoice("Wrap V", sampler.wrapV, {"Clamp", "Repeat", "Mirror", "Border"});
                drawChoice("Wrap W", sampler.wrapW, {"Clamp", "Repeat", "Mirror", "Border"});
                drawChoice("Min", sampler.minFilter, {"Nearest", "Linear"});
                drawChoice("Mag", sampler.magFilter, {"Nearest", "Linear"});
                drawChoice("Mip", sampler.mipFilter, {"Nearest", "Linear"});
                NLS::Editor::Inspector::ReflectedPropertyOptions anisotropy;
                anisotropy.metadata.emplace();
                anisotropy.metadata->min = 1;
                anisotropy.metadata->max = 16;
                const auto publish = [&]
                {candidate.samplerOverrides[property.name]=sampler;return session.document.Apply(candidate,"Edit material sampler"); };
                editField(property.name + ":Anisotropy", "Anisotropy", sampler.maxAnisotropy, publish, anisotropy);
                NLS::Editor::Inspector::ReflectedPropertyOptions bias;
                bias.metadata.emplace();
                bias.metadata->step = 0.1f;
                editField(property.name + ":Bias", "Mip bias", sampler.mipLodBias, publish, bias);
                if (ImGui::Button("Inherit sampler"))
                {
                    candidate.samplerOverrides.erase(property.name);
                    (void)session.document.Apply(candidate, "Inherit sampler");
                }
                ImGui::TreePop();
            }ImGui::PopID();
        }
    }
    MaterialPropertyEditHost finishHost(m_service, session);
    (void)NLS::Editor::Inspector::FinishReflectedEdits(m_parameterEditor, finishHost);
}

void MaterialEditor::DrawParameters(Materials::MaterialEditSession& session)
{
    if (!session.preview || !session.preview->GetShader()) {ImGui::TextUnformatted("Compiling material parameters...");return;}
    const auto schema=session.preview->GetShader()->GetMaterialParameterSchema();
    m_parameterEditor.SetLifetime(session.document.Lifetime());
    MaterialPropertyEditHost host(m_service, session);
    for (const auto& parameter : schema->parameters)
    {
        if (!m_filter.empty() && parameter.name.find(m_filter)==std::string::npos && parameter.displayName.find(m_filter)==std::string::npos) continue;
        ImGui::PushID(parameter.name.c_str());
        auto value=parameter.defaultValue;
        if (session.compiled)
            if (const auto resolved=session.compiled->resolved.parameters.find(parameter.name);resolved!=session.compiled->resolved.parameters.end()) value=resolved->second.value;
        if(parameter.isStatic)
        {
            if(session.compiled)if(const auto found=session.compiled->resolved.staticParameters.find(parameter.name);
                found!=session.compiled->resolved.staticParameters.end())value=found->second;
            if(const auto found=session.document.Asset().staticParameters.find(parameter.name);
                found!=session.document.Asset().staticParameters.end())value=found->second;
        }
        for (const auto& local : session.document.Asset().parameters)
            if (local.id==parameter.id || local.name==parameter.name) value=local.value;
        const auto label = parameter.displayName.empty() ? parameter.name : parameter.displayName;
        NLS::Editor::Inspector::ReflectedPropertyOptions options;
        options.metadata.emplace();
        if (parameter.range)
        {
            options.metadata->rangeMin = (*parameter.range)[0];
            options.metadata->rangeMax = (*parameter.range)[1];
        }
        const auto nativeDefault=std::visit([&](const auto& item)->meta::Variant
        {
            using T=std::decay_t<decltype(item)>;
            if constexpr(std::is_same_v<T,std::array<float,4>>)
            {if(parameter.color) return Maths::Color(item[0],item[1],item[2],item[3]);return Maths::Vector4(item[0],item[1],item[2],item[3]);}
            else if constexpr(std::is_same_v<T,std::array<float,16>>)
            {Maths::Matrix4 matrix;std::copy(item.begin(),item.end(),matrix.data);return matrix;}
            else return meta::Variant(item);
        },parameter.defaultValue);
        options.defaultValue=[nativeDefault](std::string_view)->std::optional<meta::Variant>{return nativeDefault;};
        options.restoreInherited=[&,id=parameter.id,name=parameter.name]
        {
            if(host.IsPropertyEditOpen() && !host.CancelPropertyEdit()) return false;
            return m_service.ClearParameter(session,id,name);
        };
        NLS::Editor::Inspector::ReflectedPropertyEdit edit;
        const auto draw = [&]<class T>(T& native, const std::string& id, const std::string& fieldLabel)
        {
            const auto result = m_parameterEditor.Draw(id, fieldLabel, native, options);
            edit.changed |= result.changed;
            edit.completed |= result.completed;
            edit.canceled |= result.canceled;
            edit.paths.insert(edit.paths.end(), result.paths.begin(), result.paths.end());
        };
        std::visit([&](auto& item)
        {
            using T=std::decay_t<decltype(item)>;
            if constexpr (std::is_same_v<T, std::array<float, 4>>)
            {
                if (parameter.color)
                {
                    Maths::Color color(item[0], item[1], item[2], item[3]);
                    draw(color, parameter.name, label);
                    item = {color.r, color.g, color.b, color.a};
                }
                else
                {
                    Maths::Vector4 vector(item[0], item[1], item[2], item[3]);
                    draw(vector, parameter.name, label);
                    item = {vector.x, vector.y, vector.z, vector.w};
                }
            }
            else if constexpr(std::is_same_v<T,std::array<float,16>>)
            {
                Maths::Matrix4 matrix;std::copy(item.begin(),item.end(),matrix.data);draw(matrix,parameter.name,label);
                std::copy(std::begin(matrix.data),std::end(matrix.data),item.begin());
            }
            else
                draw(item, parameter.name, label);
        },value);
        host.preview = [&]
        { return m_service.SetParameter(session, parameter, value); };
        if (!NLS::Editor::Inspector::ApplyReflectedEdit(host, edit, "Edit " + label))
            m_error = session.document.Error();
        ImGui::PopID();
    }
    (void)NLS::Editor::Inspector::FinishReflectedEdits(m_parameterEditor, host);
    if(ImGui::CollapsingHeader("Retained parameters"))
        for(const auto& parameter:session.document.Asset().parameters)
            if(!(parameter.id?schema->Find(parameter.id):schema->Find(parameter.name)))
                ImGui::TextWrapped("%s: no matching shader parameter; source value retained",parameter.name.c_str());
}
}
