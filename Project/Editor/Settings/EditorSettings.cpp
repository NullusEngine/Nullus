#include "Settings/EditorSettings.h"

#include "Gen/MetaGenerated.h"

#include <Reflection/ReflectionDatabase.h>

#include <algorithm>

namespace NLS::Editor::Settings
{
namespace
{
uint32_t ToUInt32(const int value)
{
    return static_cast<uint32_t>(std::max(value, 0));
}

size_t ToSize(const int value)
{
    return static_cast<size_t>(std::max(value, 0));
}

uint64_t ToUInt64(const int value)
{
    return static_cast<uint64_t>(std::max(value, 0));
}
}

EditorDebugDrawSettingsObject& EditorSettings::GetDebugDrawSettingsObject()
{
    static EditorDebugDrawSettingsObject object;
    return object;
}

EditorSceneToolSettingsObject& EditorSettings::GetSceneToolSettingsObject()
{
    static EditorSceneToolSettingsObject object;
    return object;
}

EditorRenderingSettingsObject& EditorSettings::GetRenderingSettingsObject()
{
    static EditorRenderingSettingsObject object;
    return object;
}

EditorRuntimeSettingsObject& EditorSettings::GetRuntimeSettingsObject()
{
    static EditorRuntimeSettingsObject object;
    return object;
}

EditorLargeSceneSettingsObject& EditorSettings::GetLargeSceneSettingsObject()
{
    static EditorLargeSceneSettingsObject object;
    return object;
}

Render::Settings::RenderDocSettings EditorSettings::BuildRenderDocSettings()
{
    const auto& runtime = GetRuntimeSettingsObject();

    Render::Settings::RenderDocSettings settings;
    settings.enabled = runtime.renderDocEnabled;
    settings.autoOpenReplayUI = runtime.renderDocAutoOpenReplayUI;
    settings.startupCaptureAfterFrames = runtime.renderDocStartupCaptureAfterFrames > 0
        ? static_cast<uint32_t>(runtime.renderDocStartupCaptureAfterFrames)
        : 0u;
    if (settings.startupCaptureAfterFrames > 0u)
        settings.enabled = true;
    return settings;
}

Render::Settings::EngineDiagnosticsSettings EditorSettings::BuildDiagnosticsSettings()
{
    return {};
}

Engine::Rendering::LargeSceneSettings EditorSettings::BuildLargeSceneSettings()
{
    const auto& editor = GetLargeSceneSettingsObject();

    auto settings = Engine::Rendering::LargeSceneSettings::Defaults();
    settings.enableSpatialIndex = editor.enableSpatialIndex;
    settings.enableParallelVisibility = editor.enableParallelVisibility;
    settings.enableLOD = editor.enableLOD;
    settings.enableHLOD = editor.enableHLOD;
    settings.enableHZBOcclusion = editor.enableHZBOcclusion;
    settings.maxVisibilityJobs = ToUInt32(editor.maxVisibilityJobs);
    settings.parallelVisibilityPrimitiveThreshold = ToSize(editor.parallelVisibilityPrimitiveThreshold);
    settings.parallelVisibilityPrimitivesPerTask = ToSize(editor.parallelVisibilityPrimitivesPerTask);
    settings.staticRebuildDirtyRatio = editor.staticRebuildDirtyRatio;
    settings.staticRebuildBudgetUs = ToUInt64(editor.staticRebuildBudgetUs);
    settings.streamingCpuBudgetUs = ToUInt64(editor.streamingCpuBudgetUs);
    settings.streamingGpuUploadBudgetBytes = ToUInt64(editor.streamingGpuUploadBudgetBytes);
    settings.streamingIoBudgetBytes = ToUInt64(editor.streamingIoBudgetBytes);
    settings.streamingCpuMemoryBudgetBytes = ToUInt64(editor.streamingCpuMemoryBudgetBytes);
    settings.streamingGpuMemoryBudgetBytes = ToUInt64(editor.streamingGpuMemoryBudgetBytes);
    settings.maxOcclusionHistoryAge = ToUInt32(editor.maxOcclusionHistoryAge);
    return settings;
}

void EditorSettings::RegisterSettingObjects(EditorSettingsRegistry& p_registry)
{
    NLS_META_GENERATED_LINK_FUNCTION();
    meta::ReflectionDatabase::Instance();
    p_registry.Register({
        "editor.runtime",
        "Runtime",
        "Editor/Runtime",
        EditorSettingPersistenceScope::User,
        [] { return meta::Variant(GetRuntimeSettingsObject(), meta::variant_policy::NoCopy {}); },
        NLS_TYPEOF(EditorRuntimeSettingsObject)
    });
    p_registry.Register({
        "editor.debug-draw",
        "Debug Draw",
        "Editor/Debugging",
        EditorSettingPersistenceScope::User,
        [] { return meta::Variant(GetDebugDrawSettingsObject(), meta::variant_policy::NoCopy {}); },
        NLS_TYPEOF(EditorDebugDrawSettingsObject)
    });
    p_registry.Register({
        "editor.scene-tools",
        "Scene Tools",
        "Editor/Scene View",
        EditorSettingPersistenceScope::User,
        [] { return meta::Variant(GetSceneToolSettingsObject(), meta::variant_policy::NoCopy {}); },
        NLS_TYPEOF(EditorSceneToolSettingsObject)
    });
    p_registry.Register({
        "editor.rendering",
        "Rendering",
        "Editor/Rendering",
        EditorSettingPersistenceScope::User,
        [] { return meta::Variant(GetRenderingSettingsObject(), meta::variant_policy::NoCopy {}); },
        NLS_TYPEOF(EditorRenderingSettingsObject)
    });
    p_registry.Register({
        "editor.large-scene",
        "Large Scene",
        "Editor/Rendering",
        EditorSettingPersistenceScope::User,
        [] { return meta::Variant(GetLargeSceneSettingsObject(), meta::variant_policy::NoCopy {}); },
        NLS_TYPEOF(EditorLargeSceneSettingsObject)
    });
}
}
