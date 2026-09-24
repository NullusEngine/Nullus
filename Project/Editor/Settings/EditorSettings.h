#pragma once

#include "Reflection/Macros.h"
#include "Reflection/RuntimeMetaProperties.h"
#include "Rendering/LargeSceneSettings.h"
#include "Rendering/Settings/EngineDiagnosticsSettings.h"
#include "Settings/EditorSettingsRegistry.h"
#include "Project/Editor/Settings/EditorSettings.generated.h"

#include <Math/Color.h>
#include <string>

namespace NLS::Editor::Settings
{
    STRUCT(EditorDebugDrawSettingsObject)
    {
        GENERATED_BODY()
        PROPERTY()
        bool debugDrawEnabled = true;
        PROPERTY()
        bool debugDrawGrid = true;
        PROPERTY()
        bool debugDrawBounds = false;
        PROPERTY()
        bool debugDrawCamera = true;
        PROPERTY()
        bool debugDrawLighting = true;
        PROPERTY()
        float lightBillboardScale = 0.5f;
    };
}

namespace NLS::Editor::Settings
{
    STRUCT(EditorSceneToolSettingsObject)
    {
        GENERATED_BODY()
        PROPERTY()
        float translationSnapUnit = 1.0f;
        PROPERTY()
        float rotationSnapUnit = 15.0f;
        PROPERTY()
        float scalingSnapUnit = 1.0f;
    };
}

namespace NLS::Editor::Settings
{
    STRUCT(EditorRenderingSettingsObject)
    {
        GENERATED_BODY()
        PROPERTY(
            DisplayName("Scene View Static Cache"),
            Tooltip("Skips Scene View rendering while camera, scene, and editor state are unchanged. Keep disabled when measuring real render performance."))
        bool enableSceneViewStaticFrameCache = false;
        PROPERTY()
        bool enableLightGrid = true;
    };
}

namespace NLS::Editor::Settings
{
    STRUCT(EditorRuntimeSettingsObject)
    {
        GENERATED_BODY()
        // Launch-only options (threaded rendering, swapchain vsync, RenderDoc
        // startup capture) intentionally live in the project file, because the
        // launcher startup panel owns everything that needs a restart.
        PROPERTY()
        bool enablePowerSavingIdlePacing = false;
        PROPERTY()
        bool renderDocEnabled = false;
        PROPERTY()
        bool renderDocAutoOpenReplayUI = false;
    };
}

namespace NLS::Editor::Settings
{
    STRUCT(EditorViewSettingsObject)
    {
        GENERATED_BODY()
        PROPERTY(
            DisplayName("Scene View Camera Speed"),
            Range(1.0f, 50.0f),
            Tooltip("Fly speed of the Scene View camera."))
        float sceneViewCameraSpeed = 15.0f;
        PROPERTY(
            DisplayName("Asset View Camera Speed"),
            Range(1.0f, 50.0f),
            Tooltip("Fly speed of the Asset View camera."))
        float assetViewCameraSpeed = 15.0f;
        PROPERTY(DisplayName("Scene View Background"), ColorUsage)
        Maths::Color sceneViewBackground = Maths::Color(0.098f, 0.098f, 0.098f, 1.0f);
        PROPERTY(DisplayName("Scene View Grid"), ColorUsage)
        Maths::Color sceneViewGrid = Maths::Color(0.176f, 0.176f, 0.176f, 1.0f);
        PROPERTY(DisplayName("Asset View Background"), ColorUsage)
        Maths::Color assetViewBackground = Maths::Color(0.098f, 0.098f, 0.098f, 1.0f);
        PROPERTY(DisplayName("Asset View Grid"), ColorUsage)
        Maths::Color assetViewGrid = Maths::Color(0.176f, 0.176f, 0.176f, 1.0f);
    };
}

namespace NLS::Editor::Settings
{
    STRUCT(EditorLargeSceneSettingsObject)
    {
        GENERATED_BODY()
        PROPERTY()
        bool enableSpatialIndex = true;
        PROPERTY()
        bool enableParallelVisibility = true;
        PROPERTY()
        bool enableLOD = true;
        PROPERTY()
        bool enableHLOD = false;
        PROPERTY()
        bool enableHZBOcclusion = false;
        PROPERTY()
        int maxVisibilityJobs = 0;
        PROPERTY()
        int parallelVisibilityPrimitiveThreshold = 1024;
        PROPERTY()
        int parallelVisibilityPrimitivesPerTask = 128;
        PROPERTY()
        double staticRebuildDirtyRatio = 0.20;
        PROPERTY()
        int staticRebuildBudgetUs = 0;
        PROPERTY()
        int streamingCpuBudgetUs = 1000;
        PROPERTY()
        int streamingGpuUploadBudgetBytes = 16 * 1024 * 1024;
        PROPERTY()
        int streamingIoBudgetBytes = 32 * 1024 * 1024;
        PROPERTY()
        int streamingCpuMemoryBudgetBytes = 1024 * 1024 * 1024;
        PROPERTY()
        int streamingGpuMemoryBudgetBytes = 1024 * 1024 * 1024;
        PROPERTY()
        int maxOcclusionHistoryAge = 2;
    };
}

namespace NLS::Editor::Settings
{
	/**
	* Accessible from anywhere editor settings
	*/
	class EditorSettings
	{
	public:
		/**
		* No construction possible
		*/
		EditorSettings() = delete;

        static EditorDebugDrawSettingsObject& GetDebugDrawSettingsObject();
        static EditorSceneToolSettingsObject& GetSceneToolSettingsObject();
        static EditorRenderingSettingsObject& GetRenderingSettingsObject();
        static EditorViewSettingsObject& GetViewSettingsObject();
        static EditorRuntimeSettingsObject& GetRuntimeSettingsObject();
        static EditorLargeSceneSettingsObject& GetLargeSceneSettingsObject();
        static Render::Settings::RenderDocSettings BuildRenderDocSettings();
        static Render::Settings::EngineDiagnosticsSettings BuildDiagnosticsSettings();
        static Engine::Rendering::LargeSceneSettings BuildLargeSceneSettings();
        static void RegisterSettingObjects(EditorSettingsRegistry& p_registry);
	};
}
