#pragma once

#include "Reflection/Macros.h"
#include "Reflection/RuntimeMetaProperties.h"
#include "Rendering/Settings/EngineDiagnosticsSettings.h"
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
        PROPERTY()
        bool renderDocEnabled = false;
        PROPERTY()
        bool renderDocAutoOpenReplayUI = false;
        PROPERTY(RequiresRestart)
        int renderDocStartupCaptureAfterFrames = 0;
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
