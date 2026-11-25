#pragma once

// A fake world and a fake probe for the engine-free CoverSensor tests. The world is a set of oriented boxes (a wall, a crate, a pole,
// a floor); a sphere cast is a ray against each box grown by the sphere's radius (flat faces exact, corners square instead of
// rounded). Like the real WorldProbe the fake answers a frame later: a cast submitted now can be read only after Tick().

#include <cmath>
#include <map>
#include <optional>
#include <vector>

#include "Core/CoverSensor.h"

namespace Fake
{
	struct Box
	{
		float cx{ 0 }, cy{ 0 };      // centre (horizontal)
		float hx{ 1 }, hy{ 1 };      // half extents along the box's own x and y
		float z0{ 0 }, z1{ 1 };      // vertical range
		float angleDeg{ 0 };         // rotation of the box's x axis from the world x axis, counter-clockwise
	};

	inline Box MakeBox(const float x0, const float x1, const float y0, const float y1, const float z0, const float z1)
	{
		return { (x0 + x1) * 0.5f, (y0 + y1) * 0.5f, (x1 - x0) * 0.5f, (y1 - y0) * 0.5f, z0, z1, 0.0f };
	}

	struct World
	{
		std::vector<Box> boxes;

		void AddFloor(const float a_extent = 200.0f) { boxes.push_back(MakeBox(-a_extent, a_extent, -a_extent, a_extent, -1.0f, 0.0f)); }

		// The first hit of a sphere of a_radius moving from a_from to a_to: the fraction along the way, or -1.
		[[nodiscard]] float Cast(const Core::Vec3& a_from, const Core::Vec3& a_to, const float a_radius) const
		{
			float best = 2.0f;
			for (const auto& b : boxes) {
				const float rad = b.angleDeg * 0.017453292519943f;
				const float c = std::cos(rad), s = std::sin(rad);
				// into the box's frame
				const auto local = [&](const Core::Vec3& p) { return Core::Vec3{ (p.x - b.cx) * c + (p.y - b.cy) * s, -(p.x - b.cx) * s + (p.y - b.cy) * c, p.z }; };
				const auto  f = local(a_from);
				const auto  t = local(a_to);
				const float lo[3] = { -b.hx - a_radius, -b.hy - a_radius, b.z0 - a_radius };
				const float hi[3] = { b.hx + a_radius, b.hy + a_radius, b.z1 + a_radius };
				const float o[3] = { f.x, f.y, f.z };
				const float d[3] = { t.x - f.x, t.y - f.y, t.z - f.z };
				float       tmin = 0.0f, tmax = 1.0f;
				bool        miss = false;
				bool        inside = true;
				for (int i = 0; i < 3; ++i) {
					if (o[i] < lo[i] || o[i] > hi[i]) {
						inside = false;
					}
					if (std::fabs(d[i]) < 1e-9f) {
						if (o[i] < lo[i] || o[i] > hi[i]) {
							miss = true;
						}
						continue;
					}
					float t1 = (lo[i] - o[i]) / d[i], t2 = (hi[i] - o[i]) / d[i];
					if (t1 > t2) {
						std::swap(t1, t2);
					}
					tmin = (std::max)(tmin, t1);
					tmax = (std::min)(tmax, t2);
				}
				if (inside) {
					return 0.0f;
				}
				if (miss || tmin > tmax) {
					continue;
				}
				best = (std::min)(best, tmin);
			}
			return best <= 1.0f ? best : -1.0f;
		}
	};

	class Probe : public Core::IProbe
	{
	public:
		explicit Probe(const World& a_world) :
			m_world(&a_world) {}

		void SetWorld(const World& a_world) { m_world = &a_world; }
		void Tick() { ++m_frame; }

		bool available{ true };      // false: every answer says "not available"
		bool refuseSubmit{ false };  // true: Submit returns 0
		int  latencyFrames{ 1 };
		int  submitted{ 0 };

		Core::ProbeTicket Submit(const Core::ProbeRequest& a_request) override
		{
			if (refuseSubmit) {
				return 0;
			}
			++submitted;
			Core::ProbeAnswer a;
			a.available = available;
			a.stop = a_request.to;
			if (available) {
				const float f = m_world->Cast(a_request.from, a_request.to, a_request.radius);
				if (f >= 0.0f) {
					a.hit = true;
					a.stop = { a_request.from.x + (a_request.to.x - a_request.from.x) * f, a_request.from.y + (a_request.to.y - a_request.from.y) * f,
						a_request.from.z + (a_request.to.z - a_request.from.z) * f };
				}
			}
			const auto id = m_next++;
			m_pending[id] = { a, m_frame + latencyFrames };
			return id;
		}

		std::optional<Core::ProbeAnswer> Poll(const Core::ProbeTicket a_ticket) override
		{
			const auto it = m_pending.find(a_ticket);
			if (it == m_pending.end() || it->second.second > m_frame) {
				return std::nullopt;
			}
			const auto a = it->second.first;
			m_pending.erase(it);
			return a;
		}

	private:
		const World*                                                        m_world;
		int                                                                 m_frame{ 0 };
		Core::ProbeTicket                                                   m_next{ 1 };
		std::map<Core::ProbeTicket, std::pair<Core::ProbeAnswer, int>> m_pending;
	};
}
