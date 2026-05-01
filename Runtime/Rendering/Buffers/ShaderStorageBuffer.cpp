#include "Rendering/Buffers/ShaderStorageBuffer.h"

NLS::Render::Buffers::ShaderStorageBuffer::ShaderStorageBuffer()
{
}

NLS::Render::Buffers::ShaderStorageBuffer::~ShaderStorageBuffer()
{
	m_explicitBuffer.reset();
}
