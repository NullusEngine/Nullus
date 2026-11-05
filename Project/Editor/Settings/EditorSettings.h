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
    STRUCT(EditorGameViewSettingsObject)
    {
        GENERATED_BODY()
        PROPERTY() int display = 0;
        PROPERTY() int sizeMode = 0;
        PROPERTY() int width = 1920;
        PROPERTY() int height = 1080;
        PROPERTY() bool fit = true;
        PROPERTY() float scale = 1.0f;
        PROPERTY() bool lowResolution = false;
        PROPERTY() bool maximizeOnPlay = false;
        PROPERTY() bool muteAudio = false;
        PROPERTY() bool editorShortcuts = false;
        PROPERTY() bool stats = false;
        PROPERTY() bool gizmos = false;
        PROPERTY() bool cameraGizmos = true;
        PROPERTY() bool lightGizmos = true;
        PROPERTY() bool colliderGizmos = true;
        PROPERTY() bool boundsGizmos = false;
        PROPERTY() bool warnNoCamera = true;
        PROPERTY() bool clearInEdit = true;
        PROPERTY() bool vsync = false;
        PROPERTY() Maths::Color playTint = Maths::Color(0.16f, 0.38f, 0.65f, 1.0f);
    };

    STRUCT(EditorConsoleSettingsObject)
    {
        GENERATED_BODY()
        PROPERTY() bool clearOnPlay = true;
        PROPERTY() bool clearOnBuild = false;
        PROPERTY() bool clearOnRecompile = false;
        PROPERTY() bool collapse = false;
        PROPERTY() bool errorPause = false;
        PROPERTY() bool showTimestamp = true;
        PROPERTY() bool messages = true;
        PROPERTY() bool warnings = true;
        PROPERTY() bool errors = true;
        PROPERTY() bool csharp = true;
        PROPERTY() bool lua = true;
        PROPERTY() bool build = true;
        PROPERTY() int lineCount = 2;
        PROPERTY() float detailRatio = 0.30f;
        PROPERTY() int messageStack = 1;
        PROPERTY() int warningStack = 1;
        PROPERTY() int errorStack = 1;
    };

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
        PROPERTY()
        bool debugDrawCameraIcons = true;
        PROPERTY()
        bool debugDrawLightIcons = true;
        PROPERTY()
        bool icons3D = true;
        PROPERTY()
        bool fadeGizmos = false;
    };
}

namespace NLS::Editor::Settings
{
    STRUCT(EditorSceneViewSettingsObject)
    {
        GENERATED_BODY()
        PROPERTY(DisplayName("Draw Mode"), Range(0, 12), Tooltip("Shading, buffers, render paths, alpha, overdraw, mipmaps and material validation. Distance fields are session-only."))
        int drawMode = 0;
        PROPERTY(DisplayName("Show Grid"))
        bool showGrid = true;
        PROPERTY(DisplayName("Always Snap"))
        bool snapEnabled = false;
        PROPERTY(DisplayName("Scene Lighting"))
        bool sceneLighting = true;
        PROPERTY(DisplayName("Effects"))
        bool effectsEnabled = true;
        PROPERTY(DisplayName("Always Refresh"))
        bool alwaysRefresh = true;
        PROPERTY(DisplayName("Post Processing"))
        bool postProcessing = true;
        PROPERTY(DisplayName("Scene Visibility"))
        bool sceneVisibility = true;
        PROPERTY(DisplayName("Show Skybox"))
        bool showSkybox = true;
        PROPERTY(DisplayName("Show Particles"))
        bool showParticles = true;
        PROPERTY(DisplayName("Show Gizmos"))
        bool showGizmos = true;
        PROPERTY(DisplayName("Camera Gizmos"))
        bool showCameras = true;
        PROPERTY(DisplayName("Light Gizmos"))
        bool showLights = true;
        PROPERTY(DisplayName("Camera Icons"))
        bool showCameraIcons = true;
        PROPERTY(DisplayName("Light Icons"))
        bool showLightIcons = true;
        PROPERTY(DisplayName("3D Icons"))
        bool icons3D = true;
        PROPERTY(DisplayName("Fade Gizmos"))
        bool fadeGizmos = false;
        PROPERTY(DisplayName("Selection Wire"))
        bool showSelectionWire = false;
        PROPERTY(DisplayName("Bounds Gizmos"))
        bool showBounds = false;
        PROPERTY(DisplayName("Selection Outline"))
        bool showSelectionOutline = true;
        PROPERTY(DisplayName("Light Icon Size"), Range(0.01f, 5.0f))
        float lightBillboardScale = 0.5f;
        PROPERTY(DisplayName("Dynamic Clipping"))
        bool cameraDynamicClipping = false;
        PROPERTY(DisplayName("Occlusion Culling"))
        bool cameraOcclusionCulling = true;
        PROPERTY(DisplayName("Camera Easing"))
        bool cameraEasing = true;
        PROPERTY(DisplayName("Camera Acceleration"))
        bool cameraAcceleration = false;
        PROPERTY(DisplayName("Minimum Camera Speed"), Range(0.01f, 98.0f))
        float cameraMinSpeed = 1.0f;
        PROPERTY(DisplayName("Maximum Camera Speed"), Range(0.02f, 99.0f))
        float cameraMaxSpeed = 50.0f;
        PROPERTY(DisplayName("Camera Field of View"), Range(1.0f, 179.0f))
        float cameraFov = 60.0f;
        PROPERTY(DisplayName("Camera Near Plane"), Range(0.001f, 1000.0f))
        float cameraNear = 0.1f;
        PROPERTY(DisplayName("Camera Far Plane"), Range(0.01f, 1000000.0f))
        float cameraFar = 5000.0f;
    };
}

namespace NLS::Editor::Settings
{
    inline EditorDebugDrawSettingsObject BuildSceneViewDebugDrawSettings(const EditorSceneViewSettingsObject& view)
    {
        EditorDebugDrawSettingsObject result;
        // Grid and transform handles are independent of the Gizmos master switch.
        result.debugDrawEnabled = true;
        result.debugDrawGrid = view.showGrid;
        result.debugDrawBounds = view.showGizmos && view.showBounds;
        result.debugDrawCamera = view.showGizmos && view.showCameras;
        result.debugDrawLighting = view.showGizmos && view.showLights;
        result.debugDrawCameraIcons = view.showGizmos && view.showCameraIcons;
        result.debugDrawLightIcons = view.showGizmos && view.showLightIcons;
        result.icons3D = view.icons3D;
        result.fadeGizmos = view.fadeGizmos;
        result.lightBillboardScale = view.lightBillboardScale;
        return result;
    }

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
    STRUCT(EditorUIDesignerSettingsObject)
    {
        GENERATED_BODY()
        PROPERTY(DisplayName("Snap to Grid"))
        bool snapToGrid = true;
        PROPERTY(DisplayName("Grid Size"), Range(1.0f, 128.0f))
        float gridSize = 8.0f;
        PROPERTY(DisplayName("Snap to Guides"))
        bool snapToGuides = true;
        PROPERTY(DisplayName("Show Grid"))
        bool showGrid = true;
        PROPERTY(DisplayName("Show Rulers"))
        bool showRulers = true;
        PROPERTY(DisplayName("Wireframe"))
        bool wireframe = false;
        PROPERTY(DisplayName("Show Hidden Elements"))
        bool showHiddenElements = false;
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
        static EditorSceneViewSettingsObject& GetSceneViewSettingsObject();
        static EditorGameViewSettingsObject& GetGameViewSettingsObject();
        static EditorConsoleSettingsObject& GetConsoleSettingsObject();
        static EditorRenderingSettingsObject& GetRenderingSettingsObject();
        static EditorViewSettingsObject& GetViewSettingsObject();
        static EditorUIDesignerSettingsObject& GetUIDesignerSettingsObject();
        static EditorRuntimeSettingsObject& GetRuntimeSettingsObject();
        static EditorLargeSceneSettingsObject& GetLargeSceneSettingsObject();
        static Render::Settings::RenderDocSettings BuildRenderDocSettings();
        static Render::Settings::EngineDiagnosticsSettings BuildDiagnosticsSettings();
        static Engine::Rendering::LargeSceneSettings BuildLargeSceneSettings();
        static void RegisterSettingObjects(EditorSettingsRegistry& p_registry);
	};
}
