#pragma once

#include <UI/Panels/PanelWindow.h>
#include <UI/Widgets/Texts/Text.h>

#include <UI/Profiling/TimelineProfilerSink.h>

namespace NLS::Editor::Panels
{
class ProfilerPanel final : public UI::PanelWindow
{
public:
    ProfilerPanel(
        const std::string& p_title,
        bool p_opened,
        const UI::PanelWindowSettings& p_windowSettings);

    void RefreshStatus();
    void BeginProfilerFrame();
    bool IsRecordingEnabled() const;
    NLS::Base::Profiling::TimelineProfilerSink& GetTimelineSink();

private:
    void OnBeforeDrawWidgets() override;
    void OnAfterDrawWidgets() override;

    NLS::Base::Profiling::TimelineProfilerSink m_timelineSink;
    UI::Widgets::Text& m_statusText;
    UI::Widgets::Text& m_detailText;
};
}
