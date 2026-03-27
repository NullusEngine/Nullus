#pragma once

#include <glad/glad.h>

#include "Rendering/RHI/RHITypes.h"
#include "Rendering/Resources/UniformType.h"
#include "Rendering/Settings/EAccessSpecifier.h"
#include "Rendering/Settings/EDataType.h"
#include "Rendering/Settings/EPixelDataFormat.h"
#include "Rendering/Settings/EPixelDataType.h"
#include "Rendering/Settings/ETextureFilteringMode.h"

namespace NLS::Render::Backend
{
	struct OpenGLTextureFormatInfo
	{
		GLint internalFormat = GL_RGBA8;
		GLenum format = GL_RGBA;
		GLenum type = GL_UNSIGNED_BYTE;
	};

	inline GLenum ToOpenGLTextureFilter(NLS::Render::Settings::ETextureFilteringMode filter)
	{
		using NLS::Render::Settings::ETextureFilteringMode;

		switch (filter)
		{
		case ETextureFilteringMode::NEAREST: return GL_NEAREST;
		case ETextureFilteringMode::LINEAR: return GL_LINEAR;
		case ETextureFilteringMode::NEAREST_MIPMAP_NEAREST: return GL_NEAREST_MIPMAP_NEAREST;
		case ETextureFilteringMode::LINEAR_MIPMAP_LINEAR: return GL_LINEAR_MIPMAP_LINEAR;
		case ETextureFilteringMode::LINEAR_MIPMAP_NEAREST: return GL_LINEAR_MIPMAP_NEAREST;
		case ETextureFilteringMode::NEAREST_MIPMAP_LINEAR: return GL_NEAREST_MIPMAP_LINEAR;
		default: return GL_LINEAR;
		}
	}

	inline GLenum ToOpenGLDataType(NLS::Render::Settings::EDataType dataType)
	{
		using NLS::Render::Settings::EDataType;

		switch (dataType)
		{
		case EDataType::BYTE: return GL_BYTE;
		case EDataType::UNISGNED_BYTE: return GL_UNSIGNED_BYTE;
		case EDataType::SHORT: return GL_SHORT;
		case EDataType::UNSIGNED_SHORT: return GL_UNSIGNED_SHORT;
		case EDataType::INT: return GL_INT;
		case EDataType::UNSIGNED_INT: return GL_UNSIGNED_INT;
		case EDataType::FLOAT: return GL_FLOAT;
		case EDataType::DOUBLE: return GL_DOUBLE;
		default: return GL_FLOAT;
		}
	}

	inline GLenum ToOpenGLPixelDataFormat(NLS::Render::Settings::EPixelDataFormat format)
	{
		using NLS::Render::Settings::EPixelDataFormat;

		switch (format)
		{
		case EPixelDataFormat::COLOR_INDEX: return GL_RED;
		case EPixelDataFormat::STENCIL_INDEX: return GL_STENCIL_INDEX;
		case EPixelDataFormat::DEPTH_COMPONENT: return GL_DEPTH_COMPONENT;
		case EPixelDataFormat::RED: return GL_RED;
		case EPixelDataFormat::GREEN: return GL_GREEN;
		case EPixelDataFormat::BLUE: return GL_BLUE;
		case EPixelDataFormat::ALPHA: return GL_ALPHA;
		case EPixelDataFormat::RGB: return GL_RGB;
		case EPixelDataFormat::BGR: return GL_BGR;
		case EPixelDataFormat::RGBA: return GL_RGBA;
		case EPixelDataFormat::BGRA: return GL_BGRA;
		case EPixelDataFormat::LUMINANCE: return GL_RED;
		case EPixelDataFormat::LUMINANCE_ALPHA: return GL_RG;
		default: return GL_RGBA;
		}
	}

	inline GLenum ToOpenGLPixelDataType(NLS::Render::Settings::EPixelDataType type)
	{
		using NLS::Render::Settings::EPixelDataType;

		switch (type)
		{
		case EPixelDataType::BYTE: return GL_BYTE;
		case EPixelDataType::UNSIGNED_BYTE: return GL_UNSIGNED_BYTE;
		case EPixelDataType::BITMAP: return GL_UNSIGNED_BYTE;
		case EPixelDataType::SHORT: return GL_SHORT;
		case EPixelDataType::UNSIGNED_SHORT: return GL_UNSIGNED_SHORT;
		case EPixelDataType::INT: return GL_INT;
		case EPixelDataType::UNSIGNED_INT: return GL_UNSIGNED_INT;
		case EPixelDataType::FLOAT: return GL_FLOAT;
		case EPixelDataType::UNSIGNED_BYTE_3_3_2: return GL_UNSIGNED_BYTE_3_3_2;
		case EPixelDataType::UNSIGNED_BYTE_2_3_3_REV: return GL_UNSIGNED_BYTE_2_3_3_REV;
		case EPixelDataType::UNSIGNED_SHORT_5_6_5: return GL_UNSIGNED_SHORT_5_6_5;
		case EPixelDataType::UNSIGNED_SHORT_5_6_5_REV: return GL_UNSIGNED_SHORT_5_6_5_REV;
		case EPixelDataType::UNSIGNED_SHORT_4_4_4_4: return GL_UNSIGNED_SHORT_4_4_4_4;
		case EPixelDataType::UNSIGNED_SHORT_4_4_4_4_REV: return GL_UNSIGNED_SHORT_4_4_4_4_REV;
		case EPixelDataType::UNSIGNED_SHORT_5_5_5_1: return GL_UNSIGNED_SHORT_5_5_5_1;
		case EPixelDataType::UNSIGNED_SHORT_1_5_5_5_REV: return GL_UNSIGNED_SHORT_1_5_5_5_REV;
		case EPixelDataType::UNSIGNED_INT_8_8_8_8: return GL_UNSIGNED_INT_8_8_8_8;
		case EPixelDataType::UNSIGNED_INT_8_8_8_8_REV: return GL_UNSIGNED_INT_8_8_8_8_REV;
		case EPixelDataType::UNSIGNED_INT_10_10_10_2: return GL_UNSIGNED_INT_10_10_10_2;
		case EPixelDataType::UNSIGNED_INT_2_10_10_10_REV: return GL_UNSIGNED_INT_2_10_10_10_REV;
		default: return GL_UNSIGNED_BYTE;
		}
	}

	inline GLenum ToOpenGLBufferUsage(NLS::Render::Settings::EAccessSpecifier usage)
	{
		using NLS::Render::Settings::EAccessSpecifier;

		switch (usage)
		{
		case EAccessSpecifier::STREAM_DRAW: return GL_STREAM_DRAW;
		case EAccessSpecifier::STREAM_READ: return GL_STREAM_READ;
		case EAccessSpecifier::STREAM_COPY: return GL_STREAM_COPY;
		case EAccessSpecifier::DYNAMIC_DRAW: return GL_DYNAMIC_DRAW;
		case EAccessSpecifier::DYNAMIC_READ: return GL_DYNAMIC_READ;
		case EAccessSpecifier::DYNAMIC_COPY: return GL_DYNAMIC_COPY;
		case EAccessSpecifier::STATIC_DRAW: return GL_STATIC_DRAW;
		case EAccessSpecifier::STATIC_READ: return GL_STATIC_READ;
		case EAccessSpecifier::STATIC_COPY: return GL_STATIC_COPY;
		default: return GL_DYNAMIC_DRAW;
		}
	}

	inline OpenGLTextureFormatInfo ToOpenGLTextureFormatInfo(NLS::Render::RHI::TextureFormat format)
	{
		using NLS::Render::RHI::TextureFormat;

		switch (format)
		{
		case TextureFormat::RGB8: return { GL_RGB8, GL_RGB, GL_UNSIGNED_BYTE };
		case TextureFormat::RGBA8: return { GL_RGBA8, GL_RGBA, GL_UNSIGNED_BYTE };
		case TextureFormat::RGBA16F: return { GL_RGBA16F, GL_RGBA, GL_FLOAT };
		case TextureFormat::Depth24Stencil8: return { GL_DEPTH24_STENCIL8, GL_DEPTH_STENCIL, GL_UNSIGNED_INT_24_8 };
		default: return { GL_RGBA8, GL_RGBA, GL_UNSIGNED_BYTE };
		}
	}

	inline NLS::Render::Resources::UniformType ToUniformType(GLenum type)
	{
		using NLS::Render::Resources::UniformType;

		switch (type)
		{
		case GL_BOOL: return UniformType::UNIFORM_BOOL;
		case GL_INT: return UniformType::UNIFORM_INT;
		case GL_FLOAT: return UniformType::UNIFORM_FLOAT;
		case GL_FLOAT_VEC2: return UniformType::UNIFORM_FLOAT_VEC2;
		case GL_FLOAT_VEC3: return UniformType::UNIFORM_FLOAT_VEC3;
		case GL_FLOAT_VEC4: return UniformType::UNIFORM_FLOAT_VEC4;
		case GL_FLOAT_MAT4: return UniformType::UNIFORM_FLOAT_MAT4;
		case GL_DOUBLE_MAT4: return UniformType::UNIFORM_DOUBLE_MAT4;
		case GL_SAMPLER_2D: return UniformType::UNIFORM_SAMPLER_2D;
		case GL_SAMPLER_CUBE: return UniformType::UNIFORM_SAMPLER_CUBE;
		default: return UniformType::UNIFORM_FLOAT;
		}
	}
}
