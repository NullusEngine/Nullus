#pragma once

#include <cstdint>
#include <memory>

#include "Rendering/BaseSceneRenderer.h"
#include "Rendering/DeferredSceneRenderer.h"
#include "Rendering/ForwardSceneRenderer.h"
#include "EngineDef.h"

namespace NLS::Engine::Rendering
{
	enum class NLS_ENGINE_API SceneRendererKind : uint8_t
	{
		Forward,
		Deferred
	};

	inline SceneRendererKind GetDefaultSceneRendererKind()
	{
		return SceneRendererKind::Forward;
	}

	inline std::unique_ptr<BaseSceneRenderer> CreateSceneRenderer(
		NLS::Render::Context::Driver& driver,
		const SceneRendererKind kind = SceneRendererKind::Forward)
	{
		switch (kind)
		{
		case SceneRendererKind::Deferred:
			return std::make_unique<DeferredSceneRenderer>(driver);
		case SceneRendererKind::Forward:
		default:
			return std::make_unique<ForwardSceneRenderer>(driver);
		}
	}
}
