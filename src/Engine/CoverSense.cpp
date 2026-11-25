#include "Engine/CoverSense.h"

#include "Config.h"
#include "Core/SenseTrigger.h"
#include "Engine/Camera.h"
#include "Engine/Frame.h"
#include "Engine/Guard.h"
#include "Engine/Input.h"
#include "Engine/WorldProbe.h"

namespace Engine::CoverSense
{
	Core::ProbeTicket ProbeAdapter::Submit(const Core::ProbeRequest& a_request)
	{
		return WorldProbe::Submit({ a_request.from.x, a_request.from.y, a_request.from.z }, { a_request.to.x, a_request.to.y, a_request.to.z }, a_request.radius);
	}

	std::optional<Core::ProbeAnswer> ProbeAdapter::Poll(const Core::ProbeTicket a_ticket)
	{
		const auto r = WorldProbe::Poll(a_ticket);
		if (!r) {
			return std::nullopt;
		}
		Core::ProbeAnswer a;
		a.available = r->ok;
		a.hit = r->ok && r->hit;
		a.stop = { r->point.x, r->point.y, r->point.z };  // the end point the cast moved back to when it hit, else the target
		return a;
	}

	Core::SensorSettings Settings()
	{
		auto s = Config::Get().sensorCfg;
		s.probeRadius = Config::Get().probeRadius;
		return s;
	}

	float HeadingDeg(const Core::Vec2& a_direction)
	{
		return std::atan2(a_direction.x, a_direction.y) * 57.29577951f;
	}

	namespace
	{
		std::string EdgeText(const float a_distance, const Core::EdgeKind a_kind, const float a_range)
		{
			if (a_kind == Core::EdgeKind::None) {
				return std::format("none within {:.1f} m", a_range);
			}
			return std::format("{:.2f} m ({})", a_distance, Core::ToString(a_kind));
		}
	}

	std::string Describe(const Core::SensorResult& a_result)
	{
		const auto& c = a_result.candidate;
		const auto& cfg = Config::Get().sensorCfg;
		const float range = a_result.isTrack ? cfg.trackEdgeRange : cfg.edgeSearchRange;
		std::string out;
		if (a_result.isTrack) {
			out = std::format("TRACK {} (normal turned {:.1f} deg): ", Core::ToString(a_result.status), a_result.turnDeg);
		}
		if (c.kind == Core::CoverKind::None) {
			out += std::format("kind NONE, reason: {}", Core::ToString(c.why));
			if (c.why == Core::Reject::TooLow || c.why == Core::Reject::PathBlocked || c.why == Core::Reject::TooNarrow) {  // the height is known only from the profile on
				out += std::format(" | learned: height {:.2f} m{}", c.height, c.heightCapped ? "+" : "");
			}
			if (c.why != Core::Reject::NoSurface && c.why != Core::Reject::BadInput && c.why != Core::Reject::ProbeUnavailable && c.why != Core::Reject::TooSmallToFit) {
				out += std::format(", normal ({:+.2f},{:+.2f}) = the wall lies toward heading {:.0f} deg, distance {:.2f} m, fit rms {:.3f}", c.normal.x, c.normal.y, HeadingDeg({ -c.normal.x, -c.normal.y }), c.distance, c.fitRms);
			}
			if (c.why == Core::Reject::TooFarToTheSide) {
				out += std::format(", wall point {:.2f} m at {:.0f} deg from the view", c.wallPointDistance, c.wallPointAngleDeg);
			}
			if (c.why == Core::Reject::TooNarrow) {
				out += std::format(", edges left {} right {}", EdgeText(c.edgeLeft, c.edgeLeftKind, range), EdgeText(c.edgeRight, c.edgeRightKind, range));
			}
		} else {
			out += std::format("kind {} cover, height {:.2f} m{} ({}), normal ({:+.2f},{:+.2f}) = the wall lies toward heading {:.0f} deg, distance to the surface {:.2f} m, anchor ({:.2f},{:.2f},{:.2f}), stand point ({:.2f},{:.2f},{:.2f}), "
							   "edge left {}, edge right {} (facing the wall), floor z {:.2f}, fit rms {:.3f}, wall point {:.2f} m at {:.0f} deg from the view, foot moved {:+.2f} m along the wall",
				c.kind == Core::CoverKind::Low ? "LOW" : "HIGH", c.height, c.heightCapped ? " (at least: taller than the highest cast)" : "", c.canPeekOver ? "can peek over" : "cannot peek over", c.normal.x, c.normal.y, HeadingDeg({ -c.normal.x, -c.normal.y }), c.distance, c.anchor.x, c.anchor.y,
				c.anchor.z, c.standPoint.x, c.standPoint.y, c.standPoint.z, EdgeText(c.edgeLeft, c.edgeLeftKind, range), EdgeText(c.edgeRight, c.edgeRightKind, range), c.floorZ, c.fitRms, c.wallPointDistance, c.wallPointAngleDeg, c.clampShift);
		}
		if (c.usedFallback) {
			out += c.kind == Core::CoverKind::None ? std::format(" | fallback: the surface ahead was rejected as above, the best outer ray too ({})", Core::ToString(c.otherWhy))
			                                       : std::format(" | fallback: the surface ahead was rejected ({}), the best outer ray was taken", Core::ToString(c.otherWhy));
		}
		out += std::format(" | {} casts, {} updates", a_result.casts, a_result.updates);
		return out;
	}

	namespace
	{
		// The compact scan logged with every candidate: the nearest surface in seven directions from -90 to +90 degrees of the view, 30 degrees
		// apart, at three heights. It is independent of the sensor's own casts: the evidence the verdict is compared with.
		constexpr int                  kScanDirs = 7;
		constexpr std::array<float, 3> kScanHeights{ 0.3f, 0.9f, 1.5f };
		constexpr float                kScanRange = 6.0f;

		bool                             s_broken{ false };
		ProbeAdapter                     s_adapter;
		std::optional<Core::CoverSensor> s_sensor;
		Core::SenseTrigger               s_trigger;

		// the query that is running (or finished and waiting for its scan)
		std::uint32_t                     s_count{ 0 };  // queries started, by the key or by themselves
		std::uint32_t                     s_keyCount{ 0 };
		std::uint32_t                     s_autoCount{ 0 };
		double                            s_started{ 0.0 };
		RE::NiPoint3                      s_pos{};
		Core::Vec2                        s_intent{};
		float                             s_yaw{ 0.0f };
		const char*                       s_why{ "key" };
		WorldProbe::Batch                 s_scan;
		bool                              s_scanPending{ false };
		std::optional<Core::SensorResult> s_held;  // the sensor's result, waiting for the scan

		void StartQuery(const Frame::Snapshot& a_snap, const float a_yaw, const char* a_why, const bool a_key)
		{
			s_intent = { std::sin(a_yaw), std::cos(a_yaw) };
			s_yaw = a_yaw;
			s_pos = a_snap.position;
			s_started = a_snap.time;
			s_why = a_why;
			++s_count;
			++(a_key ? s_keyCount : s_autoCount);
			s_sensor->SetSettings(Settings());
			(void)s_sensor->Find({ a_snap.position.x, a_snap.position.y, a_snap.position.z }, s_intent);

			s_scan.Clear();
			s_scanPending = true;
			const float radius = Config::Get().probeRadius;
			for (int k = 0; k < kScanDirs; ++k) {
				const float a = a_yaw + static_cast<float>(k - 3) * 30.0f * 0.017453292f;
				for (const float h : kScanHeights) {
					const RE::NiPoint3 from{ a_snap.position.x, a_snap.position.y, a_snap.position.z + h };
					s_scan.Add(from, { from.x + std::sin(a) * kScanRange, from.y + std::cos(a) * kScanRange, from.z }, radius);
				}
			}
			s_trigger.Sensed(a_snap.time, { a_snap.position.x, a_snap.position.y }, a_yaw);
		}

		void LogQuery(const Core::SensorResult& a_result)
		{
			float viewDeg = s_yaw * 57.29577951f;
			viewDeg = std::fmod(std::fmod(viewDeg, 360.0f) + 360.0f, 360.0f);
			COVER_DETAIL("COVER candidate #{} [{}] at ({:.2f},{:.2f},{:.2f}), looking toward heading {:.0f} deg: {}", s_count, s_why, s_pos.x, s_pos.y, s_pos.z, HeadingDeg(s_intent), Describe(a_result));
			std::string cells;
			for (int k = 0; k < kScanDirs; ++k) {
				cells += std::format(" {:+d}:", (k - 3) * 30);
				for (std::size_t h = 0; h < kScanHeights.size(); ++h) {
					const auto& r = s_scan[static_cast<std::size_t>(k) * kScanHeights.size() + h];
					cells += !r.ok ? "?" : (!r.hit ? "." : std::format("{:.2f}", r.fraction * kScanRange + Config::Get().probeRadius));
					cells += h + 1 < kScanHeights.size() ? "/" : "";
				}
			}
			COVER_DETAIL("COVER scan #{} view heading {:.0f} deg, nearest surface in metres at 0.3/0.9/1.5 m height ('.' = nothing within {:.0f} m), offsets from the view in degrees:{}", s_count, viewDeg, kScanRange, cells);
		}

		void TickImpl(const Frame::Snapshot& a_snap)
		{
			const auto& cfg = Config::Get();
			if (!s_sensor) {
				s_sensor.emplace(s_adapter, Settings());
			}
			auto& sensor = *s_sensor;
			sensor.Update();
			if (sensor.HasResult()) {
				s_held = sensor.TakeResult();
			}
			if (s_held && (!s_scanPending || s_scan.Done())) {
				s_scanPending = false;
				LogQuery(*s_held);
				s_held.reset();
			} else if ((sensor.Busy() || s_held) && a_snap.time - s_started > 3.0) {
				sensor.Cancel();
				s_held.reset();
				s_scanPending = false;
				COVER_DETAIL("COVER candidate #{}: no answer within 3 s (the casts run inside the third-person camera update)", s_count);
			}
			const bool idle = !sensor.Busy() && !s_held;

			// Sense on stop: only in third person, not paused, with the cast queue running.
			Core::SenseReason reason = Core::SenseReason::None;
			float             yaw{ 0.0f };
			const bool        yawOk = Camera::ReadViewYaw(yaw);
			const bool        thirdPerson = a_snap.valid && !a_snap.paused && a_snap.cameraIndex == RE::CameraStates::kThirdPerson && WorldProbe::Available() && yawOk;
			if (cfg.senseOnStop && thirdPerson) {
				Core::SenseTriggerSettings ts;
				ts.stillSeconds = cfg.senseStillTime;
				ts.minMove = cfg.senseMinMove;
				ts.turnDeg = cfg.senseTurnDeg;
				ts.minInterval = cfg.senseMinInterval;
				s_trigger.SetSettings(ts);
				reason = s_trigger.Update(a_snap.time, { a_snap.position.x, a_snap.position.y }, yaw, !idle);
			} else {
				s_trigger.Reset();
			}

			if (!idle) {
				return;  // a key press waits for the running query to finish
			}
			const bool key = Input::ConsumeSenseRequest();
			if (key) {
				if (!a_snap.valid) {
					COVER_DETAIL("COVER sense key pressed, but there is no player in a loaded cell");
					return;
				}
				if (!WorldProbe::Available()) {
					COVER_DETAIL("COVER sense key pressed: the cast queue is not available (the casts run inside the third-person camera update: first person, a fault or bWorldProbe=0)");
					return;
				}
				if (!yawOk) {
					yaw = a_snap.angle.z;
					COVER_DETAIL("COVER sense key: the camera yaw could not be read; using the player's heading");
				}
				StartQuery(a_snap, yaw, "key", true);
			} else if (reason != Core::SenseReason::None) {
				StartQuery(a_snap, yaw, reason == Core::SenseReason::Stopped ? "stop" : "turn", false);
			}
		}
	}

	std::uint32_t KeyQueries() noexcept
	{
		return s_keyCount;
	}

	std::uint32_t AutoQueries() noexcept
	{
		return s_autoCount;
	}

	void OnTick(const Frame::Snapshot& a_snap)
	{
		const auto& cfg = Config::Get();
		if (!cfg.enabled || !cfg.sensor || !cfg.worldProbe || s_broken) {
			return;
		}
		unsigned long code{ 0 };
		if (!Guarded([&] { TickImpl(a_snap); }, code)) {
			s_broken = true;
			REX::ERROR("T2.3: exception 0x{:08X} in the cover sensor tick; the sensor is off for this session", code);
		}
	}
}
