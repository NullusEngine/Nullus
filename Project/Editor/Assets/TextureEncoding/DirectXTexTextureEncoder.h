#pragma once

#include "Rendering/Assets/TextureEncoder.h"

#include <memory>

namespace NLS::Editor::Assets
{
constexpr uint32_t kDirectXTexTextureEncoderVersion = 1u;

const char* GetDirectXTexTextureEncoderToolVersion();
std::shared_ptr<NLS::Render::Assets::ITextureEncoder> CreateDirectXTexTextureEncoder();
}
