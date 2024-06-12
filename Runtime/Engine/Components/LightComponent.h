#pragma once

#include <Rendering/Entities/Light.h>

#include "Components/Component.h"
#include "EngineDef.h"
namespace NLS::Engine::Components
{
using namespace NLS::Rendering;
using namespace NLS::Rendering::Entities;
/**
 * Base class for any light
 */
class NLS_ENGINE_API LightComponent : public Component
{
public:
    /**
     * Constructor
     * @param p_owner
     */
    LightComponent();

    void OnCreate()override;
    void SetLightType(Settings::ELightType type);
    /**
     * Returns light data
     */
    const NLS::Rendering::Entities::Light* GetData() const;

    /**
     * Returns light color
     */
    const Maths::Vector3& GetColor() const;

    /**
     * Returns light intensity
     */
    float GetIntensity() const;

    /**
     * Defines a new color for the light
     * @param p_color
     */
    void SetColor(const Maths::Vector3& p_color);

    /**
     * Defines the intensity for the light
     * @param p_intensity
     */
    void SetIntensity(float p_intensity);

    /**
     * Returns the light constant
     */
    float GetConstant() const;

    /**
     * Returns the light linear
     */
    float GetLinear() const;

    /**
     * Returns the light quadratic
     */
    float GetQuadratic() const;

    /**
     * Defines the light constant
     * @param p_constant
     */
    void SetConstant(float p_constant);

    /**
     * Defines the light linear
     * @param p_linear
     */
    void SetLinear(float p_linear);

    /**
     * Defines the light quadratic
     * @param p_quadratic
     */
    void SetQuadratic(float p_quadratic);

    /**
     * Returns the light cutoff
     */
    float GetCutoff() const;

    /**
     * Returns the light outercutoff
     */
    float GetOuterCutoff() const;

    /**
     * Defines the light cutoff
     * @param p_cutoff
     */
    void SetCutoff(float p_cutoff);

    /**
     * Defines the light outercutoff
     * @param p_cutoff
     */
    void SetOuterCutoff(float p_outerCutoff);

    /**
     * Returns the radius of the sphere
     */
    float GetRadius() const;

    /**
     * Defines the radius of the sphere
     * @param p_radius
     */
    void SetRadius(float p_radius);

    /**
     * Returns the size of the box
     */
    Maths::Vector3 GetSize() const;

    /**
     * Defines the size of the box
     * @param p_size
     */
    void SetSize(const Maths::Vector3& p_size);

protected:
    Light* m_data = nullptr;
};
} // namespace NLS::Engine::Components