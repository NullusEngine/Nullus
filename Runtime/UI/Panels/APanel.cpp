#include <algorithm>
#include <chrono>

#include "UI/Panels/APanel.h"

#include "Profiling/Profiler.h"

namespace NLS::UI
{
	uint64_t APanel::__PANEL_ID_INCREMENT = 0;

	APanel::APanel()
	{
		m_panelID = "##" + std::to_string(__PANEL_ID_INCREMENT++);
	}

	void APanel::Draw()
	{
		if (!enabled)
			return;

		const auto drawStart = std::chrono::steady_clock::now();
		NLS_PROFILE_NAMED_SCOPE(GetProfilerScopeName());
		_Draw_Impl();
		const auto drawEnd = std::chrono::steady_clock::now();
		m_lastDrawDurationUs = std::max<uint64_t>(
			1u,
			static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(drawEnd - drawStart).count()));
	}

	const std::string& APanel::GetPanelID() const
	{
		return m_panelID;
	}

	uint64_t APanel::GetLastDrawDurationUs() const
	{
		return m_lastDrawDurationUs;
	}

	const std::string& APanel::GetProfilerName() const
	{
		return m_panelID;
	}

	const char* APanel::GetProfilerScopeName()
	{
		const auto& profilerName = GetProfilerName();
		if (m_cachedProfilerScopeName.empty() || m_cachedProfilerName != profilerName)
		{
			m_cachedProfilerName = profilerName;
			m_cachedProfilerScopeName = "Panel::Draw:";
			m_cachedProfilerScopeName += profilerName;
		}

		return m_cachedProfilerScopeName.c_str();
	}

}
