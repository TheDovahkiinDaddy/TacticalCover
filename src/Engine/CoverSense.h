#pragma once

// T2.3: CoverSensor wired to the world probe. ProbeAdapter implements Core::IProbe on top of Engine/WorldProbe (queued casts that run
// inside the third-person camera's Update hook and answer a frame later); OnTick runs the debug key: [Sensor] iSenseKey (Numpad 8)
// asks the sensor for the cover candidate in front of the camera and logs it (and, with [Sensor] bSenseOnStop, the same happens by itself when the
// player stops or the view has turned and settled: no key needed). Every candidate line is followed by a "COVER scan" line with the same number (kind, height, normal, distance, stand point, distance to each
// edge, or the reason for no cover). The t2 rig script uses the same adapter and the same log format.

#include "Core/CoverSensor.h"

namespace Engine::Frame
{
	struct Snapshot;
}

namespace Engine::CoverSense
{
	class ProbeAdapter final : public Core::IProbe
	{
	public:
		Core::ProbeTicket                Submit(const Core::ProbeRequest& a_request) override;
		std::optional<Core::ProbeAnswer> Poll(Core::ProbeTicket a_ticket) override;
	};

	// The [Sensor] settings as read from the INI.
	[[nodiscard]] Core::SensorSettings Settings();

	// One log line for a finished query (candidate or rejection, or the tracked candidate and its status).
	[[nodiscard]] std::string Describe(const Core::SensorResult& a_result);

	// Heading in degrees (0 = +y, 90 = +x) of a horizontal direction.
	[[nodiscard]] float HeadingDeg(const Core::Vec2& a_direction);

	// How many queries the sense key / the automatic triggers (stop, turn) have started so far.
	[[nodiscard]] std::uint32_t KeyQueries() noexcept;
	[[nodiscard]] std::uint32_t AutoQueries() noexcept;

	// Every tick: finishes a running query, starts one when the sense key was pressed.
	void OnTick(const Frame::Snapshot& a_snap);
}
