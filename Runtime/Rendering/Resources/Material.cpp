#include "Rendering/Resources/Material.h"
#include "Rendering/Buffers/UniformBuffer.h"
#include "Rendering/Resources/Texture.h"

using namespace NLS;
using namespace Maths;
//TODO: Add constructor with a shader reference

NLS::Render::Resources::Material::Material(NLS::Render::Resources::Shader* p_shader)
{
	SetShader(p_shader);
}

void NLS::Render::Resources::Material::SetShader(NLS::Render::Resources::Shader* p_shader)
{
	m_shader = p_shader;

	if (m_shader)
	{
		// TODO: Move that line to Engine Material
		// NLS::Render::Buffers::UniformBuffer::BindBlockToShader(*m_shader, "EngineUBO");
		FillUniform();
	}
	else
	{
		m_uniformsData.clear();
	}
}

void NLS::Render::Resources::Material::FillUniform()
{
	m_uniformsData.clear();

	for (const NLS::Render::Resources::UniformInfo& element : m_shader->uniforms)
		m_uniformsData.emplace(element.name, element.defaultValue);
}

void NLS::Render::Resources::Material::Bind(NLS::Render::Resources::Texture* p_emptyTexture) const
{
	if (HasShader())
	{
		using namespace Maths;
		using namespace NLS::Render::Resources;

		m_shader->Bind();

		int textureSlot = 0;

		for (auto& [name, value] : m_uniformsData)
		{
			auto uniformData = m_shader->GetUniformInfo(name);

			if (uniformData)
			{
				switch (uniformData->type)
				{
				case NLS::Render::Resources::UniformType::UNIFORM_BOOL:			if (value.type() == typeid(bool))		m_shader->SetUniformInt(name, std::any_cast<bool>(value));			break;
				case NLS::Render::Resources::UniformType::UNIFORM_INT:			if (value.type() == typeid(int))		m_shader->SetUniformInt(name, std::any_cast<int>(value));			break;
				case NLS::Render::Resources::UniformType::UNIFORM_FLOAT:		if (value.type() == typeid(float))		m_shader->SetUniformFloat(name, std::any_cast<float>(value));		break;
				case NLS::Render::Resources::UniformType::UNIFORM_FLOAT_VEC2:	if (value.type() == typeid(Vector2))	m_shader->SetUniformVec2(name, std::any_cast<Vector2>(value));		break;
				case NLS::Render::Resources::UniformType::UNIFORM_FLOAT_VEC3:	if (value.type() == typeid(Vector3))	m_shader->SetUniformVec3(name, std::any_cast<Vector3>(value));		break;
				case NLS::Render::Resources::UniformType::UNIFORM_FLOAT_VEC4:	if (value.type() == typeid(Vector4))	m_shader->SetUniformVec4(name, std::any_cast<Vector4>(value));		break;
				case NLS::Render::Resources::UniformType::UNIFORM_SAMPLER_2D:
				{
					if (value.type() == typeid(Texture*))
					{
						if (auto tex = std::any_cast<Texture*>(value); tex)
						{
							tex->Bind(textureSlot);
							m_shader->SetUniformInt(uniformData->name, textureSlot++);
						}
						else if (p_emptyTexture)
						{
							p_emptyTexture->Bind(textureSlot);
							m_shader->SetUniformInt(uniformData->name, textureSlot++);
						}
					}
				}
				}
			}
		}
	}
}

void NLS::Render::Resources::Material::UnBind() const
{
	if (HasShader())
	{
		m_shader->Unbind();
	}
}

NLS::Render::Resources::Shader*& NLS::Render::Resources::Material::GetShader()
{
	return m_shader;
}

bool NLS::Render::Resources::Material::HasShader() const
{
	return m_shader;
}

bool NLS::Render::Resources::Material::IsValid() const
{
	return HasShader();
}

void NLS::Render::Resources::Material::SetBlendable(bool p_transparent)
{
	m_blendable = p_transparent;
}

void NLS::Render::Resources::Material::SetBackfaceCulling(bool p_backfaceCulling)
{
	m_backfaceCulling = p_backfaceCulling;
}

void NLS::Render::Resources::Material::SetFrontfaceCulling(bool p_frontfaceCulling)
{
	m_frontfaceCulling = p_frontfaceCulling;
}

void NLS::Render::Resources::Material::SetDepthTest(bool p_depthTest)
{
	m_depthTest = p_depthTest;
}

void NLS::Render::Resources::Material::SetDepthWriting(bool p_depthWriting)
{
	m_depthWriting = p_depthWriting;
}

void NLS::Render::Resources::Material::SetColorWriting(bool p_colorWriting)
{
	m_colorWriting = p_colorWriting;
}

void NLS::Render::Resources::Material::SetGPUInstances(int p_instances)
{
	m_gpuInstances = p_instances;
}

bool NLS::Render::Resources::Material::IsBlendable() const
{
	return m_blendable;
}

bool NLS::Render::Resources::Material::HasBackfaceCulling() const
{
	return m_backfaceCulling;
}

bool NLS::Render::Resources::Material::HasFrontfaceCulling() const
{
	return m_frontfaceCulling;
}

bool NLS::Render::Resources::Material::HasDepthTest() const
{
	return m_depthTest;
}

bool NLS::Render::Resources::Material::HasDepthWriting() const
{
	return m_depthWriting;
}

bool NLS::Render::Resources::Material::HasColorWriting() const
{
	return m_colorWriting;
}

int NLS::Render::Resources::Material::GetGPUInstances() const
{
	return m_gpuInstances;
}

const NLS::Render::Data::StateMask NLS::Render::Resources::Material::GenerateStateMask() const
{
    Data::StateMask stateMask;
	stateMask.depthWriting = m_depthWriting;
	stateMask.colorWriting = m_colorWriting;
	stateMask.blendable = m_blendable;
	stateMask.depthTest = m_depthTest;
	stateMask.frontfaceCulling = m_frontfaceCulling;
	stateMask.backfaceCulling = m_backfaceCulling;
	return stateMask;
}

std::map<std::string, std::any>& NLS::Render::Resources::Material::GetUniformsData()
{
	return m_uniformsData;
}
