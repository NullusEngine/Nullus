#pragma once

#include "Reflection/Macros.h"
#include "Reflection/RuntimeMetaProperties.h"
#include "Rendering/Settings/DriverSettings.h"
#include "Settings/EditorSettingsRegistry.h"
#include "Project/Editor/Settings/EditorSettings.generated.h"

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
        bool showGeometryBounds = false;
        PROPERTY()
        bool showLightBounds = false;
        PROPERTY()
        bool showGeometryFrustumCullingInSceneView = false;
        PROPERTY()
        bool showLightFrustumCullingInSceneView = false;
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
        PROPERTY()
        bool enableLightGrid = true;
    };
}

namespace NLS::Editor::Settings
{
    STRUCT(EditorRuntimeSettingsObject)
    {
        GENERATED_BODY()
        PROPERTY(RequiresRestart)
        bool enableThreadedRendering = true;
        PROPERTY(RequiresRestart)
        bool enableRhiDebugValidation = false;
        PROPERTY()
        bool renderDocEnabled = false;
        PROPERTY()
        bool renderDocAutoOpenReplayUI = false;
        PROPERTY(RequiresRestart)
        int renderDocStartupCaptureAfterFrames = 0;
        PROPERTY()
        bool logRenderDrawPath = false;
        PROPERTY()
        bool diagSkipSkyboxDraw = false;
        PROPERTY()
        bool logMaterialBindings = false;
        PROPERTY()
        bool dx12LogMessages = false;
        PROPERTY()
        bool dx12LogFrameFlow = false;
        PROPERTY()
        bool logEditorFps = false;
        PROPERTY()
        bool editorGridSkipPlane = false;
        PROPERTY()
        bool editorGridSkipAxes = false;
        PROPERTY()
        bool editorDisableGridPass = false;
        PROPERTY()
        bool editorDisableDebugCamerasPass = false;
        PROPERTY()
        bool editorDisableDebugLightsPass = false;
        PROPERTY()
        bool editorDisableDebugActorPass = false;
        PROPERTY()
        bool editorDisableDebugDrawPass = false;
        PROPERTY()
        bool editorDisablePickingPass = false;
        PROPERTY()
        bool editorLogScenePicking = false;
        PROPERTY()
        std::string editorValidationFocusView;
        PROPERTY()
        std::string editorValidationExclusiveView;
        PROPERTY()
        std::string editorValidationSelectActor;
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
        static EditorRuntimeSettingsObject& GetRuntimeSettingsObject();
        static Render::Settings::RenderDocSettings BuildRenderDocSettings();
        static Render::Settings::EngineDiagnosticsSettings BuildDiagnosticsSettings();
        static void RegisterSettingObjects(EditorSettingsRegistry& p_registry);
	};
}
