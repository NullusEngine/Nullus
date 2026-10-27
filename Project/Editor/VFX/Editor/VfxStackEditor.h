#pragma once

#include "VfxSession.h"
#include <imgui.h>
#include <array>
#include <optional>
#include <string_view>

namespace NLS::Editor::VFXTools
{
// The source identity survives inherited-emitter scoping and failed compiles.
std::string_view VfxSourceId(const VFX::Authoring::Stack& stack);
std::string_view VfxSourceId(const VFX::Authoring::Module& module);
bool StoreVfxModuleInput(VFX::Authoring::Emitter& emitter,const VFX::Authoring::Module& module,
    const std::string& name,const VFX::Authoring::Input& input);
// Restores a local module input to its definition default, or removes the
// inherited override so the parent value becomes effective again.
bool ResetVfxModuleInput(VFX::Authoring::Emitter& emitter,const VFX::Authoring::Module& module,
    const std::string& name,const VFX::Authoring::Input& input);

struct VfxModuleChoice
{
    std::string id, name, category, description;
    bool asset = false, deprecated = false;
};
std::vector<VfxModuleChoice> FindVfxModules(const VFX::Authoring::SourceLibrary& library,
    VFX::Authoring::Stage stage, std::string_view search = {}, bool includeDeprecated = false);

// Compact module header summary, mirroring Niagara's stack row text: parameter
// names, curve/dynamic-input markers and the first literal values.
std::string VfxInputSummary(const VFX::Authoring::Module& module,const VFX::Authoring::ModuleDefinition* definition);
// Case-insensitive search across the label, script id, category, description,
// input names and parameter bindings. Every space-separated token must match.
bool MatchesVfxModule(const VFX::Authoring::Module& module,std::string_view label,
    const VFX::Authoring::ModuleDefinition* definition,std::string_view search);

// Both Overview and Details use the same compact hit targets and state colors.
struct VfxStackRow
{
    const char* label = "";
    // Right-aligned dimmed text before the row actions (input summary).
    const char* summary = nullptr;
    // Small chip (module version) drawn before the summary.
    const char* badge = nullptr;
    float width = 250;
    ImU32 accent = 0;
    bool selected = false, enabled = true, expanded = true;
    bool expandable = false, enable = false, add = false, remove = false, muted = false;
    bool header = false, title = false, inherited = false, issue = false;
};
struct VfxStackRowResult
{
    bool selected = false, enabled = false, expanded = false, add = false, removed = false;
    bool doubleClicked = false, context = false;
};
VfxStackRowResult DrawVfxStackRow(const VfxStackRow& row);

class VfxModulePicker final
{
public:
    void Open(std::string emitter, std::string stack);
    // Call outside the node-editor canvas, in the window that requested Open.
    bool Draw(VfxSession& session);
    const std::string& Error() const { return m_error; }
private:
    std::string m_emitter, m_stack, m_error;
    std::array<char,256> m_search{};
    bool m_open = false, m_focus = false, m_deprecated = false;
    int m_index = 0;
};
}
