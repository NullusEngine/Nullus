#include "Rendering/Assets/TextureEncoder.h"

#include <algorithm>

namespace NLS::Render::Assets
{
bool TextureEncoderRegistry::Register(std::shared_ptr<ITextureEncoder> encoder)
{
    if (encoder == nullptr || encoder->GetId().empty())
        return false;

    const auto existing = std::find_if(
        m_encoders.begin(),
        m_encoders.end(),
        [&encoder](const std::shared_ptr<ITextureEncoder>& candidate)
        {
            return candidate != nullptr && candidate->GetId() == encoder->GetId();
        });
    if (existing != m_encoders.end())
        return false;

    m_encoders.push_back(std::move(encoder));
    return true;
}

const ITextureEncoder* TextureEncoderRegistry::Find(const std::string_view encoderId) const
{
    const auto found = std::find_if(
        m_encoders.begin(),
        m_encoders.end(),
        [encoderId](const std::shared_ptr<ITextureEncoder>& candidate)
        {
            return candidate != nullptr && candidate->GetId() == encoderId;
        });
    return found != m_encoders.end() ? found->get() : nullptr;
}

const ITextureEncoder* TextureEncoderRegistry::FindForFormat(const RHI::TextureFormat format) const
{
    const auto found = std::find_if(
        m_encoders.begin(),
        m_encoders.end(),
        [format](const std::shared_ptr<ITextureEncoder>& candidate)
        {
            return candidate != nullptr && candidate->SupportsFormat(format);
        });
    return found != m_encoders.end() ? found->get() : nullptr;
}
}
