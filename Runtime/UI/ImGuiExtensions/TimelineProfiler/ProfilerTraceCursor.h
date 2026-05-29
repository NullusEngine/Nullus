#pragma once

#include <cmath>
#include <cstdint>
#include <limits>
#include <optional>
#include <string>

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

inline bool ShouldExportTraceDurationEvent(
	const std::uint64_t ticksBegin,
	const std::uint64_t ticksEnd)
{
	return ticksBegin != 0u && ticksEnd > ticksBegin;
}

inline unsigned long long TraceTicksToMicroseconds(
	const std::uint64_t ticks,
	const double ticksToMs,
	const bool ceilPositive)
{
	const double microseconds = 1000.0 * ticksToMs * static_cast<double>(ticks);
	if (!(microseconds > 0.0))
		return 0u;

	const double roundedMicroseconds = ceilPositive
		? std::ceil(microseconds)
		: microseconds;
	constexpr double kMaxMicroseconds =
		static_cast<double>(std::numeric_limits<unsigned long long>::max());
	if (roundedMicroseconds >= kMaxMicroseconds)
		return std::numeric_limits<unsigned long long>::max();

	return static_cast<unsigned long long>(roundedMicroseconds);
}

inline std::string EscapeTraceJsonString(const char* text)
{
	std::string escaped;
	if (text == nullptr)
		return escaped;

	for (const char* cursor = text; *cursor != '\0'; ++cursor)
	{
		const auto character = static_cast<unsigned char>(*cursor);
		switch (character)
		{
		case '\"':
			escaped += "\\\"";
			break;
		case '\\':
			escaped += "\\\\";
			break;
		case '\b':
			escaped += "\\b";
			break;
		case '\f':
			escaped += "\\f";
			break;
		case '\n':
			escaped += "\\n";
			break;
		case '\r':
			escaped += "\\r";
			break;
		case '\t':
			escaped += "\\t";
			break;
		default:
			if (character < 0x20u)
			{
				static constexpr char kHexDigits[] = "0123456789abcdef";
				escaped += "\\u00";
				escaped += kHexDigits[(character >> 4u) & 0x0fu];
				escaped += kHexDigits[character & 0x0fu];
			}
			else
			{
				escaped += static_cast<char>(character);
			}
			break;
		}
	}
	return escaped;
}

inline std::string BuildTraceMetadataEventJson(
	const char* metadataEventName,
	const std::uint32_t processId,
	const std::optional<std::uint32_t> threadIndex,
	const char* name)
{
	const std::string escapedMetadataEventName = EscapeTraceJsonString(metadataEventName);
	const std::string escapedName = EscapeTraceJsonString(name);
	std::string json;
	json.reserve(64u + escapedMetadataEventName.size() + escapedName.size());
	json += "{\"name\":\"";
	json += escapedMetadataEventName;
	json += "\",\"ph\":\"M\",\"pid\":";
	json += std::to_string(processId);
	if (threadIndex.has_value())
	{
		json += ",\"tid\":";
		json += std::to_string(*threadIndex);
	}
	json += ",\"args\":{\"name\":\"";
	json += escapedName;
	json += "\"}}";
	return json;
}

inline std::optional<std::string> BuildTraceDurationEventJson(
	const std::uint32_t threadIndex,
	const std::uint64_t ticksBegin,
	const std::uint64_t ticksEnd,
	const std::uint64_t baseTime,
	const double ticksToMs,
	const char* name)
{
	if (!ShouldExportTraceDurationEvent(ticksBegin, ticksEnd))
		return std::nullopt;

	const std::uint64_t relativeBeginTicks = ticksBegin > baseTime
		? ticksBegin - baseTime
		: 0u;
	const auto timestampUs = TraceTicksToMicroseconds(relativeBeginTicks, ticksToMs, false);
	const auto durationUs = TraceTicksToMicroseconds(ticksEnd - ticksBegin, ticksToMs, true);
	if (durationUs == 0u)
		return std::nullopt;

	const std::string escapedName = EscapeTraceJsonString(name);
	std::string json;
	json.reserve(64u + escapedName.size());
	json += "{\"pid\":0,\"tid\":";
	json += std::to_string(threadIndex);
	json += ",\"ts\":";
	json += std::to_string(timestampUs);
	json += ",\"dur\":";
	json += std::to_string(durationUs);
	json += ",\"ph\":\"X\",\"name\":\"";
	json += escapedName;
	json += "\"}";
	return json;
}
}
