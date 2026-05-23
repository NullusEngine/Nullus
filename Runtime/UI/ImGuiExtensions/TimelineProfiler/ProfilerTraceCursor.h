#pragma once

#include <cstdint>

namespace NLS::UI::TimelineProfilerDetail
{
struct TraceFrameHistoryRange
{
	std::uint32_t Begin = 0;
	std::uint32_t End = 0;
};

struct TraceFrameExportRange
{
	std::uint32_t Begin = 0;
	std::uint32_t End = 0;
};

inline TraceFrameExportRange ResolveTraceFrameExportRange(
	const TraceFrameHistoryRange frameRange,
	std::uint32_t& lastExportedFrame)
{
	if (frameRange.End <= lastExportedFrame)
		return {};

	if (lastExportedFrame + 1u < frameRange.Begin)
		lastExportedFrame = frameRange.Begin - 1u;

	if (lastExportedFrame + 1u >= frameRange.End)
		return {};

	return { lastExportedFrame + 1u, frameRange.End };
}
}
