#pragma once

#include <cstdint>
#include <string>

#include "RenderDef.h"

namespace NLS::Render::Settings
{
    struct NLS_RENDER_API RenderDocSettings
    {
        bool enabled = false;
        bool autoOpenReplayUI = false;
        uint32_t startupCaptureAfterFrames = 0;
        std::string captureDirectory;
        std::string captureLabel;
    };

    struct NLS_RENDER_API EngineDiagnosticsSettings
    {
        bool logRenderDrawPath = false;
        bool diagSkipSkyboxDraw = false;
        bool logMaterialBindings = false;
        bool dx12LogMessages = false;
        bool dx12LogFrameFlow = false;
        bool logEditorFps = false;
        bool editorGridSkipPlane = false;
        bool editorGridSkipAxes = false;
        bool editorDisableGridPass = false;
        bool editorDisableDebugCamerasPass = false;
        bool editorDisableDebugLightsPass = false;
        bool editorDisableDebugGameObjectPass = false;
        bool editorDisableDebugDrawPass = false;
        bool editorDisablePickingPass = false;
        bool editorLogScenePicking = false;
        bool editorLogSceneCameraInput = false;
        bool editorValidationOpenFrameInfo = false;
        bool editorValidationOpenProfiler = false;
        uint32_t editorValidationTimelineTraceFrames = 0u;
        bool editorValidationDisableHZBOcclusion = false;
        uint32_t editorValidationOcclusionStackCount = 0u;
        std::string editorValidationFocusView;
        std::string editorValidationExclusiveView;
        std::string editorValidationSelectGameObject;
        std::string editorValidationSceneCamera;
        uint32_t editorValidationCameraForwardFrames = 0u;
	        std::string editorValidationCreateAsset;
	        std::string editorValidationAssetBrowserFolder;
	        std::string editorValidationSceneReadbackOutput;
        std::string editorValidationSceneReadbackSummary;
        std::string editorValidationPrefabDragProxySummaryOutput;
        std::string editorThumbnailTelemetrySummaryOutput;
    };
}
