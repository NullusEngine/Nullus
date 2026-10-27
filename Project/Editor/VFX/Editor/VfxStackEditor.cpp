#include "VfxStackEditor.h"
#include "VfxStyle.h"
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <set>
#include <imgui_internal.h>

namespace NLS::Editor::VFXTools
{
namespace author = VFX::Authoring;
std::string_view VfxSourceId(const author::Stack& stack) { return stack.inheritedId.empty() ? stack.id : stack.inheritedId; }
std::string_view VfxSourceId(const author::Module& module) { return module.inheritedId.empty() ? module.id : module.inheritedId; }
bool StoreVfxModuleInput(author::Emitter& emitter,const author::Module& module,const std::string& name,const author::Input& input)
{
    if (!module.inputs.contains(name)) return false;
    const auto id = std::string(VfxSourceId(module));
    for (auto& stack : emitter.stacks) for (auto& local : stack.modules) if (local.id == id)
    { local.inputs[name] = input; return true; }
    if (emitter.parent.Empty()) return false;
    auto item = std::find_if(emitter.overrides.begin(),emitter.overrides.end(),[&](const auto& value) { return value.module == id; });
    if (item == emitter.overrides.end()) { emitter.overrides.push_back({id}); item = std::prev(emitter.overrides.end()); }
    item->inputs[name] = input; return true;
}
bool ResetVfxModuleInput(author::Emitter& emitter,const author::Module& module,const std::string& name,const author::Input& input)
{
    const auto id = std::string(VfxSourceId(module));
    for (auto& stack : emitter.stacks) for (auto& local : stack.modules) if (local.id == id)
    { local.inputs[name] = input; return true; }
    if (emitter.parent.Empty()) return false;
    auto item = std::find_if(emitter.overrides.begin(),emitter.overrides.end(),[&](const auto& value) { return value.module == id; });
    if (item == emitter.overrides.end()) return true;
    item->inputs.erase(name);
    if (item->inputs.empty() && !item->enabled.has_value()) emitter.overrides.erase(item);
    return true;
}
namespace
{
std::string Lower(std::string_view text)
{
    std::string result(text);
    std::transform(result.begin(),result.end(),result.begin(),[](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return result;
}
bool Matches(const VfxModuleChoice& choice, std::string_view search)
{
    const auto text = Lower(choice.name+" "+choice.id+" "+choice.category+" "+choice.description);
    const auto query = Lower(search);
    size_t begin = 0;
    while (begin < query.size())
    {
        const auto end = query.find(' ',begin);
        const auto token = query.substr(begin,end == std::string::npos ? end : end-begin);
        if (!token.empty() && text.find(token) == std::string::npos) return false;
        if (end == std::string::npos) break;
        begin = end+1;
    }
    return true;
}
bool MatchesTokens(std::string_view text,std::string_view search)
{
    const auto haystack = Lower(text);
    const auto query = Lower(search);
    size_t begin = 0;
    while (begin < query.size())
    {
        const auto end = query.find(' ',begin);
        const auto token = query.substr(begin,end == std::string::npos ? end : end-begin);
        if (!token.empty() && haystack.find(token) == std::string::npos) return false;
        if (end == std::string::npos) break;
        begin = end+1;
    }
    return true;
}
std::string FormatValue(const NLS::VFX::ValueType type,const NLS::VFX::Value& value)
{
    char buffer[64]{};
    switch (type)
    {
    case NLS::VFX::ValueType::Bool: std::snprintf(buffer,sizeof(buffer),"%s",value[0] != 0.0f ? "True" : "False"); break;
    case NLS::VFX::ValueType::Int32: std::snprintf(buffer,sizeof(buffer),"%d",static_cast<int>(value[0])); break;
    case NLS::VFX::ValueType::Float: std::snprintf(buffer,sizeof(buffer),"%g",static_cast<double>(value[0])); break;
    default: return {};
    }
    return buffer;
}
}
std::string VfxInputSummary(const author::Module& module,const author::ModuleDefinition* definition)
{
    // The summary highlights what the artist changed: bindings, curves and
    // scripts always show, literal values only when they differ from the
    // definition default. Three entries keep the row readable.
    struct Entry { std::string text; int priority; };
    std::vector<Entry> entries;
    const auto add = [&](std::string text,int priority)
    {
        if (text.empty() || entries.size() >= 6) return;
        entries.push_back({std::move(text),priority});
    };
    for (const auto& [name,input] : module.inputs)
    {
        if (input.mode == author::InputMode::Parameter) add(input.parameter.empty() ? name : input.parameter,0);
        else if (input.mode == author::InputMode::Curve) add(name+" Curve",1);
        else if (input.mode == author::InputMode::DynamicInput) add(name+" Script",1);
        else if (input.mode == author::InputMode::Script) add(name+" Asset",1);
        else if (input.mode == author::InputMode::Literal)
        {
            NLS::VFX::Value defaultValue{};
            if (definition) for (const auto& declared : definition->inputs) if (declared.name == name) { defaultValue = declared.defaultValue; break; }
            if (input.value == defaultValue) continue;
            if (const auto text = FormatValue(input.type,input.value); !text.empty())
            {
                // A literal that differs from the default is shown as "Name Value".
                char buffer[128]{};
                std::snprintf(buffer,sizeof(buffer),"%s %s",name.c_str(),text.c_str());
                add(buffer,2);
            }
        }
    }
    std::stable_sort(entries.begin(),entries.end(),[](const Entry& a,const Entry& b) { return a.priority < b.priority; });
    std::string summary;
    for (size_t index = 0; index < entries.size() && index < 3; ++index)
    {
        if (!summary.empty()) summary += "  ";
        summary += entries[index].text;
    }
    return summary;
}
bool MatchesVfxModule(const author::Module& module,const std::string_view label,
    const author::ModuleDefinition* definition,const std::string_view search)
{
    if (search.empty()) return true;
    std::string text(label);
    text += " "+module.script;
    if (definition)
    {
        text += " "+definition->category+" "+definition->description+" "+definition->name;
        for (const auto& input : definition->inputs) text += " "+input.name;
        for (const auto& attribute : definition->attributes) text += " "+attribute.name;
    }
    for (const auto& [name,input] : module.inputs)
    {
        text += " "+name;
        if (!input.parameter.empty()) text += " "+input.parameter;
    }
    return MatchesTokens(text,search);
}
std::vector<VfxModuleChoice> FindVfxModules(const author::SourceLibrary& library, author::Stage stage,
    std::string_view search, bool includeDeprecated)
{
    std::vector<VfxModuleChoice> result;
    // Engine-provided module assets are the canonical entries. Match them by
    // asset name so the raw built-in row is hidden without consulting a
    // code-defined module id stored on the asset.
    std::set<std::string> assetNames;
    for (const auto& [id,module] : library.Scripts())
        if (module.kind == author::AssetKind::Module && module.visible && (!module.deprecated || includeDeprecated))
            if (const auto* version = author::SelectVersion(module,{}))
                if (std::find(version->stages.begin(),version->stages.end(),stage) != version->stages.end())
                    assetNames.insert(module.name);
    for (const auto& module : author::ModuleLibrary()) if (author::SupportsStage(module,stage))
    {
        if (assetNames.contains(module.name)) continue;
        VfxModuleChoice choice{module.id,module.name,module.category,module.description};
        if (Matches(choice,search)) result.push_back(std::move(choice));
    }
    for (const auto& [id,module] : library.Scripts())
        if (module.kind == author::AssetKind::Module && module.visible && (!module.deprecated || includeDeprecated))
            if (const auto* version = author::SelectVersion(module,{});
                version && std::find(version->stages.begin(),version->stages.end(),stage) != version->stages.end())
            {
                VfxModuleChoice choice{id,module.name,module.category.empty() ? "Local Modules" : module.category,module.description,true,module.deprecated};
                if (Matches(choice,search)) result.push_back(std::move(choice));
            }
    std::stable_sort(result.begin(),result.end(),[](const auto& a,const auto& b)
    { const auto ac = Lower(a.category), bc = Lower(b.category); return ac == bc ? Lower(a.name) < Lower(b.name) : ac < bc; });
    return result;
}

VfxStackRowResult DrawVfxStackRow(const VfxStackRow& row)
{
    VfxStackRowResult result;
    const auto origin = ImGui::GetCursorScreenPos();
    const float scale = ImGui::GetFontSize()/13.0f;
    const float height = ImGui::GetFontSize()+(row.header ? 10.0f : 7.0f)*scale;
    const float width = (std::max)(row.width,120.0f*scale), tool = height;
    const float expandWidth = row.expandable ? tool : 8.0f*scale;
    const float rightWidth = (row.enable || row.add ? tool : 0)+(row.remove ? tool : 0)+(row.inherited || row.issue ? 18.0f*scale : 0);
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing,{0,0});
    ImGui::BeginGroup();
    ImGui::SetCursorScreenPos({origin.x+expandWidth,origin.y});
    result.selected = ImGui::InvisibleButton("##Select",{width-expandWidth-rightWidth,height});
    auto item = GImGui->LastItemData;
    const bool hovered = ImGui::IsItemHovered();
    result.doubleClicked = hovered && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left);
    result.context = hovered && ImGui::IsMouseReleased(ImGuiMouseButton_Right);
    auto* draw = ImGui::GetWindowDrawList();
    const ImVec2 end{origin.x+width,origin.y+height};
    const ImU32 background = row.selected ? IM_COL32(53,78,102,255) : hovered ? IM_COL32(53,53,53,255) : row.header ? IM_COL32(43,43,43,255) : IM_COL32(30,30,30,255);
    draw->AddRectFilled(origin,end,background);
    if (row.title && row.accent && !row.selected && !hovered)
    {
        const auto tint = ImGui::ColorConvertU32ToFloat4(row.accent);
        const auto upper = ImGui::ColorConvertFloat4ToU32({tint.x*0.75f,tint.y*0.75f,tint.z*0.75f,1});
        const auto lower = ImGui::ColorConvertFloat4ToU32({tint.x*0.35f,tint.y*0.35f,tint.z*0.35f,1});
        draw->AddRectFilledMultiColor(origin,end,upper,upper,lower,lower);
    }
    draw->AddLine({origin.x,end.y-1},{end.x,end.y-1},IM_COL32(12,12,12,255));
    if (row.accent) draw->AddRectFilled(origin,{origin.x+2.0f*scale,end.y},row.accent);
    const auto color = ImGui::GetColorU32(row.enabled && !row.muted ? ImGuiCol_Text : ImGuiCol_TextDisabled);
    ImGui::RenderTextClipped({origin.x+expandWidth+2*scale,origin.y+(height-ImGui::GetFontSize())/2},
        {end.x-rightWidth-3*scale,end.y},row.label,nullptr,nullptr);
    // Tint the text alone: a disabled module remains selectable and can be enabled.
    if (!row.enabled || row.muted)
    {
        draw->AddRectFilled({origin.x+expandWidth,origin.y},{end.x-rightWidth,end.y-1},background);
        draw->PushClipRect({origin.x+expandWidth,origin.y},{end.x-rightWidth-3*scale,end.y},true);
        draw->AddText({origin.x+expandWidth+2*scale,origin.y+(height-ImGui::GetFontSize())/2},color,row.label);
        draw->PopClipRect();
    }
    if (hovered && row.label && ImGui::CalcTextSize(row.label).x > width-expandWidth-rightWidth-5*scale) ImGui::SetTooltip("%s",row.label);
    // Right-aligned summary chip: badge (module version) then the input summary.
    // The summary is clipped so a long binding list never covers the actions.
    {
        float right = end.x-rightWidth-4*scale;
        if (row.badge && *row.badge)
        {
            const auto text = ImGui::CalcTextSize(row.badge);
            const ImVec2 chipMin{right-text.x-4*scale,origin.y+(height-text.y)/2};
            const ImVec2 chipMax{right,chipMin.y+text.y};
            draw->AddRectFilled(chipMin,chipMax,IM_COL32(48,66,84,255),2*scale);
            draw->AddText({chipMin.x+2*scale,chipMin.y},IM_COL32(168,200,232,255),row.badge);
            right = chipMin.x-4*scale;
        }
        if (row.summary && *row.summary)
        {
            const float available = right-(origin.x+expandWidth+4*scale);
            if (available > 24.0f*scale)
            {
                draw->PushClipRect({right-available,origin.y},{right,end.y-1},true);
                const auto text = ImGui::CalcTextSize(row.summary);
                const auto color = ImGui::ColorConvertU32ToFloat4(ImGui::GetColorU32(ImGuiCol_TextDisabled));
                draw->AddText({right-(std::min)(text.x,available),origin.y+(height-ImGui::GetFontSize())/2},
                    ImGui::ColorConvertFloat4ToU32({color.x,color.y,color.z,row.enabled && !row.muted ? 0.9f : 0.45f}),row.summary);
                draw->PopClipRect();
            }
        }
    }
    if (row.expandable)
    {
        ImGui::SetCursorScreenPos(origin);
        result.expanded = ImGui::InvisibleButton("##Expand",{tool,height});
        const ImVec2 center{origin.x+tool/2,origin.y+height/2};
        const float r = 3.5f*scale;
        if (row.expanded) draw->AddTriangleFilled({center.x-r,center.y-r/2},{center.x+r,center.y-r/2},{center.x,center.y+r},ImGui::GetColorU32(ImGuiCol_TextDisabled));
        else draw->AddTriangleFilled({center.x-r/2,center.y-r},{center.x-r/2,center.y+r},{center.x+r,center.y},ImGui::GetColorU32(ImGuiCol_TextDisabled));
    }
    if (row.inherited || row.issue)
    {
        const ImVec2 center{end.x-rightWidth+9*scale,origin.y+height/2};
        draw->AddCircle(center,5*scale,row.issue ? IM_COL32(235,160,65,255) : IM_COL32(120,155,184,255),12,1.0f*scale);
        draw->AddText({center.x-2*scale,center.y-ImGui::GetFontSize()/2},row.issue ? IM_COL32(235,160,65,255) : IM_COL32(120,155,184,255),row.issue ? "!" : "i");
    }
    if (row.remove)
    {
        const float right = end.x-(row.enable || row.add ? tool : 0);
        ImGui::SetCursorScreenPos({right-tool,origin.y});
        result.removed = ImGui::InvisibleButton("##Remove",{tool,height});
        const ImVec2 center{right-tool/2,origin.y+height/2};
        const auto color = ImGui::GetColorU32(ImGui::IsItemHovered() ? ImGuiCol_Text : ImGuiCol_TextDisabled);
        draw->AddRect({center.x-4*scale,center.y-2*scale},{center.x+4*scale,center.y+6*scale},color,1*scale);
        draw->AddLine({center.x-6*scale,center.y-4*scale},{center.x+6*scale,center.y-4*scale},color,1.5f*scale);
        draw->AddLine({center.x-2*scale,center.y-6*scale},{center.x+2*scale,center.y-6*scale},color,1.5f*scale);
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Delete module");
    }
    if (row.enable || row.add)
    {
        ImGui::SetCursorScreenPos({end.x-tool,origin.y});
        const bool pressed = ImGui::InvisibleButton(row.enable ? "##Enabled" : "##Add",{tool,height});
        const ImVec2 center{end.x-tool/2,origin.y+height/2};
        if (row.enable)
        {
            const float r = 6*scale;
            draw->AddRectFilled({center.x-r,center.y-r},{center.x+r,center.y+r},IM_COL32(12,14,16,255),2*scale);
            draw->AddRect({center.x-r,center.y-r},{center.x+r,center.y+r},IM_COL32(65,69,73,255),2*scale);
            if (row.enabled)
            {
                draw->AddLine({center.x-4*scale,center.y},{center.x-scale,center.y+3*scale},IM_COL32(36,153,235,255),2*scale);
                draw->AddLine({center.x-scale,center.y+3*scale},{center.x+5*scale,center.y-4*scale},IM_COL32(36,153,235,255),2*scale);
            }
            result.enabled = pressed;
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s module",row.enabled ? "Disable" : "Enable");
        }
        else
        {
            const auto accent = row.accent ? row.accent : IM_COL32(150,150,150,255);
            draw->AddCircle(center,6*scale,accent,16,1.5f*scale);
            draw->AddLine({center.x-3*scale,center.y},{center.x+3*scale,center.y},accent,1.5f*scale);
            draw->AddLine({center.x,center.y-3*scale},{center.x,center.y+3*scale},accent,1.5f*scale);
            result.add = pressed;
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Add module");
        }
    }
    ImGui::SetCursorScreenPos({origin.x,origin.y+height});
    ImGui::EndGroup();
    ImGui::PopStyleVar();
    // Preserve the row's identity for existing context menus and drag sources.
    item.Rect = ImRect(origin,end); item.NavRect = item.Rect;
    GImGui->LastItemData = item;
    return result;
}

void VfxModulePicker::Open(std::string emitter,std::string stack)
{
    m_emitter = std::move(emitter); m_stack = std::move(stack);
    m_search.fill(0); m_index = 0; m_open = true; m_focus = true; m_error.clear();
}
bool VfxModulePicker::Draw(VfxSession& session)
{
    if (m_open) { ImGui::OpenPopup("Add Module"); m_open = false; }
    const float scale = ImGui::GetFontSize()/13.0f;
    ImGui::SetNextWindowSize({440*scale,420*scale},ImGuiCond_Appearing);
    bool changed = false;
    if (!ImGui::BeginPopup("Add Module")) return false;
    const auto& system = session.EditingSystem();
    const author::Stack* stack = nullptr;
    for (const auto& emitter : system.emitters) if (emitter.settings.id == m_emitter)
        for (const auto& candidate : emitter.stacks) if (candidate.id == m_stack || VfxSourceId(candidate) == m_stack) stack = &candidate;
    if (!stack) { ImGui::TextDisabled("This stage is no longer available."); ImGui::EndPopup(); return false; }
    ImGui::TextUnformatted(author::StageName(stack->stage));
    if (m_focus) { ImGui::SetKeyboardFocusHere(); m_focus = false; }
    ImGui::SetNextItemWidth(-1);
    if (ImGui::InputTextWithHint("##SearchModules","Search modules...",m_search.data(),m_search.size())) m_index = 0;
    ImGui::Checkbox("Show deprecated",&m_deprecated);
    const auto choices = FindVfxModules(session.Library(),stack->stage,m_search.data(),m_deprecated);
    m_index = std::clamp(m_index,0,(std::max)(0,static_cast<int>(choices.size())-1));
    if (ImGui::IsKeyPressed(ImGuiKey_DownArrow)) m_index = (std::min)(m_index+1,(std::max)(0,static_cast<int>(choices.size())-1));
    if (ImGui::IsKeyPressed(ImGuiKey_UpArrow)) m_index = (std::max)(0,m_index-1);
    const bool enter = ImGui::IsKeyPressed(ImGuiKey_Enter,false) || ImGui::IsKeyPressed(ImGuiKey_KeypadEnter,false);
    const auto add = [&](const VfxModuleChoice& choice)
    {
        if (session.AddModule(m_emitter,m_stack,choice.id,choice.asset)) { changed = true; ImGui::CloseCurrentPopup(); }
        else m_error = session.ActionError();
    };
    if (ImGui::BeginChild("Modules",{0,-24*scale},ImGuiChildFlags_None))
    {
        std::string category;
        for (size_t index = 0; index < choices.size(); ++index)
        {
            const auto& choice = choices[index];
            if (choice.category != category) { category = choice.category; ImGui::SeparatorText(category.c_str()); }
            ImGui::PushID(choice.id.c_str());
            if (ImGui::Selectable(choice.name.c_str(),static_cast<int>(index) == m_index)) add(choice);
            if (ImGui::IsItemHovered())
            {
                const auto& io = ImGui::GetIO();
                if (io.MouseDelta.x != 0 || io.MouseDelta.y != 0 || ImGui::IsMouseClicked(ImGuiMouseButton_Left)) m_index = static_cast<int>(index);
                ImGui::SetTooltip("%s%s\n%s",choice.name.c_str(),choice.asset ? " (asset)" : "",choice.description.c_str());
            }
            ImGui::PopID();
        }
        if (choices.empty()) ImGui::TextDisabled("No compatible modules found.");
    }
    ImGui::EndChild();
    if (enter && !choices.empty() && !changed) add(choices[m_index]);
    if (!m_error.empty()) ImGui::TextColored({1,0.45f,0.3f,1},"%s",m_error.c_str());
    else ImGui::TextDisabled("%zu compatible modules",choices.size());
    ImGui::EndPopup();
    return changed;
}
}
