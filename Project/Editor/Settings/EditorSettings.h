#pragma once

#include "Reflection/Macros.h"
#include "Settings/EditorSettingsRegistry.h"
#include "Project/Editor/Settings/EditorSettings.generated.h"

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
        static void RegisterSettingObjects(EditorSettingsRegistry& p_registry);
	};
}
