#include "Rendering/Resources/Material.h"
#include "Rendering/Buffers/UniformBuffer.h"
#include "Rendering/Resources/BindingSet.h"
#include "Rendering/Resources/Texture2D.h"
#include "Rendering/Resources/TextureCube.h"
#include "Rendering/Backend/OpenGL/OpenGLShaderProgramAPI.h"

namespace
{
	using OpenGLShaderProgramAPI = NLS::Render::Backend::OpenGLShaderProgramAPI;
	using NLS::Maths::Matrix4;
	using NLS::Maths::Vector2;
	using NLS::Maths::Vector3;
	using NLS::Maths::Vector4;

	void SetUniformValue(int32_t location, bool value)
	{
		OpenGLShaderProgramAPI::SetUniformInt(location, value ? 1 : 0);
	}

	void SetUniformValue(int32_t location, int value)
	{
		OpenGLShaderProgramAPI::SetUniformInt(location, value);
	}

	void SetUniformValue(int32_t location, float value)
	{
		OpenGLShaderProgramAPI::SetUniformFloat(location, value);
	}

	void SetUniformValue(int32_t location, const Vector2& value)
	{
		OpenGLShaderProgramAPI::SetUniformVec2(location, value.x, value.y);
	}

	void SetUniformValue(int32_t location, const Vector3& value)
	{
		OpenGLShaderProgramAPI::SetUniformVec3(location, value.x, value.y, value.z);
	}

	void SetUniformValue(int32_t location, const Vector4& value)
	{
		OpenGLShaderProgramAPI::SetUniformVec4(location, value.x, value.y, value.z, value.w);
	}

	void SetUniformValue(int32_t location, const Matrix4& value)
	{
		OpenGLShaderProgramAPI::SetUniformMat4(location, &value.data[0]);
	}
}

namespace NLS::Render::Resources
{
using namespace Maths;

Material::Material(Shader* p_shader)
{
	SetShader(p_shader);
}

void Material::SetShader(Shader* p_shader)
{
	m_shader = p_shader;

	if (m_shader)
	{
		FillUniform();
	}
	else
	{
		m_parameterBlock.Clear();
	}
}

void Material::FillUniform()
{
	m_parameterBlock.Clear();
	m_bindingLayout.bindings.clear();
	m_bindingSet.Clear();

	for (const UniformInfo& element : m_shader->uniforms)
		m_parameterBlock.Set(element.name, element.defaultValue);

	RebuildBindingLayout();
	RebuildBindingSet();
}

void Material::SyncParameterLayout()
{
	FillUniform();
}

void Material::RebuildBindingLayout()
{
	m_bindingLayout.bindings.clear();

	if (!m_shader)
		return;

	int32_t sampledTextureSlot = 0;
	for (const auto& property : m_shader->GetReflection().properties)
	{
		if (property.kind != ShaderResourceKind::SampledTexture)
			continue;

		m_bindingLayout.bindings.push_back({
			property.name,
			property.kind,
			property.type,
			sampledTextureSlot++
		});
	}
}

void Material::RebuildBindingSet() const
{
	m_bindingSet.SetLayout(m_bindingLayout);

	for (const auto& binding : m_bindingLayout.bindings)
	{
		const auto* parameter = m_parameterBlock.TryGet(binding.name);
		if (!parameter)
			continue;

		switch (binding.type)
		{
		case UniformType::UNIFORM_SAMPLER_2D:
			if (parameter->type() == typeid(Texture2D*))
				m_bindingSet.SetTexture(binding.name, std::any_cast<Texture2D*>(*parameter));
			break;
		case UniformType::UNIFORM_SAMPLER_CUBE:
			if (parameter->type() == typeid(TextureCube*))
				m_bindingSet.SetTexture(binding.name, std::any_cast<TextureCube*>(*parameter));
			break;
		default:
			break;
		}
	}
}

void Material::Bind(Texture2D* p_emptyTexture) const
{
	if (!HasShader())
		return;

	m_shader->Bind();
	RebuildBindingSet();

	for (const auto& property : m_shader->GetReflection().properties)
	{
		auto uniformData = m_shader->GetUniformInfo(property.name);
		if (!uniformData || uniformData->location < 0)
			continue;

		const auto* valuePtr = m_parameterBlock.TryGet(property.name);
		if (!valuePtr)
			continue;

		const auto& value = *valuePtr;

		switch (uniformData->type)
		{
		case UniformType::UNIFORM_BOOL:			if (value.type() == typeid(bool))		SetUniformValue(uniformData->location, std::any_cast<bool>(value)); break;
		case UniformType::UNIFORM_INT:			if (value.type() == typeid(int))		SetUniformValue(uniformData->location, std::any_cast<int>(value)); break;
		case UniformType::UNIFORM_FLOAT:		if (value.type() == typeid(float))		SetUniformValue(uniformData->location, std::any_cast<float>(value)); break;
		case UniformType::UNIFORM_FLOAT_VEC2:	if (value.type() == typeid(Vector2))	SetUniformValue(uniformData->location, std::any_cast<Vector2>(value)); break;
		case UniformType::UNIFORM_FLOAT_VEC3:	if (value.type() == typeid(Vector3))	SetUniformValue(uniformData->location, std::any_cast<Vector3>(value)); break;
		case UniformType::UNIFORM_FLOAT_VEC4:	if (value.type() == typeid(Vector4))	SetUniformValue(uniformData->location, std::any_cast<Vector4>(value)); break;
		case UniformType::UNIFORM_FLOAT_MAT4:	if (value.type() == typeid(Matrix4))	SetUniformValue(uniformData->location, std::any_cast<Matrix4>(value)); break;
		case UniformType::UNIFORM_SAMPLER_2D:
		{
			const auto* binding = m_bindingSet.Find(property.name);
			const auto* boundTexture = m_bindingSet.GetTexture(property.name);
			if (auto tex = dynamic_cast<const Texture2D*>(boundTexture); tex)
			{
				const auto textureSlot = binding ? static_cast<uint32_t>(binding->slot) : 0u;
				tex->Bind(textureSlot);
				SetUniformValue(uniformData->location, static_cast<int>(textureSlot));
			}
			else if (p_emptyTexture)
			{
				const auto textureSlot = binding ? static_cast<uint32_t>(binding->slot) : 0u;
				p_emptyTexture->Bind(textureSlot);
				SetUniformValue(uniformData->location, static_cast<int>(textureSlot));
			}
			break;
		}
		case UniformType::UNIFORM_SAMPLER_CUBE:
		{
			const auto* binding = m_bindingSet.Find(property.name);
			const auto* boundTexture = m_bindingSet.GetTexture(property.name);
			if (auto tex = dynamic_cast<const TextureCube*>(boundTexture); tex)
			{
				const auto textureSlot = binding ? static_cast<uint32_t>(binding->slot) : 0u;
				tex->Bind(textureSlot);
				SetUniformValue(uniformData->location, static_cast<int>(textureSlot));
			}
			break;
		}
		}
	}
}

void Material::UnBind() const
{
	if (HasShader())
	{
		m_shader->Unbind();
	}
}

Shader*& Material::GetShader()
{
	return m_shader;
}

bool Material::HasShader() const
{
	return m_shader;
}

bool Material::IsValid() const
{
	return HasShader();
}

void Material::SetBlendable(bool p_transparent) { m_blendable = p_transparent; }
void Material::SetBackfaceCulling(bool p_backfaceCulling) { m_backfaceCulling = p_backfaceCulling; }
void Material::SetFrontfaceCulling(bool p_frontfaceCulling) { m_frontfaceCulling = p_frontfaceCulling; }
void Material::SetDepthTest(bool p_depthTest) { m_depthTest = p_depthTest; }
void Material::SetDepthWriting(bool p_depthWriting) { m_depthWriting = p_depthWriting; }
void Material::SetColorWriting(bool p_colorWriting) { m_colorWriting = p_colorWriting; }
void Material::SetGPUInstances(int p_instances) { m_gpuInstances = p_instances; }
bool Material::IsBlendable() const { return m_blendable; }
bool Material::HasBackfaceCulling() const { return m_backfaceCulling; }
bool Material::HasFrontfaceCulling() const { return m_frontfaceCulling; }
bool Material::HasDepthTest() const { return m_depthTest; }
bool Material::HasDepthWriting() const { return m_depthWriting; }
bool Material::HasColorWriting() const { return m_colorWriting; }
int Material::GetGPUInstances() const { return m_gpuInstances; }

const Data::StateMask Material::GenerateStateMask() const
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

RHI::GraphicsPipelineDesc Material::BuildGraphicsPipelineDesc() const
{
	RHI::GraphicsPipelineDesc desc;
	desc.blendState.enabled = m_blendable;
	desc.blendState.colorWrite = m_colorWriting;
	desc.depthStencilState.depthTest = m_depthTest;
	desc.depthStencilState.depthWrite = m_depthWriting;
	desc.rasterState.culling = m_backfaceCulling || m_frontfaceCulling;
	if (m_backfaceCulling && m_frontfaceCulling)
		desc.rasterState.cullFace = Settings::ECullFace::FRONT_AND_BACK;
	else
		desc.rasterState.cullFace = m_frontfaceCulling
			? Settings::ECullFace::FRONT
			: Settings::ECullFace::BACK;
	desc.gpuInstances = m_gpuInstances;
	return desc;
}

MaterialParameterBlock& Material::GetParameterBlock()
{
	return m_parameterBlock;
}

const MaterialParameterBlock& Material::GetParameterBlock() const
{
	return m_parameterBlock;
}

const ResourceBindingLayout& Material::GetBindingLayout() const
{
	return m_bindingLayout;
}

const BindingSet& Material::GetBindingSet() const
{
	return m_bindingSet;
}

std::map<std::string, std::any>& Material::GetUniformsData()
{
	return m_parameterBlock.Data();
}
}
