#pragma once

#include "Rendering/Assets/TextureEncoder.h"

#include <memory>

namespace NLS::Cook
{
constexpr uint32_t kDirectXTexCookEncoderVersion = 1u;

const char* GetDirectXTexCookEncoderToolVersion();
std::shared_ptr<NLS::Render::Assets::ITextureEncoder> CreateDirectXTexCookEncoder();
}
