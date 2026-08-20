#pragma once

#include "Math/Vector3.h"

#include "Components/Component.h"
#include "EngineDef.h"
#include "Reflection/Macros.h"
#include "Rendering/Settings/ELightType.h"
#include "Components/LightComponent.generated.h"

namespace NLS::Render::Entities
{
    class Light;
}

namespace NLS::Engine::Components
{
/**
 * Base class for any light
 */
CLASS(NLS_ENGINE_API LightComponent, Scriptable, ComponentMenu("Rendering/Light")) : public Component
{
public:
    GENERATED_BODY()
    /**
     * Constructor
     * @param p_owner
     */
    LightComponent();

    void OnCreate()override;

    FUNCTION(Scriptable)
    void SetLightType(Render::Settings::ELightType type);

    FUNCTION(Scriptable)
    Render::Settings::ELightType GetLightType() const;
    /**
     * Returns light data
     */
    FUNCTION()
    const NLS::Render::Entities::Light* GetData() const;

    /**
     * Returns light color
     */
    FUNCTION(Scriptable)
    const Maths::Vector3& GetColor() const;

    /**
     * Returns light intensity
     */
    FUNCTION(Scriptable)
    float GetIntensity() const;

    /**
     * Defines a new color for the light
     * @param p_color
     */
    FUNCTION(Scriptable)
    void SetColor(const Maths::Vector3& p_color);

    /**
     * Defines the intensity for the light
     * @param p_intensity
     */
    FUNCTION(Scriptable)
    void SetIntensity(float p_intensity);

    /** Returns the explicit light influence range. */
    FUNCTION(Scriptable)
    float GetRange() const;

    /** Sets the explicit light influence range; invalid values become zero. */
    FUNCTION(Scriptable)
    void SetRange(float p_range);

    /**
     * Returns the light cutoff
     */
    FUNCTION()
    float GetCutoff() const;

    /**
     * Returns the light outercutoff
     */
    FUNCTION(Scriptable)
    float GetOuterCutoff() const;

    /**
     * Defines the light cutoff
     * @param p_cutoff
     */
    FUNCTION()
    void SetCutoff(float p_cutoff);

    /**
     * Defines the light outercutoff
     * @param p_cutoff
     */
    FUNCTION(Scriptable)
    void SetOuterCutoff(float p_outerCutoff);

    /**
     * Returns the size of the box
     */
    FUNCTION()
    Maths::Vector3 GetSize() const;

    /**
     * Defines the size of the box
     * @param p_size
     */
    FUNCTION()
    void SetSize(const Maths::Vector3& p_size);

protected:
    Render::Entities::Light* m_data = nullptr;
};
} // namespace NLS::Engine::Components
