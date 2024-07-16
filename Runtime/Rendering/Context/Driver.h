#pragma once

#include <string>
#include <array>
#include <memory>

#include "Rendering/Settings/DriverSettings.h"
#include "Rendering/Settings/ERenderingCapability.h"
#include "Rendering/Settings/EPrimitiveMode.h"
#include "Rendering/Settings/ERasterizationMode.h"
#include "Rendering/Settings/EComparaisonAlgorithm.h"
#include "Rendering/Settings/EOperation.h"
#include "Rendering/Settings/ECullFace.h"
#include "Rendering/Settings/ECullingOptions.h"
#include "Rendering/Settings/EPixelDataFormat.h"
#include "Rendering/Settings/EPixelDataType.h"
#include "Rendering/Data/PipelineState.h"
#include "Rendering/Resources/IMesh.h"

#include <Math/Vector4.h>
#include "RenderDef.h"
class DriverImpl;

namespace NLS::Render::Context
{
/**
 * Handles the lifecycle of the underlying graphics context
 */
class NLS_RENDER_API Driver final
{
public:
    /**
     * Creates the driver
     * @param p_driverSettings
     */
    Driver(const Settings::DriverSettings& p_driverSettings);

    /**
     * Destroy the driver
     */
    ~Driver();

    /**
     * Set the viewport
     * @param p_x
     * @param p_y
     * @param p_width
     * @param p_height
     */
    void SetViewport(
        uint32_t p_x,
        uint32_t p_y,
        uint32_t p_width,
        uint32_t p_height);

    /**
     * Clear the screen using the previously defined clear color (With Renderer::SetClearColor()) or by
     * using the OpenGL default one.
     * @param p_colorBuffer
     * @param p_depthBuffer
     * @param p_stencilBuffer
     * @param p_color
     */
    void Clear(
        bool p_colorBuffer,
        bool p_depthBuffer,
        bool p_stencilBuffer,
        const NLS::Maths::Vector4& p_color = NLS::Maths::Vector4(0, 0, 0, 0));

    /**
     * Read a block of pixels from the currently bound framebuffer (or backbuffer).
     * @param p_x
     * @param p_y
     * @param p_width
     * @param p_height
     * @param p_format
     * @param p_type
     * @param p_data
     */
    void ReadPixels(
        uint32_t p_x,
        uint32_t p_y,
        uint32_t p_width,
        uint32_t p_height,
        Settings::EPixelDataFormat p_format,
        Settings::EPixelDataType p_type,
        void* p_data) const;

    /**
     * Draw a mesh
     * @param p_pso
     * @param p_mesh
     * @param p_primitiveMode
     * @param p_instances
     */
    void Draw(
        NLS::Render::Data::PipelineState p_pso,
        const Resources::IMesh& p_mesh,
        Settings::EPrimitiveMode p_primitiveMode = Settings::EPrimitiveMode::TRIANGLES,
        uint32_t p_instances = 1);

    /**
     * Create a pipeline state from the default state
     */
    Data::PipelineState CreatePipelineState() const;

    /**
     * Returns the vendor
     */
    std::string_view GetVendor() const;

    /**
     * Returns details about the current rendering hardware
     */
    std::string_view GetHardware() const;

    /**
     * Returns the current graphics API version
     */
    std::string_view GetVersion() const;

    /**
     * Returns the current shading language version
     */
    std::string_view GetShadingLanguageVersion() const;

private:
    void SetPipelineState(Data::PipelineState p_state);
    void ResetPipelineState();

private:
    std::string m_vendor;
    std::string m_hardware;
    std::string m_version;
    std::string m_shadingLanguageVersion;
    Data::PipelineState m_defaultPipelineState;
    Data::PipelineState m_pipelineState;
};
} // namespace NLS::Render::Context