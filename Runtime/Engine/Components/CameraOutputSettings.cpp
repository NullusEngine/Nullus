#include "CameraComponent.h"
#include <algorithm>
#include <cmath>
namespace NLS::Engine::Components
{
void CameraComponent::SetTargetDisplay(int value) {targetDisplay=std::clamp(value,0,7);MarkRenderStateChanged();}
void CameraComponent::SetDepth(float value) {if(std::isfinite(value)){depth=value;MarkRenderStateChanged();}}
void CameraComponent::SetViewportRect(Maths::Vector4 value)
{
    if(!std::isfinite(value.x)||!std::isfinite(value.y)||!std::isfinite(value.z)||!std::isfinite(value.w))return;
    value.x=std::clamp(value.x,0.0f,1.0f);value.y=std::clamp(value.y,0.0f,1.0f);
    value.z=std::clamp(value.z,0.0f,1.0f-value.x);value.w=std::clamp(value.w,0.0f,1.0f-value.y);
    viewportRect=value;MarkRenderStateChanged();
}
void CameraComponent::SetClearMode(ECameraClearMode value)
{clearMode=static_cast<ECameraClearMode>(std::clamp(static_cast<int>(value),0,3));MarkRenderStateChanged();}
}
