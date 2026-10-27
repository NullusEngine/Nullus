#include "Panels/BuildProfilesPanel.h"

#include <imgui.h>

#include <algorithm>
#include <cstdio>
#include <cstring>

#include "Core/Context.h"
#include "Core/EditorActions.h"
#include "Build/BuildFileUtils.h"
#include "Build/BuildPipelineInternal.h"
#include "Build/IBuildPlatform.h"
#include "Inspector/ReflectedEditHost.h"
#include <ServiceLocator.h>

namespace
{
constexpr size_t kBufferSize = 512u;

void CopyToBuffer(const std::string& value, char (&buffer)[kBufferSize])
{
    std::fill(std::begin(buffer), std::end(buffer), '\0');
    const auto length = (std::min)(value.size(), kBufferSize - 1u);
    if (length > 0u)
        std::memcpy(buffer, value.data(), length);
}

template <size_t N>
bool InputString(const char* label, std::string& value, char (&buffer)[N])
{
    if (ImGui::InputText(label, buffer, N))
    {
        value = buffer;
        return true;
    }
    return false;
}

const char* ModeName(const NLS::Build::BuildMode mode)
{
    return NLS::Build::ToString(mode);
}
} // namespace

namespace NLS::Editor::Panels
{
BuildProfilesPanel::BuildProfilesPanel(
    const std::string& p_title,
    const bool p_opened,
    const UI::PanelWindowSettings& p_windowSettings)
    : UI::PanelWindow(p_title, p_opened, p_windowSettings)
    , m_opened(p_opened)
{
    minSize = {760.0f, 480.0f};
    ReloadProfiles();
}

void BuildProfilesPanel::Open()
{
    m_opened = true;
    enabled = true;
    ReloadProfiles();
}

void BuildProfilesPanel::Close()
{
    m_opened = false;
    enabled = false;
}

std::filesystem::path BuildProfilesPanel::GetProjectRoot() const
{
    return std::filesystem::path(EDITOR_CONTEXT(projectFilePath)).parent_path();
}

std::filesystem::path BuildProfilesPanel::GetProfilesPath() const
{
    return GetProjectRoot() / NLS::Build::BuildProfilesFileName();
}

void BuildProfilesPanel::ReloadProfiles()
{
    m_activePropertyProfile.clear();
    m_profileDocuments.clear();
    m_properties.CancelEdits();
    m_document = {};
    m_diagnostics.clear();
    m_profileIds.clear();

    const auto profilesPath = GetProfilesPath();
    std::error_code existsError;
    if (!std::filesystem::is_regular_file(profilesPath, existsError))
    {
        // A fresh project has no profile document yet. Synthesize the host
        // default so the panel can show and save a working configuration.
        NLS::Build::BuildProfile profile = NLS::Build::MakeDefaultBuildProfile(
            EDITOR_CONTEXT(projectSettings).GetOrDefault<std::string>("executable_name", "Game"),
            GetProjectRoot().stem().string(),
            EDITOR_CONTEXT(projectSettings).GetOrDefault<std::string>("start_scene", std::string {}),
            NLS::Build::Detail::DetectHostPlatform(),
            NLS::Build::Detail::DetectHostArchitecture(),
            NLS::Build::Detail::DetectHostBackend());
        m_document.schemaVersion = 1u;
        m_document.defaultProfileId = profile.id;
        m_selectedProfileId = profile.id;
        m_profileIds.push_back(profile.id);
        m_document.profiles.push_back(std::move(profile));
        m_dirty = true;
        m_statusMessage = "No BuildProfiles.json yet; save to create one.";
        LoadLastReport();
        return;
    }

    if (!NLS::Build::LoadBuildProfiles(profilesPath, m_document, m_diagnostics))
    {
        m_statusMessage = "BuildProfiles.json could not be loaded.";
        return;
    }
    for (const auto& profile : m_document.profiles)
        m_profileIds.push_back(profile.id);
    if (m_selectedProfileId.empty() ||
        std::find(m_profileIds.begin(), m_profileIds.end(), m_selectedProfileId) == m_profileIds.end())
    {
        m_selectedProfileId = m_document.defaultProfileId;
    }
    m_dirty = false;
    m_statusMessage.clear();
    LoadLastReport();
}

void BuildProfilesPanel::LoadLastReport()
{
    m_hasLastReport = false;
    const auto candidates = {
        GetProjectRoot() / "Dist" / m_selectedProfileId / "BuildReport.json",
        GetProjectRoot() / "Dist" / "BuildReport.json"};
    for (const auto& candidate : candidates)
    {
        std::string error;
        if (auto report = NLS::Build::BuildReport::Load(candidate, &error); report.has_value())
        {
            m_lastReport = std::move(*report);
            m_hasLastReport = true;
            return;
        }
    }
}

bool BuildProfilesPanel::ValidateForSave(
    const NLS::Build::BuildProfilesDocument& document,
    std::vector<NLS::Build::BuildDiagnostic>& diagnostics)
{
    diagnostics.clear();
    if (document.profiles.empty())
    {
        diagnostics.push_back({
            NLS::Build::DiagnosticSeverity::Error,
            "build-profiles-empty",
            "At least one build profile is required.",
            {}});
        return false;
    }
    for (const auto& profile : document.profiles)
        NLS::Build::ValidateBuildProfile(profile, diagnostics);
    return std::none_of(
        diagnostics.begin(),
        diagnostics.end(),
        [](const NLS::Build::BuildDiagnostic& diagnostic) { return diagnostic.IsError(); });
}

bool BuildProfilesPanel::SaveProfiles()
{
    for(auto& [id,source]:m_profileDocuments)
    {
        (void)source->EndEdit();
        std::vector<NLS::Build::BuildDiagnostic> pending;
        if(auto value=NLS::Build::ParseBuildProfile(source->Asset(),pending))
            for(auto& profile:m_document.profiles) if(profile.id==id) {profile=std::move(*value);break;}
    }
    std::vector<NLS::Build::BuildDiagnostic> diagnostics;
    if (!ValidateForSave(m_document, diagnostics))
    {
        m_diagnostics = std::move(diagnostics);
        m_statusMessage = "Fix the profile errors before saving.";
        return false;
    }
    if (!NLS::Build::SaveBuildProfiles(GetProfilesPath(), m_document, diagnostics))
    {
        m_diagnostics = std::move(diagnostics);
        m_statusMessage = "BuildProfiles.json could not be written.";
        return false;
    }
    m_diagnostics.clear();
    m_dirty = false;
    for(auto& [id,source]:m_profileDocuments) source->MarkSaved();
    m_statusMessage = "BuildProfiles.json saved.";
    return true;
}

void BuildProfilesPanel::DrawProfileList()
{
    if (ImGui::Button("Reload"))
        ReloadProfiles();
    ImGui::SameLine();
    ImGui::BeginDisabled(!m_dirty);
    if (ImGui::Button("Save"))
        SaveProfiles();
    ImGui::EndDisabled();

    ImGui::Separator();
    ImGui::TextUnformatted("Profiles");
    for (const auto& profile : m_document.profiles)
    {
        const bool selected = profile.id == m_selectedProfileId;
        if (ImGui::Selectable(profile.id.c_str(), selected))
        {
            m_selectedProfileId = profile.id;
            LoadLastReport();
        }
        if (selected && !m_document.defaultProfileId.empty() &&
            m_document.defaultProfileId == profile.id)
        {
            ImGui::SameLine();
            ImGui::TextDisabled("(default)");
        }
    }

    ImGui::Separator();
    char newId[kBufferSize] {};
    CopyToBuffer(m_newProfileId, newId);
    ImGui::InputText("New profile id", newId, kBufferSize);
    m_newProfileId = newId;
    ImGui::BeginDisabled(m_newProfileId.empty());
    if (ImGui::Button("Add profile"))
    {
        if (std::find(m_profileIds.begin(), m_profileIds.end(), m_newProfileId) != m_profileIds.end())
        {
            m_statusMessage = "A profile with that id already exists.";
        }
        else
        {
            auto profile = NLS::Build::MakeDefaultBuildProfile(
                EDITOR_CONTEXT(projectSettings).GetOrDefault<std::string>("executable_name", "Game"),
                GetProjectRoot().stem().string(),
                EDITOR_CONTEXT(projectSettings).GetOrDefault<std::string>("start_scene", std::string {}),
                NLS::Build::Detail::DetectHostPlatform(),
                NLS::Build::Detail::DetectHostArchitecture(),
                NLS::Build::Detail::DetectHostBackend());
            profile.id = m_newProfileId;
            profile.displayName = m_newProfileId;
            m_document.profiles.push_back(std::move(profile));
            m_profileIds.push_back(m_newProfileId);
            m_selectedProfileId = m_newProfileId;
            m_dirty = true;
            m_newProfileId.clear();
        }
    }
    ImGui::EndDisabled();
}

void BuildProfilesPanel::DrawProfileDetails()
{
    auto* profile = const_cast<NLS::Build::BuildProfile*>(
        NLS::Build::FindBuildProfile(m_document, m_selectedProfileId));
    if (profile == nullptr)
        return;

    if(m_activePropertyProfile!=profile->id)
    {
        const auto previous=m_profileDocuments.find(m_activePropertyProfile);
        if(previous!=m_profileDocuments.end())
        {
            (void)previous->second->EndEdit(false);
            std::vector<NLS::Build::BuildDiagnostic> diagnostics;
            if(auto value=NLS::Build::ParseBuildProfile(previous->second->Asset(),diagnostics))
                for(auto& old:m_document.profiles) if(old.id==m_activePropertyProfile) {old=std::move(*value);break;}
        }
        m_properties.CancelEdits();m_activePropertyProfile=profile->id;
    }
    auto& source=m_profileDocuments[profile->id];
    if(!source)
    {
        auto* undo=NLS::Core::ServiceLocator::Contains<Undo::EditorUndoService>() ? &NLS::Core::ServiceLocator::Get<Undo::EditorUndoService>() : nullptr;
        source=std::make_unique<Properties::SourcePropertyDocument>(undo);
        source->New(NLS::Build::SerializeBuildProfile(*profile));
        if(!m_dirty) source->MarkSaved();
    }
    std::vector<NLS::Build::BuildDiagnostic> parseDiagnostics;
    if(auto latest=NLS::Build::ParseBuildProfile(source->Asset(),parseDiagnostics)) *profile=std::move(*latest);
    m_properties.SetLifetime(source->Lifetime());
    Inspector::ReflectedPropertyEdit aggregate;
    const auto field=[&]<class T>(const char* label,T& value,const Inspector::ReflectedPropertyOptions& options=Inspector::ReflectedPropertyOptions{})
    {
        auto edit=m_properties.DrawValue(profile->id+":"+label,label,value,options);
        aggregate.changed|=edit.changed;aggregate.completed|=edit.completed;aggregate.canceled|=edit.canceled;
        return edit.changed;
    };
    const auto choice=[&](const char* label,auto& value,std::initializer_list<const char*> labels)
    {
        int32_t index=static_cast<int32_t>(value);Inspector::ReflectedPropertyOptions options;
        int32_t i=0;for(const auto* name:labels) {Inspector::InspectorPropertyChoice item{std::to_string(i),name};item.typedValue=i++;options.choices.push_back(std::move(item));}
        const bool changed=field(label,index,options);if(changed) value=static_cast<std::remove_reference_t<decltype(value)>>(index);return changed;
    };
    bool changed=false;
    ImGui::Text("Profile: %s",profile->id.c_str());
    changed|=field("Display name",profile->displayName);changed|=field("Product name",profile->productName);
    changed|=field("Company",profile->companyName);changed|=field("Version",profile->version);
    changed|=field("Build number",profile->buildNumber);changed|=field("Executable name",profile->executableName);
    changed|=field("Start scene",profile->startScene);changed|=field("Icon path",profile->iconPath);
    changed|=choice("Mode",profile->mode,{"Development","Shipping"});
    ImGui::SeparatorText("Target");
    if(choice("Platform",profile->target.platform,{"Windows","Linux","macOS","Android","iOS","Web","Unknown"}))
    {profile->target.artifactPlatform=NLS::Build::DefaultArtifactPlatform(profile->target.platform);changed=true;}
    changed|=choice("Architecture",profile->target.architecture,{"x64","arm64","unknown"});
    changed|=choice("Graphics backend",profile->target.graphicsBackend,{"DX12","Vulkan","Metal","WebGPU","OpenGL","None"});
    changed|=field("Minimum OS",profile->target.minimumOSVersion);changed|=field("Artifact platform",profile->target.artifactPlatform);changed|=field("Shader target",profile->target.shaderTarget);
    Inspector::ReflectedPropertyOptions textureOptions;textureOptions.metadata.emplace();textureOptions.metadata->min=0;textureOptions.metadata->max=16384;
    textureOptions.metadata->tooltip="0 keeps the source size; otherwise the cooker clamps the longest texture edge.";
    changed|=field("Max texture size",profile->target.maxTextureSize,textureOptions);

    // Capability report
    NLS::Build::RegisterDefaultBuildPlatforms();
    const auto* adapter = NLS::Build::BuildPlatformRegistry::Get().Find(profile->target);
    if (adapter == nullptr)
    {
        ImGui::TextColored(ImVec4(0.95f, 0.55f, 0.25f, 1.0f), "No adapter: this target is not implemented yet.");
    }
    else
    {
        const auto capability = adapter->GetCapability(profile->target);
        if (!capability.toolchainAvailable)
        {
            ImGui::TextColored(ImVec4(0.95f, 0.55f, 0.25f, 1.0f), "Toolchain: %s", capability.reason.c_str());
        }
        else
        {
            ImGui::TextColored(ImVec4(0.45f, 0.85f, 0.45f, 1.0f), "Adapter ready on this host.");
        }
    }

    // Output and signing
    ImGui::SeparatorText("Output");
    changed|=choice("Format",profile->outputFormat,{"zip","tar.zst","appimage","dmg","apk","aab","ipa","directory"});
    changed|=field("Output directory",profile->outputDirectory);changed|=field("Player template",profile->playerTemplatePath);
    changed|=field("Signing reference",profile->signingProfile);changed|=field("Include debug symbols",profile->includeDebugSymbols);
    changed|=field("Validate launch after build",profile->validateLaunchOnBuild);
    ImGui::SeparatorText("Content");
    for(auto pair:{std::pair{"Keep rules",&profile->keepAssetPaths},std::pair{"Asset packs",&profile->assetPackSelections}})
    {
        NLS::Array<std::string> values(*pair.second);
        if(field(pair.first,values)) {pair.second->assign(values.begin(),values.end());changed=true;}
    }
    class Host final:public Inspector::IReflectedEditHost
    {
    public:
        Host(Properties::SourcePropertyDocument& document,const NLS::Build::BuildProfile& candidate):doc(document),value(candidate){}
        bool BeginPropertyEdit(std::string_view label) override{return doc.BeginEdit(std::string(label));}
        bool PreviewPropertyEdit() override{return doc.PreviewEdit(NLS::Build::SerializeBuildProfile(value));}
        bool CommitPropertyEdit() override{return doc.EndEdit();}
        bool CancelPropertyEdit() override{return doc.EndEdit(false);}
        bool IsPropertyEditOpen() const override{return doc.IsEditing();}
        Properties::SourcePropertyDocument& doc;const NLS::Build::BuildProfile& value;
    } host(*source,*profile);
    if(!Inspector::ApplyReflectedEdit(host,aggregate,"Edit build profile")) m_statusMessage=source->Error();
    (void)Inspector::FinishReflectedEdits(m_properties,host);
    if(aggregate.canceled) {std::vector<NLS::Build::BuildDiagnostic> diagnostics;if(auto restored=NLS::Build::ParseBuildProfile(source->Asset(),diagnostics)) *profile=std::move(*restored);}
    if(changed) m_dirty=true;

    ImGui::SeparatorText("Build");
    if (ImGui::Button("Build with this profile"))
    {
        if (m_dirty && !SaveProfiles())
            return;
        EDITOR_EXEC(Build(false, false, profile->id));
    }
    ImGui::SameLine();
    if (ImGui::Button("Build and run"))
    {
        if (m_dirty && !SaveProfiles())
            return;
        EDITOR_EXEC(Build(true, false, profile->id));
    }
}

void BuildProfilesPanel::DrawActions()
{
    if (!m_diagnostics.empty())
    {
        ImGui::SeparatorText("Diagnostics");
        for (const auto& diagnostic : m_diagnostics)
        {
            const auto color = diagnostic.IsError()
                ? ImVec4(0.95f, 0.35f, 0.35f, 1.0f)
                : ImVec4(0.95f, 0.75f, 0.35f, 1.0f);
            ImGui::TextColored(color, "[%s] %s", diagnostic.code.c_str(), diagnostic.message.c_str());
            if (!diagnostic.detail.empty())
                ImGui::TextDisabled("  %s", diagnostic.detail.c_str());
        }
    }
    if (!m_statusMessage.empty())
        ImGui::TextWrapped("%s", m_statusMessage.c_str());
}

void BuildProfilesPanel::DrawLastReport()
{
    ImGui::SeparatorText("Last build");
    if (!m_hasLastReport)
    {
        ImGui::TextDisabled("No build report found for this profile.");
        return;
    }
    ImGui::Text("%s", m_lastReport.succeeded ? "Succeeded" : "Failed");
    ImGui::Text("Target: %s", m_lastReport.targetId.c_str());
    ImGui::Text("Finished: %s", m_lastReport.finishedUtc.c_str());
    for (const auto& output : m_lastReport.outputs)
        ImGui::BulletText("%s (%llu bytes)", output.path.c_str(), static_cast<unsigned long long>(output.size));
    for (const auto& diagnostic : m_lastReport.diagnostics)
    {
        if (diagnostic.severity == NLS::Build::DiagnosticSeverity::Error)
            ImGui::TextColored(ImVec4(0.95f, 0.35f, 0.35f, 1.0f), "%s", diagnostic.message.c_str());
    }
}

void BuildProfilesPanel::_Draw_Impl()
{
    if (!m_opened)
        return;

    ImGui::BeginChild("##profiles", ImVec2(240.0f, 0.0f), true);
    DrawProfileList();
    ImGui::EndChild();
    ImGui::SameLine();
    ImGui::BeginChild("##details", ImVec2(0.0f, 0.0f), true);
    DrawProfileDetails();
    DrawActions();
    DrawLastReport();
    ImGui::EndChild();
}
} // namespace NLS::Editor::Panels
