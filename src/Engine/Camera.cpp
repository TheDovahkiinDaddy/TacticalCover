#include "Engine/Camera.h"
#include "Engine/CameraSettings.h"

#include "Config.h"
#include "Core/CameraSlot.h"
#include "Engine/Frame.h"
#include "Engine/Guard.h"
#include "Engine/Offsets.h"
#include "Engine/WorldProbe.h"

#include "REX/W32/KERNEL32.h"

#include <atomic>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <mutex>
#include "Engine/CoverPose.h"
#include "Engine/Input.h"

namespace Engine::Camera
{
	namespace
	{
		using Orig_t = void (*)(void*, void*);
		using Clock = std::chrono::steady_clock;

		Orig_t            s_orig{ nullptr };
		std::atomic<bool> s_installed{ false };
		std::atomic<bool> s_gaveUp{ false };
		// T4.0: compatibility mode. Another mod (Stride, DyMC.dll) owns the ThirdPersonState vtable, or slot 12 does not hold the game function: CoverSF stays out of the
		// vtable, pumps its cast queue from the call-site hook at Update + 0x431 (patched in the game code, so it runs whoever owns the slot) and keeps its own camera off.
		std::atomic<bool> s_compat{ false };
		// M4b11 (REFERENCE 23.27): slot 12 was held by an allow-listed plugin (DDC.dll): our hook sits in front of it and s_orig is its function (which calls the game's). The own camera runs as in the plain case.
		std::atomic<bool> s_chained{ false };
		std::atomic<bool> s_slotLostLogged{ false };
		double            s_nextSlotCheck{ 0.0 };
		// T5.0b: the rig hold (spike)
		std::atomic<bool>          s_rigHold{ false };
		std::atomic<float>         s_rigBoom[3]{ 0.0f, 0.0f, 0.0f };
		std::atomic<float>         s_rigPivot[3]{ 0.0f, 0.0f, 0.0f };
		std::atomic<std::uint64_t> s_rigWrites{ 0 };
		std::atomic<bool>          s_rigFaulted{ false };
		std::atomic<bool> s_faulted{ false };
		// T5.3a: the cover camera
		std::atomic<float>         s_ccBackTarget{ 0.0f }, s_ccUpTarget{ 0.0f };
		std::atomic<float>         s_ccBackNow{ 0.0f }, s_ccUpNow{ 0.0f };
		std::atomic<bool>          s_ccReset{ false };
		std::atomic<std::uint64_t> s_ccWrites{ 0 }, s_ccRestored{ 0 }, s_ccLeftAlone{ 0 };
		std::atomic<bool>          s_ccFaulted{ false };
		struct NodeMemory
		{
			bool         valid{ false };
			RE::NiPoint3 base{};   // what the graph gave (before the offset)
			RE::NiPoint3 wrote{};  // what was written
		};
		NodeMemory s_ccBoom, s_ccPivot;  // hook thread only
		double     s_ccLast{ 0.0 };

		std::atomic<float> s_target{ 0.0f };
		std::atomic<float> s_applied{ 0.0f };
		std::atomic<float> s_allowed{ 1000.0f };  // collision limit, metres; huge until the tick has computed one

		std::atomic<std::uint64_t> s_calls{ 0 };
		std::atomic<std::uint64_t> s_callsThird{ 0 };
		std::atomic<std::uint64_t> s_callsOther{ 0 };
		std::atomic<std::uint64_t> s_applyCalls{ 0 };
		// the call-site hook (mode 3)
		using Site_t = void (*)(void*, RE::NiPoint3*, RE::NiMatrix3*);
		Site_t                     s_origSite{ nullptr };
		std::atomic<bool>          s_siteInstalled{ false };
		std::atomic<std::uint64_t> s_siteCalls{ 0 };
		std::atomic<std::uint64_t> s_siteApplied{ 0 };
		bool                       s_siteMissLogged{ false };
		int                        s_siteMatrixLogs{ 0 };
		// How the offset reaches the picture: 0 = the GetTranslation getter (slot 14) only, 1 = move the camera root and its first
		// child after Update, 2 = write the offset into the state's own position (+0x190 / +0x19C) after Update and take it out again
		// before the next Update (so it cannot accumulate). Experiment of run 18; the default is [Camera] iCameraMode.
		std::atomic<int> s_mode{ -1 };  // -1: not set yet, takes the INI value
		std::atomic<double>        s_lastThirdTime{ 0.0 };

		// S4b: the pivot shift (mode 4) and the probe. Hook thread only unless noted.
		std::atomic<std::uint64_t> s_pivotWrites{ 0 };    // Updates in which the pivot node's local x was shifted
		std::atomic<std::uint64_t> s_pivotRestored{ 0 };  // ... and the old value was written back
		std::atomic<std::uint64_t> s_pivotChanged{ 0 };   // ... but the float no longer held what was written (left alone)
		std::atomic<std::uint64_t> s_pivotNull{ 0 };      // Updates in which the pivot node pointer was null or unreadable
		float                      s_lastPivotUsed{ 0.0f };  // the x the latest Update read (written value in mode 4, else the node's value)
		bool                       s_pivotUsedKnown{ false };
		bool                       s_pivotChangedLogged{ false };
		bool                       s_pivotLayoutLogged{ false };
		std::mutex                 s_probeMutex;
		std::string                s_probeLabel;  // pending request, guarded by s_probeMutex
		std::atomic<bool>          s_probePending{ false };
		ProbeResult                s_lastProbe;   // guarded by s_probeMutex
		float                      s_siteRow0[3]{ 0, 0, 0 };  // rows 0 and 1 of the matrix the latest call-site call returned
		float                      s_siteRow1[3]{ 0, 0, 0 };
		bool                       s_siteRowsValid{ false };

		// Hook thread only.
		Clock::time_point s_lastApply{};
		bool              s_haveLastApply{ false };
		bool              s_firstCallLogged{ false };
		std::uint32_t     s_hookThreads[8]{};
		std::size_t       s_hookThreadCount{ 0 };

		// Tick thread only.
		double s_nextInstall{ 0.0 };
		double s_nextAllowed{ 0.0 };
		double s_nextStats{ 0.0 };
		std::uint64_t s_statsCallsThird{ 0 };
		std::uint64_t s_statsCalls{ 0 };

		[[nodiscard]] std::uintptr_t ModuleBase()
		{
			return REX::FModule::GetExecutingModule().GetBaseAddress();
		}

		[[nodiscard]] int CurrentMode() noexcept
		{
			const int m = s_mode.load();
			return m < 0 ? Config::Get().cameraMode : m;
		}

		// ---- S4b probe ---------------------------------------------------------------------------------------
		// Runs in the Update hook after the original (and after the pivot write-back), once per request. It only reads.
		[[nodiscard]] RE::NiAVObject* ReadNode(void* a_state, const std::ptrdiff_t a_off)
		{
			std::uintptr_t p{ 0 };
			if (!SafeRead(reinterpret_cast<const char*>(a_state) + a_off, p) || p == 0) {
				return nullptr;
			}
			return reinterpret_cast<RE::NiAVObject*>(p);
		}

		[[nodiscard]] std::string DescribeNode(const char* a_what, RE::NiAVObject* a_n)
		{
			if (!a_n) {
				return std::format(" {} null;", a_what);
			}
			const char* nm = a_n->name.c_str();
			const auto& t = a_n->local.translate;
			const auto& r = a_n->local.rotate;
			return std::format(" {} {} '{}' local t=({:+.4f},{:+.4f},{:+.4f}) rot rows=({:+.3f},{:+.3f},{:+.3f}) ({:+.3f},{:+.3f},{:+.3f}) ({:+.3f},{:+.3f},{:+.3f});", a_what, static_cast<const void*>(a_n), nm ? nm : "(no name)", t.x, t.y, t.z,
				r[0].x, r[0].y, r[0].z, r[1].x, r[1].y, r[1].z, r[2].x, r[2].y, r[2].z);
		}

		void DoProbe(void* a_this, const std::string& a_label)
		{
			ProbeResult r;
			r.label = a_label;
			const auto camera = RE::PlayerCamera::GetSingleton();
			const auto player = RE::PlayerCharacter::GetSingleton();
			if (!camera || !camera->cameraRoot || !player) {
				COVER_DETAIL("RESULT S4b probe [{}]: camera root or player not available", a_label);
				return;
			}
			const auto* root = camera->cameraRoot.get();
			const auto  boom = ReadNode(a_this, kTpsBoomNode);
			const auto  pivot = ReadNode(a_this, kTpsPivotNode);
			const auto  fov = ReadNode(a_this, kTpsFovNode);
			const auto  bone = ReadNode(a_this, kTpsBoneNode);
			float       yaw{ 0.0f }, blend{ 0.0f }, constC{ 0.0f };
			const bool  haveYaw = SafeRead(reinterpret_cast<const char*>(a_this) + kTpsYaw, yaw);
			const bool  haveBlend = SafeRead(reinterpret_cast<const char*>(a_this) + kTpsBlend, blend);
			bool        haveC = false;
			if (kPivotConstId.address() - ModuleBase() == kPivotConstRva) {
				haveC = SafeRead(reinterpret_cast<const void*>(kPivotConstId.address()), constC);
			}
			RE::NiPoint3 p190{}, p19c{};
			(void)SafeRead(reinterpret_cast<const char*>(a_this) + kThirdPersonStatePosA, p190);
			(void)SafeRead(reinterpret_cast<const char*>(a_this) + kThirdPersonStatePosB, p19c);
			float factor{ 0.0f }, timer{ 0.0f };
			const bool haveFactor = SafeRead(reinterpret_cast<const char*>(a_this) + kTpsCollisionFactor, factor) && SafeRead(reinterpret_cast<const char*>(a_this) + kTpsCollisionTimer, timer);
			r.factor = factor;
			r.timer = timer;
			r.wantedToFinal = std::sqrt((p19c.x - p190.x) * (p19c.x - p190.x) + (p19c.y - p190.y) * (p19c.y - p190.y) + (p19c.z - p190.z) * (p19c.z - p190.z));
			COVER_DETAIL("S4b probe nodes [{}]: state {}{}{}{}{} | yaw {:+.5f} (read {}) blend {:.4f} (read {}) C {:.4f} (read {}) | state+0x190 ({:+.3f},{:+.3f},{:+.3f}) +0x19C ({:+.3f},{:+.3f},{:+.3f})", a_label, a_this, DescribeNode("boom+1F8", boom),
				DescribeNode("pivot+200", pivot), DescribeNode("fov+208", fov), DescribeNode("bone+210", bone), yaw, haveYaw, blend, haveBlend, constC, haveC, p190.x, p190.y, p190.z, p19c.x, p19c.y, p19c.z);
			if (!pivot || !haveYaw || !haveBlend || !haveC) {
				COVER_DETAIL("RESULT S4b probe [{}]: pivot node {} yaw {} blend {} C {}: cannot derive P", a_label, static_cast<const void*>(pivot), haveYaw, haveBlend, haveC);
				return;
			}
			if (!s_pivotLayoutLogged) {
				s_pivotLayoutLogged = true;
				COVER_DETAIL("S4b probe: local translate of the pivot node is at node+0x{:X}", static_cast<std::uint32_t>(reinterpret_cast<const char*>(&pivot->local.translate) - reinterpret_cast<const char*>(pivot)));
			}
			const float usedX = s_pivotUsedKnown ? s_lastPivotUsed : pivot->local.translate.x;
			const float py = pivot->local.translate.y;
			const float pz = pivot->local.translate.z;

			const auto  pp = player->GetPosition();
			const float sn = std::sin(yaw), cs = std::cos(yaw);
			// Convention A (rows of Rz(yaw) are right, forward): forward (sin, cos), right (cos, -sin). B is the mirror image.
			struct Conv
			{
				float fx, fy, rx, ry, fdot, rdot;
			};
			Conv  conv[2] = { { sn, cs, cs, -sn, 0, 0 }, { -sn, cs, cs, sn, 0, 0 } };
			float bestScore = -10.0f;
			int   best = 0;
			for (int i = 0; i < 2; ++i) {
				auto& c = conv[i];
				if (s_siteRowsValid) {
					const float l1 = std::hypot(s_siteRow1[0], s_siteRow1[1]);
					const float l0 = std::hypot(s_siteRow0[0], s_siteRow0[1]);
					c.fdot = l1 > 0.01f ? (s_siteRow1[0] * c.fx + s_siteRow1[1] * c.fy) / l1 : 0.0f;
					c.rdot = l0 > 0.01f ? (s_siteRow0[0] * c.rx + s_siteRow0[1] * c.ry) / l0 : 0.0f;
				}
				if (c.fdot + c.rdot > bestScore) {
					bestScore = c.fdot + c.rdot;
					best = i;
				}
			}
			const auto& cv = conv[best];
			r.convention = best == 0 ? 'A' : 'B';
			r.convOk = s_siteRowsValid && cv.fdot > 0.9f && cv.rdot > 0.9f;
			// the pose function's matrix at the call site: rows 1 and 0 against the yaw forward and right (only logged, never assumed)
			COVER_DETAIL("S4b probe yaw check [{}]: yaw {:+.4f} rad; call-site matrix row1 ({:+.3f},{:+.3f},{:+.3f}) row0 ({:+.3f},{:+.3f},{:+.3f}) (valid {}); conv A forward ({:+.3f},{:+.3f}) dot row1 {:+.3f}, right ({:+.3f},{:+.3f}) dot row0 {:+.3f}; conv B forward ({:+.3f},{:+.3f}) dot row1 {:+.3f}, right ({:+.3f},{:+.3f}) dot row0 {:+.3f} -> using {} ({})",
				a_label, yaw, s_siteRow1[0], s_siteRow1[1], s_siteRow1[2], s_siteRow0[0], s_siteRow0[1], s_siteRow0[2], s_siteRowsValid, conv[0].fx, conv[0].fy, conv[0].fdot, conv[0].rx, conv[0].ry, conv[0].rdot, conv[1].fx, conv[1].fy,
				conv[1].fdot, conv[1].rx, conv[1].ry, conv[1].rdot, r.convention, r.convOk ? "both dots above 0.9" : "NOT confirmed");

			const float        omb = 1.0f - blend;
			const RE::NiPoint3 P{ pp.x + cv.rx * omb * usedX + cv.fx * omb * py, pp.y + cv.ry * omb * usedX + cv.fy * omb * py, pp.z + pz - blend * constC };
			const auto         cam = root->local.translate;
			const float        dx = cam.x - pp.x, dy = cam.y - pp.y, dz = cam.z - pp.z;
			r.side = dx * cv.rx + dy * cv.ry;
			r.back = -(dx * cv.fx + dy * cv.fy);
			r.up = dz;
			r.pSide = (P.x - pp.x) * cv.rx + (P.y - pp.y) * cv.ry;
			r.pForward = (P.x - pp.x) * cv.fx + (P.y - pp.y) * cv.fy;
			r.applied = s_applied.load();
			r.blend = blend;
			r.pivotX = usedX;
			const float hdist = std::hypot(P.x - cam.x, P.y - cam.y);
			r.horizDistToP = hdist;
			const auto& rot = root->local.rotate;
			float       minDist = 1e9f;
			std::string dists;
			for (int i = 0; i < 3; ++i) {
				float       ax = rot[static_cast<std::size_t>(i)].x, ay = rot[static_cast<std::size_t>(i)].y, az = rot[static_cast<std::size_t>(i)].z;
				const float al = std::sqrt(ax * ax + ay * ay + az * az);
				if (al < 0.5f) {
					dists += std::format(" row{} unusable (len {:.3f});", i, al);
					continue;
				}
				ax /= al;
				ay /= al;
				az /= al;
				const float vx = P.x - cam.x, vy = P.y - cam.y, vz = P.z - cam.z;
				const float along = vx * ax + vy * ay + vz * az;
				const float ex = vx - along * ax, ey = vy - along * ay, ez = vz - along * az;
				const float d = std::sqrt(ex * ex + ey * ey + ez * ez);
				r.distLine[i] = d;
				if (d < minDist) {
					minDist = d;
					r.bestRow = i;
				}
				dists += std::format(" row{} dist {:.4f} (P ahead by {:+.3f});", i, d, along);
			}
			// angle between row 1's horizontal direction and the yaw forward (positive = counter-clockwise seen from above)
			{
				const float ax = rot[1].x, ay = rot[1].y;
				const float cross = cv.fx * ay - cv.fy * ax;
				const float dot = cv.fx * ax + cv.fy * ay;
				r.angleDeg = std::atan2(cross, dot) * 57.29578f;
			}
			r.predictedDeg = hdist > 0.01f ? std::atan(r.applied / hdist) * 57.29578f : 0.0f;
			r.ok = true;
			COVER_DETAIL("RESULT S4b probe [{}]: mode {} applied {:+.3f} | camera in yaw frame: side {:+.3f} back {:.3f} up {:+.3f} | P: side {:+.3f} forward {:+.3f} up {:+.3f}, horizontal distance camera to P {:.3f} | angle(final row1, yaw forward) {:+.2f} deg (atan(applied/dist) {:+.2f}) | distance of P from the view line:{} best row {} | pivot x used {:+.4f}, blend {:.3f}, yaw convention {} ({}) | collision factor {:.4f} timer {:.4f} (read {}) | wanted-to-final distance {:.3f}",
				a_label, CurrentMode(), r.applied, r.side, r.back, r.up, r.pSide, r.pForward, P.z - pp.z, hdist, r.angleDeg, r.predictedDeg, dists, r.bestRow, usedX, blend, r.convention,
				r.convOk ? "confirmed by the call-site matrix" : "NOT confirmed", factor, timer, haveFactor, r.wantedToFinal);
			std::scoped_lock lk(s_probeMutex);
			s_lastProbe = r;
		}

		// ---- S4b pivot shift (mode 4) ------------------------------------------------------------------------
		struct PivotWrite
		{
			RE::NiAVObject* node{ nullptr };
			float           old{ 0.0f };
			float           wrote{ 0.0f };
		};

		// Before the original Update: add the eased offset to the pivot node's local x.
		void PivotBefore(void* a_this, PivotWrite& a_w)
		{
			s_pivotUsedKnown = false;
			const auto camera = RE::PlayerCamera::GetSingleton();
			if (!camera || !a_this || a_this != camera->cameraStates[RE::CameraStates::kThirdPerson]) {
				return;
			}
			const auto pivot = ReadNode(a_this, kTpsPivotNode);
			if (!pivot) {
				s_pivotNull.fetch_add(1);
				return;
			}
			s_lastPivotUsed = pivot->local.translate.x;
			s_pivotUsedKnown = true;
			if (CurrentMode() != 4) {
				return;
			}
			const float applied = s_applied.load();
			if (std::fabs(applied) < 0.0005f) {
				return;
			}
			a_w.node = pivot;
			a_w.old = pivot->local.translate.x;
			a_w.wrote = a_w.old + applied;
			pivot->local.translate.x = a_w.wrote;
			s_lastPivotUsed = a_w.wrote;
			s_pivotWrites.fetch_add(1);
		}

		// After the original Update: put the old value back, only if the float still holds exactly what was written.
		void PivotAfter(const PivotWrite& a_w)
		{
			if (!a_w.node) {
				return;
			}
			const float now = a_w.node->local.translate.x;
			if (std::bit_cast<std::uint32_t>(now) == std::bit_cast<std::uint32_t>(a_w.wrote)) {
				a_w.node->local.translate.x = a_w.old;
				s_pivotRestored.fetch_add(1);
			} else {
				s_pivotChanged.fetch_add(1);
				if (!s_pivotChangedLogged) {
					s_pivotChangedLogged = true;
					REX::WARN("S4b: the pivot node's local x no longer held the value written before Update (wrote {:+.5f}, read {:+.5f}, old {:+.5f}); left alone", a_w.wrote, now, a_w.old);
				}
			}
		}

		// ---- the hook ----------------------------------------------------------------------------------
		// Runs after the original Update, once per frame in third person: counts the calls and advances the smoothed offset. The
		// offset itself reaches the picture in the call-site hook below (the engine computes the camera position there).
		void AfterUpdate(void* a_this)
		{
			const auto  camera = RE::PlayerCamera::GetSingleton();
			const auto& cfg = Config::Get();
			s_calls.fetch_add(1);

			const auto tid = REX::W32::GetCurrentThreadId();
			if (s_hookThreadCount < std::size(s_hookThreads) && std::find(std::begin(s_hookThreads), std::begin(s_hookThreads) + s_hookThreadCount, tid) == std::begin(s_hookThreads) + s_hookThreadCount) {
				s_hookThreads[s_hookThreadCount++] = tid;
			}

			if (!camera) {
				return;
			}
			const bool isThird = a_this && a_this == camera->cameraStates[RE::CameraStates::kThirdPerson];
			if (!s_firstCallLogged) {
				s_firstCallLogged = true;
				COVER_DETAIL("S4 hook: first call this={} PlayerCamera::cameraStates[third]={} (equal={}) currentState={} thread={}", a_this,
					camera->cameraStates[RE::CameraStates::kThirdPerson], isThird, camera->currentState, tid);
			}
			if (!isThird) {
				if (s_callsOther.fetch_add(1) < 3) {
					COVER_DETAIL("S4 hook: call on an object that is not cameraStates[third]: this={} currentState={}", a_this, camera->currentState);
				}
				return;
			}
			s_callsThird.fetch_add(1);
			s_lastThirdTime.store(Frame::Now());

			const auto now = Clock::now();
			const float dt = s_haveLastApply ? (std::min)(std::chrono::duration<float>(now - s_lastApply).count(), 0.25f) : 0.0f;
			s_lastApply = now;
			s_haveLastApply = true;

			// Smooth towards the target, limited by the collision allowance. Easing is exponential in time.
			const float target = s_target.load();
			const float limit = CurrentMode() == 4 ? 1000.0f : s_allowed.load();  // mode 4: the engine's own collision reads the shifted node
			const float want = target < 0.0f ? -(std::min)(-target, limit) : (std::min)(target, limit);
			float       current = s_applied.load();
			const float k = cfg.cameraSmoothTime <= 0.001f ? 1.0f : 1.0f - std::exp(-dt / cfg.cameraSmoothTime);
			current += (want - current) * k;
			if (std::fabs(current - want) < 0.0005f) {
				current = want;
			}
			s_applied.store(current);

			if (s_probePending.exchange(false)) {
				std::string label;
				{
					std::scoped_lock lk(s_probeMutex);
					label = s_probeLabel;
				}
				DoProbe(a_this, label);
			}
		}

		// The call-site hook: void(ThirdPersonState*, NiPoint3* position, NiMatrix3* rotation). The original fills position and
		// rotation; the offset is added to the position afterwards. Which row or column of the matrix is the camera's right
		// vector is not assumed: the sideways direction comes from the camera-to-player geometry, and the matrix candidates are
		// logged against it so that the numbers can settle it.
		void SiteAfter(void* a_this, RE::NiPoint3* a_pos, RE::NiMatrix3* a_rot)
		{
			const auto camera = RE::PlayerCamera::GetSingleton();
			if (!camera || !a_pos || a_this != camera->cameraStates[RE::CameraStates::kThirdPerson]) {
				return;
			}
			s_siteCalls.fetch_add(1);
			if (a_rot) {
				s_siteRow0[0] = (*a_rot)[0].x;
				s_siteRow0[1] = (*a_rot)[0].y;
				s_siteRow0[2] = (*a_rot)[0].z;
				s_siteRow1[0] = (*a_rot)[1].x;
				s_siteRow1[1] = (*a_rot)[1].y;
				s_siteRow1[2] = (*a_rot)[1].z;
				s_siteRowsValid = true;
			}
			const int mode = s_mode.load() < 0 ? Config::Get().cameraMode : s_mode.load();
			if (mode != 3) {
				return;
			}
			const float current = s_applied.load();
			if (std::fabs(current) < 0.0005f) {
				return;
			}
			// *a_pos is RELATIVE to the player (run 23: it equals camera root minus player), so the camera-to-player direction is -pos.
			float       fx = -a_pos->x;
			float       fy = -a_pos->y;
			const float len = std::sqrt(fx * fx + fy * fy);
			if (len < 0.05f) {
				return;
			}
			fx /= len;
			fy /= len;
			float rx = fy;  // geometric right = forward x up
			float ry = -fx;
			if (a_rot && s_siteMatrixLogs < 3) {
				++s_siteMatrixLogs;
				// rows and columns of the returned rotation against the geometric right (fy,-fx) and forward (fx,fy)
				std::string cand;
				for (int i = 0; i < 3; ++i) {
					const auto& r = (*a_rot)[static_cast<std::size_t>(i)];
					cand += std::format(" row{}=({:+.2f},{:+.2f},{:+.2f}) right.dot={:+.2f} fwd.dot={:+.2f};", i, r.x, r.y, r.z, r.x * fy - r.y * fx, r.x * fx + r.y * fy);
				}
				for (int j = 0; j < 3; ++j) {
					const float cx = (*a_rot)[0][static_cast<std::size_t>(j)];
					const float cy = (*a_rot)[1][static_cast<std::size_t>(j)];
					const float cz = (*a_rot)[2][static_cast<std::size_t>(j)];
					cand += std::format(" col{}=({:+.2f},{:+.2f},{:+.2f}) right.dot={:+.2f} fwd.dot={:+.2f};", j, cx, cy, cz, cx * fy - cy * fx, cx * fx + cy * fy);
				}
				COVER_DETAIL("S4 call site: position relative to the player ({:.3f},{:.3f},{:.3f}), camera-to-player forward ({:.3f},{:.3f}); rotation candidates:{}", a_pos->x, a_pos->y, a_pos->z, fx, fy, cand);
			}
			// Run 23: row 0 of the returned rotation is the camera's right vector (right.dot +0.95) and row 1 its forward vector
			// (fwd.dot +0.95). [Camera] iCameraRightSource: 0 = row 0 (default), 1 = the geometry above.
			if (a_rot && Config::Get().cameraRightSource == 0) {
				const float mx = (*a_rot)[0].x;
				const float my = (*a_rot)[0].y;
				const float ml = std::sqrt(mx * mx + my * my);
				if (ml > 0.5f) {
					rx = mx / ml;
					ry = my / ml;
				}
			}
			a_pos->x += rx * current;
			a_pos->y += ry * current;
			s_siteApplied.fetch_add(1);
		}

		[[nodiscard]] bool IsLiveThirdPerson(void* a_this)
		{
			const auto camera = RE::PlayerCamera::GetSingleton();
			return camera && a_this && a_this == camera->cameraStates[RE::CameraStates::kThirdPerson];
		}

		// T5.0b: writes the stored points into the local translates of the boom and the pivot node, before the original call of the site; only for the live third-person state
		std::atomic<float>  s_graphBoomY{ 0.0f };
		std::atomic<double> s_graphBoomAt{ -1.0 };
		std::atomic<bool>   s_relOn{ false };
		std::atomic<double> s_relStart{ 0.0 };
		std::atomic<float>  s_relDur{ 0.25f };

		std::atomic<bool>   s_enOn{ false };
		std::atomic<double> s_enStart{ 0.0 };
		std::atomic<float>  s_enDur{ 1.5f };
		bool                s_enHave{ false };
		float               s_enX{ 0.0f }, s_enY{ 0.0f }, s_enZ{ 0.0f };  // the begin position in the yaw frame (right, forward, up)

		void EntryBlendAfter(void* a_this, RE::NiPoint3* a_pos)
		{
			if (!s_enOn.load() || !a_pos || !IsLiveThirdPerson(a_this)) {
				return;
			}
			float yaw{ 0.0f };
			if (!SafeRead(reinterpret_cast<const char*>(a_this) + kTpsYaw, yaw)) {
				return;
			}
			const float c = std::cos(yaw), s = std::sin(yaw);
			const double now = Frame::Now();
			if (!s_enHave) {
				s_enX = a_pos->x * c - a_pos->y * s;
				s_enY = a_pos->x * s + a_pos->y * c;
				s_enZ = a_pos->z;
				s_enHave = true;
				return;
			}
			const double u = (now - s_enStart.load()) / static_cast<double>((std::max)(s_enDur.load(), 0.05f));
			if (u >= 1.0) {
				s_enOn = false;
				return;
			}
			const float e = static_cast<float>(u * u * (3.0 - 2.0 * u));
			const float bx = s_enX * c + s_enY * s, by = -s_enX * s + s_enY * c;
			a_pos->x = bx + (a_pos->x - bx) * e;
			a_pos->y = by + (a_pos->y - by) * e;
			a_pos->z = s_enZ + (a_pos->z - s_enZ) * e;
		}

		// the node's world translate from its parent's world and its own local one (what the engine's update does), so that a local write is seen by what reads the world
		void FixWorld(RE::NiAVObject* a_node)
		{
			if (a_node && a_node->parent) {
				a_node->world.translate = (a_node->parent->world * a_node->local).translate;
			}
		}

		void HoldWorldFix(void* a_this)
		{
			if (!Config::Get().poseHoldWorld || !s_rigHold.load() || s_rigFaulted.load() || !IsLiveThirdPerson(a_this)) {
				return;
			}
			unsigned long code{ 0 };
			(void)Guarded(
				[&] {
					FixWorld(ReadNode(a_this, kTpsPivotNode));
					FixWorld(ReadNode(a_this, kTpsBoomNode));
				},
				code);
		}

		void ApplyRigHold(void* a_this)
		{
			if (!s_rigHold.load() || s_rigFaulted.load() || !(Config::Get().rigHold || Config::Get().coverPoses) || !IsLiveThirdPerson(a_this)) {
				return;
			}
			unsigned long code{ 0 };
			const bool    ok = Guarded(
                [&] {
                    const auto boom = ReadNode(a_this, kTpsBoomNode);
                    const auto pivot = ReadNode(a_this, kTpsPivotNode);
                    if (s_relOn.load() && boom && pivot) {
                        // the hand-over: from the held values to what the graph wrote this frame
                        const double u = (Frame::Now() - s_relStart.load()) / static_cast<double>((std::max)(s_relDur.load(), 0.01f));
                        if (u >= 1.0) {
                            s_relOn = false;
                            s_rigHold = false;
                            return;
                        }
                        const float e = static_cast<float>(u * u * (3.0 - 2.0 * u));
                        const auto  g1 = boom->local.translate;
                        const auto  g2 = pivot->local.translate;
                        s_graphBoomY.store(g1.y);
                        s_graphBoomAt.store(Frame::Now());
                        boom->local.translate = { s_rigBoom[0].load() + (g1.x - s_rigBoom[0].load()) * e, s_rigBoom[1].load() + (g1.y - s_rigBoom[1].load()) * e, s_rigBoom[2].load() + (g1.z - s_rigBoom[2].load()) * e };
                        pivot->local.translate = { s_rigPivot[0].load() + (g2.x - s_rigPivot[0].load()) * e, s_rigPivot[1].load() + (g2.y - s_rigPivot[1].load()) * e, s_rigPivot[2].load() + (g2.z - s_rigPivot[2].load()) * e };
                        s_rigWrites.fetch_add(1);
                        return;
                    }
                    if (boom) {
                        s_graphBoomY.store(boom->local.translate.y);
                        s_graphBoomAt.store(Frame::Now());
                        boom->local.translate = { s_rigBoom[0].load(), s_rigBoom[1].load(), s_rigBoom[2].load() };
                    }
                    if (pivot) {
                        pivot->local.translate = { s_rigPivot[0].load(), s_rigPivot[1].load(), s_rigPivot[2].load() };
                    }
                    s_rigWrites.fetch_add(1);
                },
                code);
			if (!ok) {
				s_rigFaulted = true;
				REX::ERROR("T5.0b: exception 0x{:08X} writing the camera rig nodes; the rig hold is off for this session", code);
			}
		}

		[[nodiscard]] bool SamePoint(const RE::NiPoint3& a, const RE::NiPoint3& b) { return a.x == b.x && a.y == b.y && a.z == b.z; }

		// ---- M4a: the own camera's framing (REFERENCE 23) ----------------------------------------------------------------------------------------------------------------------
		// Hook thread only, except the atomics. One eased value each for the distance behind, the shoulder (its size), the height; the side (+1 right, -1 left) eases on its own and is what the swap key changes.
		struct Ease
		{
			bool   init{ false };
			float  cur{ 0.0f }, from{ 0.0f }, target{ 0.0f };
			double t0{ 0.0 };
			float  dur{ 0.25f };

			float Step(const float a_target, const double a_now, const float a_dur)
			{
				if (!init) {
					init = true;
					cur = from = target = a_target;
					t0 = a_now;
					return cur;
				}
				if (a_target != target) {
					from = cur;
					target = a_target;
					t0 = a_now;
					dur = a_dur;
				}
				if (dur <= 0.001f) {
					cur = target;
				} else {
					const float u = std::clamp(static_cast<float>((a_now - t0) / static_cast<double>(dur)), 0.0f, 1.0f);
					cur = from + (target - from) * (u * u * (3.0f - 2.0f * u));
				}
				return cur;
			}
		};
		Ease              s_frDist, s_frShoulder, s_frHeight, s_frSide;
		std::atomic<int>  s_frOverride{ 0 };  // the shoulder the corner hold wants (0 = none)
		std::atomic<int>  s_frSideDir{ 1 };   // the wanted shoulder: +1 right, -1 left
		std::atomic<int>  s_frCoverKind{ 0 };
		std::atomic<bool> s_frCoverAim{ false };
		std::atomic<int>  s_frSituation{ 0 };
		std::atomic<bool> s_frFaulted{ false };
		std::atomic<std::uint64_t> s_frWrites{ 0 };
		FramingState      s_frState;  // hook thread writes, GetFraming copies under s_frMutex
		std::mutex        s_frMutex;
		int               s_frLastLogged{ -1 };
		std::atomic<int> s_frForce{ -1 };

		[[nodiscard]] int PickSituation()
		{
			if (const int f = s_frForce.load(); f >= 0) {
				return f;
			}
			const int  cover = s_frCoverKind.load();
			const bool aiming = s_frCoverAim.load() || CoverPose::VarValue("iIsSighted") > 0.5 || CoverPose::VarValue("iSyncSighted") > 0.5;
			if (cover != 0) {
				return (cover == 1 ? 4 : 5) + (aiming ? 2 : 0);
			}
			if (aiming) {
				return 2;
			}
			if (CoverPose::VarValue("iSyncSprintState") > 0.5) {
				return 3;
			}
			if (const auto player = RE::PlayerCharacter::GetSingleton()) {
				std::uint32_t f1{ 0 };
				if (SafeRead(&player->actorState1, f1) && (f1 & 0x100u) != 0) {  // the sprint bit of the actor's movement flags (REFERENCE 7: 0xB000019A while sprinting)
					return 3;
				}
			}
			if (CoverPose::WeaponState() == 3) {
				return 1;
			}
			return 0;
		}

		struct FrameVals
		{
			bool  on{ false };
			float dist{ 0.0f }, x{ 0.0f }, height{ 0.0f };
		};

		// the framing of this frame: the eased distance behind, the pivot's x (the shoulder, signed) and the height offset. Nothing is written here (ApplyCoverCam writes, with the cover camera's offsets,
		// into one memory of what the graph gave and what was written, so that nothing accumulates and the engine reads the same values before and after the pose call: the camera looks at the pivot)
		FrameVals ComputeFraming(void* a_this)
		{
			FrameVals    v;
			const auto&  cfg = Config::Get();
			if (!cfg.framing || s_frFaulted.load() || s_compat.load() || !IsLiveThirdPerson(a_this)) {
				return v;
			}
			const double now = Frame::Now();
			const int    sit = PickSituation();
			s_frSituation.store(sit);
			float dist = cfg.frameDistExplore, shoulder = cfg.frameShoulderExplore, height = 0.0f;
			switch (sit) {
			case 1:
			case 4:
			case 5:
				dist = cfg.frameDistDrawn;
				shoulder = cfg.frameShoulderDrawn;
				height = cfg.frameHeightDrawn;
				break;
			case 2:
			case 6:
			case 7:
				dist = cfg.frameDistAim;
				shoulder = cfg.frameShoulderAim;
				height = cfg.frameHeightAim;
				break;
			case 3:
				dist = cfg.frameDistSprint;
				shoulder = cfg.frameShoulderSprint;
				break;
			default:
				break;
			}
			if (sit != s_frLastLogged) {
				if (cfg.frameLog) {
					COVER_DETAIL("M4A FRAME situation {} -> {} ({}): distance behind {:.2f}, shoulder {:.2f}, height {:+.2f} (eased over {:.2f} s), side {}", s_frLastLogged < 0 ? "-" : SituationName(s_frLastLogged), SituationName(sit), sit, dist, shoulder, height, cfg.frameEase, s_frSideDir.load() > 0 ? "right" : "left");
				}
				s_frLastLogged = sit;
			}
			const float d = s_frDist.Step(dist, now, cfg.frameEase);
			const float sh = s_frShoulder.Step(shoulder, now, cfg.frameEase);
			const float h = s_frHeight.Step(height, now, cfg.frameEase);
			const int   ovr = s_frOverride.load();
			const int   wantDir = ovr != 0 ? ovr : s_frSideDir.load();
			const float sd = s_frSide.Step(static_cast<float>(wantDir), now, cfg.shoulderSwapEase);
			// the swap changes the side by a full 2.0 within fShoulderSwapEase; the left shoulder is the right plus fShoulderLeftExtra
			v.on = true;
			v.dist = d;
			v.x = sd >= 0.0f ? sd * sh : sd * (sh + cfg.shoulderLeftExtra);
			v.height = h;
			std::lock_guard lock(s_frMutex);
			s_frState = FramingState{ .on = true, .situation = sit, .dist = d, .shoulder = sh, .height = h, .side = sd, .sideTarget = wantDir, .pivotX = v.x, .boomY = -d - s_ccBackNow.load(), .writes = s_frWrites.load(), .faulted = false };
			return v;
		}

		// one node of the cover camera: a_delta is added to the point the graph wrote (axis a_axis: 1 = y, 2 = z); a value that still holds what was written last time was not rewritten, so
		// the offset is taken out of it first
		struct NodeEdit
		{
			std::optional<float> absX, absY;  // the framing: replaces the graph's x or y
			float                addY{ 0.0f }, addZ{ 0.0f };
		};

		void CoverCamNode(RE::NiAVObject* a_node, NodeMemory& a_mem, const NodeEdit& a_edit, const bool a_zero)
		{
			if (!a_node) {
				a_mem.valid = false;
				return;
			}
			const RE::NiPoint3 cur = a_node->local.translate;
			RE::NiPoint3       base = cur;
			const bool         mine = a_mem.valid && SamePoint(cur, a_mem.wrote);
			if (mine) {
				base = a_mem.base;  // not rewritten by the graph: this is our own value from the last call
			}
			if (a_zero) {
				if (mine) {
					a_node->local.translate = base;
					s_ccRestored.fetch_add(1);
				} else if (a_mem.valid) {
					s_ccLeftAlone.fetch_add(1);  // the graph wrote something else meanwhile: leave it
				}
				a_mem.valid = false;
				return;
			}
			RE::NiPoint3 nt = base;
			if (a_edit.absX) {
				nt.x = *a_edit.absX;
			}
			if (a_edit.absY) {
				nt.y = *a_edit.absY;
			}
			nt.y += a_edit.addY;
			nt.z += a_edit.addZ;
			a_node->local.translate = nt;
			a_mem.base = base;
			a_mem.wrote = nt;
			a_mem.valid = true;
		}

		std::atomic<bool>  s_cornerFollow{ false };
		std::atomic<bool>  s_chOn{ false };
		std::atomic<float> s_chHeading{ 0.0f };
		std::atomic<float> s_chCone{ 0.0f };
		std::atomic<int>   s_chWrites{ 0 }, s_chClamps{ 0 }, s_chCorr{ 0 }, s_chLate{ 0 };
		std::atomic<float> s_chMaxDev{ 0.0f }, s_chLateMax{ 0.0f };

		// card M4b spike: the offset of the body (the COM bone) from the actor's Root bone in the world, found from the pivot node's parent chain (the skeleton's root is the ancestor named HumanExportRoot)
		[[nodiscard]] bool BodyShift(void* a_state, float& a_wx, float& a_wy)
		{
			auto* pivot = ReadNode(a_state, kTpsPivotNode);
			if (!pivot) {
				return false;
			}
			RE::NiAVObject* root = pivot;
			for (int i = 0; i < 12 && root; ++i) {
				const char* nm = root->name.c_str();
				if (nm && std::strcmp(nm, "HumanExportRoot") == 0) {
					break;
				}
				root = root->parent;
			}
			if (!root) {
				return false;
			}
			const RE::BSFixedString fCom{ "COM" };
			const RE::BSFixedString fRoot{ "Root" };
			auto*                   com = root->GetObjectByName(fCom);
			auto*                   rt = root->GetObjectByName(fRoot);
			if (!com || !rt) {
				return false;
			}
			a_wx = com->world.translate.x - rt->world.translate.x;
			a_wy = com->world.translate.y - rt->world.translate.y;
			return std::isfinite(a_wx) && std::isfinite(a_wy) && std::fabs(a_wx) < 3.0f && std::fabs(a_wy) < 3.0f;
		}

		// T5.3a: runs in the call-site hook before the original call, for the live third-person state only
		void ApplyCoverCam(void* a_this)
		{
			if (s_ccFaulted.load() || !IsLiveThirdPerson(a_this)) {
				return;
			}
			if (s_chOn.load()) {
				// the corner spike, every frame: the body keeps the cover's heading, the view stays inside the cone
				unsigned long hcode{ 0 };
				(void)Guarded([&] {
					if (const auto pl = RE::PlayerCharacter::GetSingleton()) {
						float dv = pl->data.angle.z - s_chHeading.load();
						while (dv > 3.14159265f) {
							dv -= 6.2831853f;
						}
						while (dv < -3.14159265f) {
							dv += 6.2831853f;
						}
						const float dd = std::fabs(dv) * 57.29578f;
						if (dd > 0.5f && s_chWrites.load() > 0) {
							s_chCorr.fetch_add(1);
							if (dd > s_chMaxDev.load()) {
								s_chMaxDev.store(dd);
							}
						}
						pl->data.angle.z = s_chHeading.load();
						s_chWrites.fetch_add(1);
					}
					const float cone = s_chCone.load();
					float       yaw{ 0.0f };
					if (cone > 0.0f && SafeRead(reinterpret_cast<const char*>(a_this) + kTpsYaw, yaw) && std::isfinite(yaw)) {
						const float c = s_chHeading.load();
						float       d = yaw - c;
						while (d > 3.14159265f) {
							d -= 6.2831853f;
						}
						while (d < -3.14159265f) {
							d += 6.2831853f;
						}
						if (std::fabs(d) > cone) {
							float ny = c + (d > 0.0f ? cone : -cone);
							while (ny >= 6.2831853f) {
								ny -= 6.2831853f;
							}
							while (ny < 0.0f) {
								ny += 6.2831853f;
							}
							*reinterpret_cast<float*>(reinterpret_cast<char*>(a_this) + kTpsYaw) = ny;
							s_chClamps.fetch_add(1);
						}
					}
				}, hcode);
			}
			const auto& cfg = Config::Get();
			const double now = Frame::Now();
			const double dt = s_ccLast > 0.0 ? std::clamp(now - s_ccLast, 0.0, 0.1) : 0.0;
			s_ccLast = now;
			float back = s_ccBackNow.load();
			float up = s_ccUpNow.load();
			if (s_ccReset.exchange(false) || !cfg.coverCamera) {
				back = 0.0f;
				up = 0.0f;
			} else {
				const float ease = (std::max)(cfg.coverCamEase, 0.05f);
				const float maxBack = (std::max)((std::max)(cfg.coverCamBackLow, cfg.coverCamBackTall), 0.01f);
				const float maxUp = (std::max)((std::max)(std::fabs(cfg.coverCamUpLow), std::fabs(cfg.coverCamUpTall)), 0.01f);
				const auto step = [&](const float a_cur, const float a_target, const float a_rate) {
					const float d = a_target - a_cur;
					const float s = a_rate * static_cast<float>(dt);
					return std::fabs(d) <= s ? a_target : a_cur + (d > 0.0f ? s : -s);
				};
				back = step(back, s_ccBackTarget.load(), maxBack / ease);
				up = step(up, s_ccUpTarget.load(), maxUp / ease);
			}
			s_ccBackNow = back;
			s_ccUpNow = up;
			FrameVals fv;
			if (cfg.framing && !s_frFaulted.load()) {
				unsigned long fcode{ 0 };
				if (!Guarded([&] { fv = ComputeFraming(a_this); }, fcode)) {
					s_frFaulted = true;
					REX::ERROR("M4a: exception 0x{:08X} working out the framing; the framing is off for this session", fcode);
					fv = FrameVals{};
				}
			}
			const bool zero = std::fabs(back) < 1e-4f && std::fabs(up) < 1e-4f && !fv.on;
			if (zero && !s_ccBoom.valid && !s_ccPivot.valid) {
				return;
			}
			NodeEdit boomEdit, pivotEdit;
			boomEdit.addY = -back;
				if (fv.on) {
					boomEdit.absY = -fv.dist;
					pivotEdit.absX = fv.x;
					pivotEdit.absY = 0.0f;
					if (s_cornerFollow.load()) {
						float wx{ 0.0f }, wy{ 0.0f }, yaw{ 0.0f };
						unsigned long ccode{ 0 };
						bool          got = false;
						(void)Guarded([&] { got = BodyShift(a_this, wx, wy) && SafeRead(reinterpret_cast<const char*>(a_this) + kTpsYaw, yaw); }, ccode);
						if (got) {
							// the view's frame: forward (sin yaw, cos yaw), right (cos yaw, -sin yaw)
							const float lx = wx * std::cos(yaw) - wy * std::sin(yaw);
							const float ly = wx * std::sin(yaw) + wy * std::cos(yaw);
							*pivotEdit.absX += lx;
							pivotEdit.absY = ly;
						}
					}  // the weapon-away graph has its pivot 0.45 m behind (y -0.449), which came on top of the boom's distance (run m4a2-1: 2.549 m behind for 2.10); the distances are behind the pivot
			}
			pivotEdit.addZ = up + fv.height;
			unsigned long code{ 0 };
			const bool    ok = Guarded(
				[&] {
					CoverCamNode(ReadNode(a_this, kTpsBoomNode), s_ccBoom, boomEdit, zero);
					CoverCamNode(ReadNode(a_this, kTpsPivotNode), s_ccPivot, pivotEdit, zero);
				},
				code);
			if (!ok) {
				s_ccFaulted = true;
				REX::ERROR("T5.3a: exception 0x{:08X} adding the cover camera offset to the rig nodes; the cover camera is off for this session", code);
				return;
			}
			if (!zero) {
				s_ccWrites.fetch_add(1);
				if (fv.on) {
					s_frWrites.fetch_add(1);
				}
			}
		}

		// ---- the camera black box (REFERENCE 23.2): one line per frame, flushed at once, so that a graphics driver reset leaves the last frames on disk ------------------------------------------
		std::FILE* s_bb{ nullptr };
		bool       s_bbTried{ false };
		bool       s_bbWarned{ false };

		void BlackBoxFrame(void* a_this, const RE::NiPoint3* a_pos, const RE::NiMatrix3* a_rot)
		{
			const auto& path = Config::Get().camBlackBox;
			if (path.empty() || !IsLiveThirdPerson(a_this)) {
				return;
			}
			if (!s_bbTried) {
				s_bbTried = true;
				if (fopen_s(&s_bb, path.c_str(), "w") != 0) {
					s_bb = nullptr;
					REX::WARN("M4a: the camera black box file '{}' could not be opened", path);
				}
			}
			if (!s_bb) {
				return;
			}
			const auto camera = RE::PlayerCamera::GetSingleton();
			const auto player = RE::PlayerCharacter::GetSingleton();
			if (!camera || !camera->cameraRoot || !player) {
				return;
			}
			float v[48]{};
			int   n = 0;
			const auto put3 = [&](const RE::NiPoint3& p) { v[n++] = p.x; v[n++] = p.y; v[n++] = p.z; };
			const auto put9 = [&](const RE::NiMatrix3& m) { for (int i = 0; i < 3; ++i) { v[n++] = m[i].x; v[n++] = m[i].y; v[n++] = m[i].z; } };
			const auto& root = camera->cameraRoot;
			put3(root->world.translate);                       // 0..2
			put9(root->world.rotate);                          // 3..11
			if (a_pos) { put3(*a_pos); } else { n += 3; }      // 12..14
			if (a_rot) { put9(*a_rot); } else { n += 9; }      // 15..23
			if (const auto boom = ReadNode(a_this, kTpsBoomNode)) { put3(boom->local.translate); put3(boom->world.translate); } else { n += 6; }    // 24..29
			if (const auto pivot = ReadNode(a_this, kTpsPivotNode)) { put3(pivot->local.translate); put3(pivot->world.translate); } else { n += 6; }  // 30..35
			put3(player->GetPosition());                       // 36..38
			float yaw{ 0.0f };
			(void)SafeRead(reinterpret_cast<const char*>(a_this) + kTpsYaw, yaw);
			float factor{ 0.0f };
			(void)SafeRead(reinterpret_cast<const char*>(a_this) + kTpsCollisionFactor, factor);
			const auto fr = GetFraming();
			v[n++] = yaw; v[n++] = factor; v[n++] = fr.dist; v[n++] = fr.pivotX; v[n++] = fr.height; v[n++] = fr.side;  // 39..44
			bool finite = true, range = false;
			for (int i = 0; i < n; ++i) {
				if (!std::isfinite(v[i])) { finite = false; } else if (std::fabs(v[i]) > 1.0e5f) { range = true; }
			}
			const auto  tp = std::chrono::system_clock::now();
			const auto  tt = std::chrono::system_clock::to_time_t(tp);
			const auto  ms = std::chrono::duration_cast<std::chrono::milliseconds>(tp.time_since_epoch()).count() % 1000;
			std::tm     lt{};
			localtime_s(&lt, &tt);
			std::fprintf(s_bb, "%02d:%02d:%02d.%03d %s sit=%d hold=%d root=(%.3f,%.3f,%.3f) rootRot=(%.3f,%.3f,%.3f|%.3f,%.3f,%.3f|%.3f,%.3f,%.3f) outPos=(%.3f,%.3f,%.3f) outRot=(%.3f,%.3f,%.3f|%.3f,%.3f,%.3f|%.3f,%.3f,%.3f) boomL=(%.3f,%.3f,%.3f) boomW=(%.3f,%.3f,%.3f) pivL=(%.3f,%.3f,%.3f) pivW=(%.3f,%.3f,%.3f) player=(%.3f,%.3f,%.3f) yaw=%.4f factor=%.3f d=%.3f x=%.3f h=%.3f side=%.2f\n",
				lt.tm_hour, lt.tm_min, lt.tm_sec, static_cast<int>(ms), !finite ? "NONFINITE" : (range ? "RANGE" : "ok"), fr.situation, s_rigHold.load() ? 1 : 0,
				v[0], v[1], v[2], v[3], v[4], v[5], v[6], v[7], v[8], v[9], v[10], v[11], v[12], v[13], v[14], v[15], v[16], v[17], v[18], v[19], v[20], v[21], v[22], v[23], v[24], v[25], v[26], v[27], v[28], v[29], v[30], v[31], v[32], v[33], v[34], v[35],
				v[36], v[37], v[38], v[39], v[40], v[41], v[42], v[43], v[44]);
			std::fflush(s_bb);
			if ((!finite || range) && !s_bbWarned) {
				s_bbWarned = true;
				REX::WARN("M4a: the camera has a {} value this frame (see the black box file)", !finite ? "NON-FINITE" : "far out of range");
			}
		}

		// ---- T5.8 probe at the call-site hook ----------------------------------------------------------------------------------------------------------------------------------
		constexpr int kPrTps = 64;                                  // +0x160 to +0x25C
		constexpr int kPrNodes = 4;                                 // +0x1F8, +0x200, +0x208, +0x210
		constexpr int kPrExtra = 20;                                // T5.8: angle x, high+0x3F4, state +0x1E4, +0x1E8, g+0xB2, g+0x64, g+0x68, camera state id (before); the same four and the root after
		constexpr int kPrN = 9 + kPrTps + kPrNodes * 18 + 9 + 9 + 3 + kPrExtra;  // rot in, tps, nodes (local + world rotation), root world rotation, rot out, pos out
		std::atomic<bool> s_hpOn{ false };
		std::mutex        s_hpMutex;
		std::string       s_hpLabel;
		float             s_prMin[kPrN], s_prMax[kPrN];
		int               s_prCount{ 0 };
		double            s_prSumD{ 0.0 };  // the sum over the frames of g[+0x68] * s1 * s2
		float             s_prFirstEff{ 0 }, s_prLastEff{ 0 }, s_prFirstEffAfter{ 0 }, s_prLastEffAfter{ 0 };
		float             s_prCur[kPrN];

		void FillMat(float* a_out, const RE::NiMatrix3& a_m)
		{
			for (int i = 0; i < 3; ++i) {
				a_out[i * 3 + 0] = a_m[i].x;
				a_out[i * 3 + 1] = a_m[i].y;
				a_out[i * 3 + 2] = a_m[i].z;
			}
		}

		[[nodiscard]] std::string ProbeName(const int a_i)
		{
			int i = a_i;
			if (i < 9) {
				return std::format("rot_in[{}]", i);
			}
			i -= 9;
			if (i < kPrTps) {
				return std::format("tps+0x{:03X}", 0x160 + 4 * i);
			}
			i -= kPrTps;
			if (i < kPrNodes * 18) {
				static constexpr const char* kNodeNames[kPrNodes] = { "boom1F8", "pivot200", "node208", "bone210" };
				const int                    n = i / 18, k = i % 18;
				return std::format("{}_{}[{}]", kNodeNames[n], k < 9 ? "local" : "world", k % 9);
			}
			i -= kPrNodes * 18;
			if (i < 9) {
				return std::format("root_world_rot[{}]", i);
			}
			i -= 9;
			if (i < 9) {
				return std::format("rot_out[{}]", i);
			}
			i -= 9;
			if (i < 3) {
				return std::format("pos_out[{}]", i);
			}
			i -= 3;
			static constexpr const char* kEx[kPrExtra] = { "angleX_before", "high3F4_before", "st1E4_before", "st1E8_before", "g_B2", "g_64", "g_68", "camStateId", "d_g68s1s2", "effPitch_before", "angleX_after", "high3F4_after", "st1E4_after",
				"st1E8_after", "effPitch_after", "x15", "x16", "x17", "x18", "x19" };
			return kEx[i];
		}

		[[nodiscard]] bool ReadF(const std::uintptr_t a_addr, float& a_out)
		{
			return SafeRead(reinterpret_cast<const void*>(a_addr), a_out);
		}

		// angle x, the high process's pitch float, the look data
		struct PitchWords
		{
			float angleX{ 0 }, high{ 0 }, gFlag{ 0 }, gX{ 0 }, gY{ 0 }, s1{ 0 }, s2{ 0 }, camId{ 0 };
			bool  ok{ false };
		};
		// the offsets above come from the planning session's reading of the listing (REFERENCE 22.15); before the first use the instructions they were read from are checked in the running image:
		// a rip-relative operand must point at the stated RVA, a displacement must be the stated one, the setter's prologue must match. A mismatch switches the pitch code off for the session.
		[[nodiscard]] bool PitchOffsetsVerified()
		{
			static int s_state = 0;  // 0 not yet, 1 verified, 2 refused
			if (s_state != 0) {
				return s_state == 1;
			}
			struct Chk
			{
				std::uintptr_t rva;
				int            len;
				char           kind;  // r = rip-relative disp32 at the end of the instruction, d = disp32 at the end, b = disp8 at the end
				std::uint64_t  expect;
			};
			static constexpr Chk kChecks[] = {
				{ 0x1E9554C, 7, 'r', kLookDataGlobalRva },  { 0x1E95553, 7, 'd', kLookDataFlag },       { 0x1E955A0, 8, 'r', kLookScale1Rva },  { 0x1E955A8, 5, 'b', kLookDataX },
				{ 0x1E955AD, 5, 'b', kLookDataY },          { 0x1E955B2, 8, 'r', kLookScale2Rva },      { 0x1E955C2, 8, 'd', kTpsLookAccumX },  { 0x1E955D2, 8, 'd', kTpsLookAccumY },
				{ 0x1E955E9, 8, 'd', kRefrAngleX },         { 0x1E95606, 8, 'd', kHighPitchOffset },    { 0x1E95618, 8, 'r', kPitchLimitRva },
			};
			const auto base = ModuleBase();
			bool       ok = true;
			for (const auto& c : kChecks) {
				std::array<std::uint8_t, 8> bytes{};
				if (SafeCopy(bytes.data(), reinterpret_cast<const void*>(base + c.rva), static_cast<std::size_t>(c.len)) != static_cast<std::size_t>(c.len)) {
					ok = false;
					break;
				}
				std::int32_t d32{ 0 };
				std::uint64_t got{ 0 };
				if (c.kind == 'b') {
					got = bytes[static_cast<std::size_t>(c.len) - 1];
				} else {
					std::memcpy(&d32, bytes.data() + c.len - 4, 4);
					got = c.kind == 'r' ? static_cast<std::uint64_t>(c.rva + static_cast<std::uintptr_t>(c.len) + static_cast<std::intptr_t>(d32)) : static_cast<std::uint64_t>(static_cast<std::uint32_t>(d32));
				}
				COVER_DETAIL("T5.8: the instruction at RVA 0x{:X} ({} bytes {:02X} {:02X} {:02X} {:02X} {:02X} {:02X} {:02X} {:02X}) gives 0x{:X}, expected 0x{:X}", c.rva, c.len, bytes[0], bytes[1], bytes[2], bytes[3], bytes[4], bytes[5], bytes[6], bytes[7], got, c.expect);
				if (got != c.expect) {
					ok = false;
				}
			}
			std::array<std::uint8_t, 5> pro{};
			if (SafeCopy(pro.data(), reinterpret_cast<const void*>(base + 0x193CDD0), pro.size()) != pro.size() || pro[0] != 0x40 || pro[1] != 0x53 || pro[2] != 0x48 || pro[3] != 0x83 || pro[4] != 0xEC) {
				ok = false;
			}
			REX::INFO("T5.8: the pitch offsets are {} (the setter at RVA 0x193CDD0 starts {:02X} {:02X} {:02X} {:02X} {:02X}, expected 40 53 48 83 EC: REX-prefixed push rbx, sub rsp,40h)", ok ? "VERIFIED" : "REFUSED: the pitch code is off for this session", pro[0], pro[1], pro[2], pro[3], pro[4]);
			s_state = ok ? 1 : 2;
			return ok;
		}

		[[nodiscard]] PitchWords ReadPitchWords()
		{
			PitchWords w;
			const auto player = RE::PlayerCharacter::GetSingleton();
			if (!player || !PitchOffsetsVerified()) {
				return w;
			}
			const auto base = ModuleBase();
			const auto* pc = reinterpret_cast<const char*>(player);
			w.ok = SafeRead(pc + kRefrAngleX, w.angleX);
			std::uintptr_t proc{ 0 }, high{ 0 };
			if (SafeRead(pc + 0x228, proc) && proc && SafeRead(reinterpret_cast<const char*>(proc) + kProcessHigh, high) && high) {
				(void)SafeRead(reinterpret_cast<const char*>(high) + kHighPitchOffset, w.high);
			}
			std::uintptr_t g{ 0 };
			if (SafeRead(reinterpret_cast<const void*>(base + kLookDataGlobalRva), g) && g) {
				std::uint8_t fb{ 0 };
				if (SafeRead(reinterpret_cast<const char*>(g) + kLookDataFlag, fb)) {
					w.gFlag = static_cast<float>(fb);
				}
				(void)SafeRead(reinterpret_cast<const char*>(g) + kLookDataX, w.gX);
				(void)SafeRead(reinterpret_cast<const char*>(g) + kLookDataY, w.gY);
			}
			(void)ReadF(base + kLookScale1Rva, w.s1);
			(void)ReadF(base + kLookScale2Rva, w.s2);
			if (const auto camera = RE::PlayerCamera::GetSingleton(); camera && camera->currentState) {
				std::uint32_t id{ 0 };
				if (SafeRead(reinterpret_cast<const char*>(camera->currentState) + 0x50, id)) {
					w.camId = static_cast<float>(id);
				}
			}
			return w;
		}

		void ProbeBefore(void* a_this, RE::NiMatrix3* a_rot)
		{
			std::fill(std::begin(s_prCur), std::end(s_prCur), 0.0f);
			if (a_rot) {
				FillMat(s_prCur, *a_rot);
			}
			int base = 9;
			for (int i = 0; i < kPrTps; ++i) {
				(void)SafeRead(reinterpret_cast<const char*>(a_this) + 0x160 + 4 * i, s_prCur[base + i]);
			}
			base += kPrTps;
			const std::ptrdiff_t offs[kPrNodes] = { kTpsBoomNode, kTpsPivotNode, kTpsFovNode, kTpsBoneNode };
			for (int n = 0; n < kPrNodes; ++n) {
				if (const auto node = ReadNode(a_this, offs[n])) {
					FillMat(s_prCur + base, node->local.rotate);
					FillMat(s_prCur + base + 9, node->world.rotate);
				}
				base += 18;
			}
			if (const auto camera = RE::PlayerCamera::GetSingleton(); camera && camera->cameraRoot) {
				FillMat(s_prCur + base, camera->cameraRoot->world.rotate);
			}
			const int ex = 9 + kPrTps + kPrNodes * 18 + 9 + 9 + 3;
			const auto w = ReadPitchWords();
			s_prCur[ex + 0] = w.angleX;
			s_prCur[ex + 1] = w.high;
			(void)SafeRead(reinterpret_cast<const char*>(a_this) + kTpsLookAccumX, s_prCur[ex + 2]);
			(void)SafeRead(reinterpret_cast<const char*>(a_this) + kTpsLookAccumY, s_prCur[ex + 3]);
			s_prCur[ex + 4] = w.gFlag;
			s_prCur[ex + 5] = w.gX;
			s_prCur[ex + 6] = w.gY;
			s_prCur[ex + 7] = w.camId;
			s_prCur[ex + 8] = w.gY * w.s1 * w.s2;
			s_prCur[ex + 9] = w.angleX + w.high;
		}

		void ProbeAfter(void* a_this, RE::NiPoint3* a_pos, RE::NiMatrix3* a_rot)
		{
			{
				const int ex = 9 + kPrTps + kPrNodes * 18 + 9 + 9 + 3;
				const auto w = ReadPitchWords();
				s_prCur[ex + 10] = w.angleX;
				s_prCur[ex + 11] = w.high;
				(void)SafeRead(reinterpret_cast<const char*>(a_this) + kTpsLookAccumX, s_prCur[ex + 12]);
				(void)SafeRead(reinterpret_cast<const char*>(a_this) + kTpsLookAccumY, s_prCur[ex + 13]);
				s_prCur[ex + 14] = w.angleX + w.high;
				s_prSumD += s_prCur[ex + 8];
				if (s_prCount == 0) {
					s_prFirstEff = s_prCur[ex + 9];
				}
				s_prLastEff = s_prCur[ex + 9];
				if (s_prCount == 0) {
					s_prFirstEffAfter = s_prCur[ex + 14];
				}
				s_prLastEffAfter = s_prCur[ex + 14];
			}
			const int base = 9 + kPrTps + kPrNodes * 18 + 9;
			if (a_rot) {
				FillMat(s_prCur + base, *a_rot);
			}
			if (a_pos) {
				s_prCur[base + 9] = a_pos->x;
				s_prCur[base + 10] = a_pos->y;
				s_prCur[base + 11] = a_pos->z;
			}
			std::lock_guard lock(s_hpMutex);
			for (int i = 0; i < kPrN; ++i) {
				if (s_prCount == 0) {
					s_prMin[i] = s_prMax[i] = s_prCur[i];
				} else {
					s_prMin[i] = (std::min)(s_prMin[i], s_prCur[i]);
					s_prMax[i] = (std::max)(s_prMax[i], s_prCur[i]);
				}
			}
			++s_prCount;
		}

		// ---- T5.8: the view's pitch in the pose (REFERENCE 22.14) -----------------------------------------------------------------------------------------------------------
		// On the own graph the matrix handed to the pose call (call A) carries the look pitch; in the pose it does not (the probe of run m3e-2). The hook learns on the own graph how much the
		// view's pitch changes per mouse count and per stick unit and second (a least-squares fit of the frame-to-frame change of the pitch against the look input of the same frame, for each of the
		// two mouse ints and for the stick, separately with and without the aim), and in the pose (bPoseFreePitch) integrates a pitch of its own from the look input with those factors and writes
		// the pitch part of the matrix (the rows forward and up; the heading and the row right are kept) before the original call.
		struct PitchFit
		{
			double num[2][2]{}, den[2][2]{}, sphi[2][2]{};  // [aim][mouse int a, b]
			double numS[2]{}, denS[2]{}, sphiS[2]{};        // [aim] the stick
			int    n[2]{};
		};
		PitchFit s_pf;
		bool     s_pfHavePrev{ false };
		float    s_pfPrev{ 0.0f };
		double   s_pfPrevT{ 0.0 };
		bool     s_pfDrive{ false };
		float    s_pfPhi{ 0.0f };
		double   s_pfLogT{ 0.0 };

		[[nodiscard]] float PitchOf(const RE::NiMatrix3& a_m)
		{
			return std::asin(std::clamp(a_m[1].z, -1.0f, 1.0f));  // the forward row's z: positive = looking up
		}

		[[nodiscard]] bool now_log_due()
		{
			const double now = Frame::Now();
			if (now - s_pfLogT > 1.0) {
				s_pfLogT = now;
				return true;
			}
			return false;
		}

		// T5.8, the pitch of our own while the pose is on (REFERENCE 22.15): the engine adds -(g[+0x68] * s1 * s2) to the player's angle x each frame on the own graph; in the pose angle x stays put. The
		// hook learns the factor between that look value and the change of the effective pitch (angle x + the high process's float) on the own graph, keeps a pitch of its own while Posed (started from
		// the effective pitch at that moment, clamped to about +-60 degrees) and writes angle x = pitch - high float before the original pose call, and again right after it.
		double s_pfNum{ 0.0 }, s_pfDen{ 0.0 };
		float  s_pfPrevEff{ 0.0f };
		bool   s_pfHavePrevEff{ false };
		bool   s_pfWrote{ false };
		float  s_pfWantAngleX{ 0.0f };

		// run m3f-2 (own graph): angle x (player + 0x80) stays 0 and the high process's float (+0x3F4) carries the pitch (-0.18 .. +0.69 rad with the mouse): the drive writes that float
		bool WriteHighPitch(const float a_value)
		{
			const auto player = RE::PlayerCharacter::GetSingleton();
			if (!player) {
				return false;
			}
			std::uintptr_t proc{ 0 }, high{ 0 };
			const auto* pc = reinterpret_cast<const char*>(player);
			if (!SafeRead(pc + 0x228, proc) || !proc || !SafeRead(reinterpret_cast<const char*>(proc) + kProcessHigh, high) || !high) {
				return false;
			}
			*reinterpret_cast<float*>(reinterpret_cast<char*>(high) + kHighPitchOffset) = a_value;
			return true;
		}

		void FreePitchAfter()
		{
			if (s_pfDrive && s_pfWrote) {
				(void)WriteHighPitch(s_pfWantAngleX);  // put ours back after the pose function (it writes angle x at its end, RVA 0x1E9C0EB)
			}
		}

		void FreePitchStep(void* a_this, RE::NiMatrix3* a_rot)
		{
			(void)a_rot;
			if (!Config::Get().poseFreePitch && !s_hpOn.load()) {
				s_pfDrive = false;
				return;
			}
			const auto camera = RE::PlayerCamera::GetSingleton();
			if (!camera || a_this != camera->cameraStates[RE::CameraStates::kThirdPerson]) {
				return;
			}
			const auto w = ReadPitchWords();
			if (!w.ok) {
				return;
			}
			const float eff = w.angleX + w.high;
			const float d = w.gY * w.s1 * w.s2;  // not gated on g+0xB2: it reads 0 on the own graph as well (run m3g-1), the pitch comes through the state's own look path (the virtual call at RVA 0x1E9557C)
			const auto  pstate = CoverPose::GetInfo().state;
			const bool  own = pstate == CoverPose::State::Off && CoverPose::VarValue("bIsInCover") < 0.5;
			if (own && s_pfHavePrevEff && std::fabs(d) > 1e-6f) {
				const double dEff = static_cast<double>(eff) - s_pfPrevEff;
				if (std::fabs(dEff) < 0.3) {
					s_pfNum += dEff * d;
					s_pfDen += static_cast<double>(d) * d;
				}
			}
			s_pfPrevEff = eff;
			s_pfHavePrevEff = true;
			if (!Config::Get().poseFreePitch || pstate != CoverPose::State::Posed) {
				s_pfDrive = false;
				s_pfWrote = false;
				return;
			}
			const double k = s_pfDen > 1e-6 ? s_pfNum / s_pfDen : -0.5;  // learned on the own graph; before any own-graph look the value the rig measured (-0.504)
			if (!s_pfDrive) {
				s_pfDrive = true;
				s_pfPhi = eff;
				COVER_DETAIL("T5.8: the pitch of the pose is driven from the look data: start {:+.1f} deg (angle x {:+.4f}, high {:+.4f}); the factor between g[+0x68]*s1*s2 and the change of the pitch learned on the own graph: {:+.3f} (the engine's: -1)",
					eff * 57.29578f, w.angleX, w.high, k);
			}
			s_pfPhi = std::clamp(s_pfPhi + static_cast<float>(k * d), -1.05f, 1.05f);
			s_pfWantAngleX = s_pfPhi - w.angleX;  // the effective pitch is angle x + the high float; angle x is 0 on the own graph
			if (now_log_due()) {
				COVER_DETAIL("T5.8: pitch {:+.1f} deg wanted, the engine's {:+.1f}; look value {:+.5f}", s_pfPhi * 57.29578f, eff * 57.29578f, d);
			}
			s_pfWrote = WriteHighPitch(s_pfWantAngleX);
		}

		// ---- T5.2c: the yaw probe ------------------------------------------------------------------------------------------------------------------------------------------------
		struct YawRow
		{
			double t;
			float  angZ, off, sum, tpsYaw;
			float  rx, ry, rz, dHead;
			int    pose;
			float  exiting, idleLoc;
			float  bx, by, bz, px, py, pz, factor, ax, ay, az, cx, cy, cz;
			// after the original call: the boom node's local and world translate, the pivot node's world translate, the camera root's world translate, the hold and the cover camera
			float  abl[3]{}, abw[3]{}, apw[3]{}, arw[3]{};
			int    hold{ 0 };
			float  ccBack{ 0.0f }, ccUp{ 0.0f };
			int    flag291{ -1 }, bone{ 0 }, n218{ 0 };
			float  blend{ 0.0f }, v1cc[3]{};
		};
		std::atomic<bool>    s_ypOn{ false };
		std::mutex           s_ypMutex;
		std::vector<YawRow>  s_ypRows;
		std::string          s_ypLabel;
		double               s_ypT0{ 0.0 };

		void YawProbeFrame(void* a_this)
		{
			const auto player = RE::PlayerCharacter::GetSingleton();
			const auto camera = RE::PlayerCamera::GetSingleton();
			if (!player || !camera || a_this != camera->cameraStates[RE::CameraStates::kThirdPerson]) {
				return;
			}
			YawRow r{};
			r.t = Frame::Now() - s_ypT0;
			const auto* pc = reinterpret_cast<const char*>(player);
			(void)SafeRead(pc + 0x88, r.angZ);
			(void)SafeRead(pc + 0xE2C, r.off);
			r.sum = std::fmod(r.angZ + r.off, 6.2831853f);
			if (r.sum < 0.0f) {
				r.sum += 6.2831853f;
			}
			(void)SafeRead(reinterpret_cast<const char*>(a_this) + kTpsYaw, r.tpsYaw);
			if (camera->cameraRoot) {
				const auto& w = camera->cameraRoot->world.translate;
				r.rx = w.x;
				r.ry = w.y;
				r.rz = w.z;
				const auto pp = player->GetPosition();
				const float dx = w.x - pp.x, dy = w.y - pp.y, dz = w.z - (pp.z + 1.55f);
				r.dHead = std::sqrt(dx * dx + dy * dy + dz * dz);
			}
			{
				const auto rg = Camera::ReadRig();
				if (rg.ok) {
					r.bx = rg.n1F8.t.x; r.by = rg.n1F8.t.y; r.bz = rg.n1F8.t.z;
					r.px = rg.n200.t.x; r.py = rg.n200.t.y; r.pz = rg.n200.t.z;
					r.factor = rg.factor;
					r.ax = rg.p190.x; r.ay = rg.p190.y; r.az = rg.p190.z;
					r.cx = rg.p19c.x; r.cy = rg.p19c.y; r.cz = rg.p19c.z;
					r.flag291 = rg.haveFlag ? static_cast<int>(rg.flag291) : -1;
					r.bone = rg.n210.present ? 1 : 0;
					r.n218 = rg.n218.present ? 1 : 0;
					r.blend = rg.blend;
					(void)SafeRead(reinterpret_cast<const char*>(a_this) + 0x1CC, r.v1cc);
				}
			}
			r.pose = static_cast<int>(CoverPose::GetInfo().state);
			r.exiting = static_cast<float>(CoverPose::VarValue("bIsExitingCover"));
			r.idleLoc = static_cast<float>(CoverPose::VarValue("iSyncIdleLocomotion"));
			std::lock_guard lock(s_ypMutex);
			if (s_ypRows.size() < 4000) {
				s_ypRows.push_back(r);
			}
		}

		void YawProbeAfter(void* a_this)
		{
			const auto camera = RE::PlayerCamera::GetSingleton();
			if (!camera || a_this != camera->cameraStates[RE::CameraStates::kThirdPerson]) {
				return;
			}
			std::lock_guard lock(s_ypMutex);
			if (s_ypRows.empty()) {
				return;
			}
			auto& r = s_ypRows.back();
			if (const auto boom = ReadNode(a_this, kTpsBoomNode)) {
				r.abl[0] = boom->local.translate.x; r.abl[1] = boom->local.translate.y; r.abl[2] = boom->local.translate.z;
				r.abw[0] = boom->world.translate.x; r.abw[1] = boom->world.translate.y; r.abw[2] = boom->world.translate.z;
			}
			if (const auto pivot = ReadNode(a_this, kTpsPivotNode)) {
				r.apw[0] = pivot->world.translate.x; r.apw[1] = pivot->world.translate.y; r.apw[2] = pivot->world.translate.z;
			}
			if (camera->cameraRoot) {
				const auto& w = camera->cameraRoot->world.translate;
				r.arw[0] = w.x; r.arw[1] = w.y; r.arw[2] = w.z;
			}
			r.hold = s_rigHold.load() ? 1 : 0;
			const auto cc = GetCoverCam();
			r.ccBack = cc.backNow;
			r.ccUp = cc.upNow;
		}

		void HookSite(void* a_this, RE::NiPoint3* a_pos, RE::NiMatrix3* a_rot)
		{
			if (s_ypOn.load()) {
				unsigned long yc{ 0 };
				(void)Guarded([&] { YawProbeFrame(a_this); }, yc);
			}
			const auto orig = s_origSite;
			if (!orig) {
				return;
			}
			if (s_compat.load()) {
				// every cast of the plugin runs here, before the original call of this site (which runs before the engine camera collision casts, REFERENCE 14.8 and 19), on the
				// engine's own thread, only for the live third-person state
				if (const auto camera = RE::PlayerCamera::GetSingleton(); camera && a_this && a_this == camera->cameraStates[RE::CameraStates::kThirdPerson]) {
					s_callsThird.fetch_add(1);
					s_siteCalls.fetch_add(1);
					s_lastThirdTime.store(Frame::Now());
					WorldProbe::RunQueued();
				}
				ApplyRigHold(a_this);
				ApplyCoverCam(a_this);
					HoldWorldFix(a_this);
				{
					unsigned long fc{ 0 };
					(void)Guarded([&] { FreePitchStep(a_this, a_rot); }, fc);
				}
				bool probe = false;
				if (s_hpOn.load()) {
					if (const auto camera = RE::PlayerCamera::GetSingleton(); camera && a_this == camera->cameraStates[RE::CameraStates::kThirdPerson]) {
						unsigned long pc{ 0 };
						probe = Guarded([&] { ProbeBefore(a_this, a_rot); }, pc);
					}
				}
				orig(a_this, a_pos, a_rot);
				{ unsigned long ec{ 0 }; (void)Guarded([&] { EntryBlendAfter(a_this, a_pos); }, ec); }
				if (s_ypOn.load()) { unsigned long yc{ 0 }; (void)Guarded([&] { YawProbeAfter(a_this); }, yc); }
				{
					unsigned long fa{ 0 };
					(void)Guarded([&] { FreePitchAfter(); }, fa);
				}
				if (probe) {
					unsigned long pc{ 0 };
					(void)Guarded([&] { ProbeAfter(a_this, a_pos, a_rot); }, pc);
				}
				return;
			}
			ApplyRigHold(a_this);
			ApplyCoverCam(a_this);
					HoldWorldFix(a_this);
			{
				unsigned long fc{ 0 };
				(void)Guarded([&] { FreePitchStep(a_this, a_rot); }, fc);
			}
			bool probe = false;
			if (s_hpOn.load()) {
				if (const auto camera = RE::PlayerCamera::GetSingleton(); camera && a_this == camera->cameraStates[RE::CameraStates::kThirdPerson]) {
					unsigned long pc{ 0 };
					probe = Guarded([&] { ProbeBefore(a_this, a_rot); }, pc);
				}
			}
			orig(a_this, a_pos, a_rot);
				if (s_chOn.load()) {
					unsigned long lc{ 0 };
					(void)Guarded([&] {
						if (const auto pl = RE::PlayerCharacter::GetSingleton()) {
							float dv = pl->data.angle.z - s_chHeading.load();
							while (dv > 3.14159265f) {
								dv -= 6.2831853f;
							}
							while (dv < -3.14159265f) {
								dv += 6.2831853f;
							}
							const float dd = std::fabs(dv) * 57.29578f;
							if (dd > 0.5f) {
								s_chLate.fetch_add(1);
								if (dd > s_chLateMax.load()) {
									s_chLateMax.store(dd);
								}
							}
						}
					}, lc);
				}
				{ unsigned long bc{ 0 }; (void)Guarded([&] { BlackBoxFrame(a_this, a_pos, a_rot); }, bc); }
				{ unsigned long ec{ 0 }; (void)Guarded([&] { EntryBlendAfter(a_this, a_pos); }, ec); }
				if (s_ypOn.load()) { unsigned long yc{ 0 }; (void)Guarded([&] { YawProbeAfter(a_this); }, yc); }
			{
				unsigned long fa{ 0 };
				(void)Guarded([&] { FreePitchAfter(); }, fa);
			}
			if (probe) {
				unsigned long pc{ 0 };
				(void)Guarded([&] { ProbeAfter(a_this, a_pos, a_rot); }, pc);
			}
			if (s_faulted.load() || !Config::Get().camera) {
				return;
			}
			unsigned long code{ 0 };
			if (!Guarded([&] { SiteAfter(a_this, a_pos, a_rot); }, code)) {
				s_faulted = true;
				REX::ERROR("S4: exception 0x{:08X} in the call-site hook after the original ran; the camera offset is off for this session", code);
			}
		}

		void TryInstallSite()
		{
			const auto base = ModuleBase();
			const auto fn = kThirdPersonUpdateId.address();
			const auto site = fn + static_cast<std::uintptr_t>(kThirdPersonPoseCallOffset);
			std::array<std::uint8_t, 5> b{};
			if (SafeCopy(b.data(), reinterpret_cast<const void*>(site), b.size()) != b.size()) {
				REX::ERROR("S4: cannot read the five bytes at Update + 0x{:X}; call-site hook NOT installed", static_cast<std::uint32_t>(kThirdPersonPoseCallOffset));
				return;
			}
			std::int32_t rel{ 0 };
			std::memcpy(&rel, b.data() + 1, sizeof(rel));
			const auto target = site + 5 + static_cast<std::intptr_t>(rel);
			COVER_DETAIL("S4: Update (ID {}) at RVA 0x{:X}; bytes at + 0x{:X}: {:02X} {:02X} {:02X} {:02X} {:02X}; call target RVA 0x{:X} (expected 0x{:X})", kThirdPersonUpdateId.id(), fn - base,
				static_cast<std::uint32_t>(kThirdPersonPoseCallOffset), b[0], b[1], b[2], b[3], b[4], target - base, kThirdPersonPoseCallTargetRva);
			if (b[0] != 0xE8 || target - base != kThirdPersonPoseCallTargetRva) {
				REX::ERROR("S4: the call site is not the expected `call 0x{:X}`; call-site hook NOT installed", kThirdPersonPoseCallTargetRva);
				return;
			}
			REL::Relocation<std::uintptr_t> r{ kThirdPersonUpdateId };
			const auto                      orig = r.write_call<5, 0x431>(reinterpret_cast<std::uintptr_t>(&HookSite));
			if (orig != target) {
				REX::ERROR("S4: write_call returned original target RVA 0x{:X}, not the expected one; the hook was written but its pass-through may be wrong", orig - base);
			}
			s_origSite = reinterpret_cast<Site_t>(orig);
			s_siteInstalled = true;
			REX::INFO("S4: call-site hook installed at Update + 0x{:X}", static_cast<std::uint32_t>(kThirdPersonPoseCallOffset));
		}

		void Hook(void* a_this, void* a_next)
		{
			const auto orig = s_orig;
			if (!orig) {
				return;
			}
			// T2.0: every cast of the plugin runs here, on the camera's thread, before the engine's own camera collision cast, so the
			// engine's cast overwrites the hit fraction in PlayerCamera + 0x2B4 afterwards (docs/REFERENCE.md 14.8). RunQueued is guarded.
			if (const auto camera = RE::PlayerCamera::GetSingleton(); camera && a_this && a_this == camera->cameraStates[RE::CameraStates::kThirdPerson]) {
				WorldProbe::RunQueued();
			}
			PivotWrite pw;
			if (!s_faulted.load() && Config::Get().camera) {
				unsigned long pcode{ 0 };
				if (!Guarded([&] { PivotBefore(a_this, pw); }, pcode)) {
					pw = PivotWrite{};
					s_faulted = true;
					REX::ERROR("S4b: exception 0x{:08X} before the original Update (pivot shift); the camera offset is off for this session", pcode);
				}
			}
			orig(a_this, a_next);
			if (pw.node) {
				unsigned long qcode{ 0 };
				if (!Guarded([&] { PivotAfter(pw); }, qcode)) {
					s_faulted = true;
					REX::ERROR("S4b: exception 0x{:08X} while writing the pivot node back; the camera offset is off for this session", qcode);
				}
			}

			if (s_faulted.load() || !Config::Get().camera) {
				return;
			}
			unsigned long code{ 0 };
			if (!Guarded([&] { AfterUpdate(a_this); }, code)) {
				s_faulted = true;
				REX::ERROR("S4: exception 0x{:08X} in the camera hook after the original ran; the camera offset is off for this session", code);
			}
		}

		// M4b11: the module whose code a function pointer lies in. The file name is lower case without the path; known is false when the address is in no loaded module (a trampoline block, say).
		// a_hintList: names tried by their module handle when the file name cannot be read.
		struct CodeOwner
		{
			bool          known{ false };
			bool          inGame{ false };
			std::string   file{};
			std::uintptr_t offset{ 0 };
		};

		[[nodiscard]] CodeOwner OwnerOfCode(const std::uintptr_t a_addr, const std::string& a_hintList)
		{
			CodeOwner                          o;
			REX::W32::MEMORY_BASIC_INFORMATION mbi{};
			if (REX::W32::VirtualQuery(reinterpret_cast<void*>(a_addr), &mbi, sizeof(mbi)) == 0 || !mbi.allocationBase) {
				return o;
			}
			const auto base = reinterpret_cast<std::uintptr_t>(mbi.allocationBase);
			o.offset = a_addr - base;
			if (base == ModuleBase()) {
				o.known = true;
				o.inGame = true;
				o.file = "starfield.exe";
				return o;
			}
			char       name[260]{};
			const auto n = REX::W32::GetModuleFileNameA(reinterpret_cast<REX::W32::HMODULE>(mbi.allocationBase), name, 259);
			if (n != 0) {
				std::string file = name;
				if (const auto slash = file.find_last_of("\\/"); slash != std::string::npos) {
					file = file.substr(slash + 1);
				}
				for (auto& c : file) {
					c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
				}
				o.file = file;
				o.known = true;
				return o;
			}
			for (const auto& entry : Core::ParseModuleList(a_hintList)) {
				std::wstring wide(entry.begin(), entry.end());
				const auto   h = REX::W32::GetModuleHandleW(wide.c_str());
				if (h && reinterpret_cast<std::uintptr_t>(h) == base) {
					o.file = entry;
					o.known = true;
					return o;
				}
			}
			return o;
		}

		[[nodiscard]] std::string OwnerText(const CodeOwner& a_o, const std::uintptr_t a_addr)
		{
			if (!a_o.known) {
				return std::format("an address in no loaded module (0x{:X})", a_addr);
			}
			return a_o.inGame ? std::format("Starfield.exe+0x{:X}", a_o.offset) : std::format("{}+0x{:X}", a_o.file, a_o.offset);
		}

		void TryInstallImpl()
		{
			const auto vtbl = kThirdPersonStateVtable.address();
			const auto slotAddr = vtbl + sizeof(void*) * kCameraStateUpdateSlot;
			std::uintptr_t slot{ 0 };
			if (!SafeRead(reinterpret_cast<const void*>(slotAddr), slot)) {
				REX::ERROR("S4: cannot read slot {} of the ThirdPersonState vtable (ID {}); camera hook NOT installed", kCameraStateUpdateSlot, kThirdPersonStateVtable.id());
				s_gaveUp = true;
				return;
			}
			const auto rva = slot - ModuleBase();
			REX::INFO("S4: ThirdPersonState vtable ID {} at RVA 0x{:X}; slot {} points to RVA 0x{:X} (expected 0x{:X})", kThirdPersonStateVtable.id(), vtbl - ModuleBase(),
				kCameraStateUpdateSlot, rva, kThirdPersonUpdateRva);
			const bool stride = REX::W32::GetModuleHandleW(L"DyMC.dll") != nullptr;
			const bool holdsGame = rva == kThirdPersonUpdateRva;
			// M4b11: the owner of the code in the slot is named in every case (REFERENCE 23.27)
			const auto owner = OwnerOfCode(slot, Config::Get().cameraChainModules);
			REX::INFO("S4: slot {} holds {}{}", kCameraStateUpdateSlot, OwnerText(owner, slot), holdsGame ? " (the game's own ThirdPersonState::Update)" : "");
			Core::CameraSlotInput in;
			in.slotHoldsGameUpdate = holdsGame;
			in.strideLoaded = stride;
			in.chainMode = Config::Get().cameraChain;
			in.owner = owner.known ? owner.file : std::string();
			in.allowList = Config::Get().cameraChainModules;
			const auto decision = Core::DecideCameraSlot(in);
			REX::INFO("S4: slot {} decision: {} ({})", kCameraStateUpdateSlot, decision.decision == Core::CameraSlotDecision::Own ? "own hook" : decision.decision == Core::CameraSlotDecision::Chain ? "CHAIN" : "stand down", decision.reason);

			const auto standDown = [&](const std::string& a_why) {
				REX::INFO("S4: COMPATIBILITY MODE: {}. {} does not touch the ThirdPersonState vtable (slot {}); its cast queue runs from the call-site hook at Update + 0x{:X}; its own camera (pivot shift, the flip key) is off for this session",
					a_why, Config::DisplayName(), kCameraStateUpdateSlot, static_cast<std::uint32_t>(kThirdPersonPoseCallOffset));
				s_compat = true;
				TryInstallSite();
				if (s_siteInstalled.load()) {
					s_installed = true;  // the rig and the world probe see a working Update pump
				} else {
					REX::ERROR("S4: compatibility mode, but the call-site hook could not be installed: the cast queue has no pump, cover sensing is off for this session");
					s_gaveUp = true;
				}
			};
			if (decision.decision == Core::CameraSlotDecision::Stand) {
				standDown(stride && !holdsGame ? std::format("DyMC.dll (Stride) is loaded in this process and slot {} does not hold the game function (RVA 0x{:X} instead of 0x{:X})", kCameraStateUpdateSlot, rva, kThirdPersonUpdateRva) :
				          stride           ? std::string("DyMC.dll (Stride) is loaded in this process") :
				                             std::format("slot {} does not hold the game function (RVA 0x{:X} instead of 0x{:X}: another plugin hooked it); {}", kCameraStateUpdateSlot, rva, kThirdPersonUpdateRva, decision.reason));
				return;
			}
			if (slot == reinterpret_cast<std::uintptr_t>(&Hook)) {
				REX::ERROR("S4: slot {} already holds our own hook; nothing to install", kCameraStateUpdateSlot);
				s_gaveUp = true;
				return;
			}

			// The live third-person state object, when it exists, should use exactly this vtable.
			if (const auto camera = RE::PlayerCamera::GetSingleton(); camera && camera->cameraStates[RE::CameraStates::kThirdPerson]) {
				std::uintptr_t live{ 0 };
				if (SafeRead(camera->cameraStates[RE::CameraStates::kThirdPerson], live)) {
					COVER_DETAIL("S4: live cameraStates[third]={} has vptr 0x{:X} (RVA 0x{:X}); {} the ThirdPersonState vtable", camera->cameraStates[RE::CameraStates::kThirdPerson], live,
						live - ModuleBase(), live == vtbl ? "equals" : "DIFFERS from");
				}
			}

			s_orig = reinterpret_cast<Orig_t>(slot);
			REL::Relocation<std::uintptr_t> r{ vtbl };
			r.write_vfunc(kCameraStateUpdateSlot, reinterpret_cast<std::uintptr_t>(&Hook));
			// M4b11: the write is verified; when it did not take, the plugin stands down as before
			std::uintptr_t after{ 0 };
			if (!SafeRead(reinterpret_cast<const void*>(slotAddr), after) || after != reinterpret_cast<std::uintptr_t>(&Hook)) {
				REX::ERROR("S4: the write to slot {} did not take (it reads 0x{:X} afterwards, expected our hook at 0x{:X})", kCameraStateUpdateSlot, after, reinterpret_cast<std::uintptr_t>(&Hook));
				s_orig = nullptr;
				standDown("the write to the vtable slot could not be verified");
				return;
			}
			s_installed = true;
			if (decision.decision == Core::CameraSlotDecision::Chain) {
				s_chained = true;
				REX::INFO("S4: camera hook installed on the ThirdPersonState vtable, slot {}, CHAINED in front of {}: its function is called as the original; the own camera is on", kCameraStateUpdateSlot, OwnerText(owner, slot));
			} else {
				REX::INFO("S4: camera hook installed on the ThirdPersonState vtable, slot {}", kCameraStateUpdateSlot);
			}

			if (Config::Get().camera) {
				TryInstallSite();
			}
		}

		// Collision limit: how far sideways can the camera go before a sphere cast from the root hits something. The cast is queued
		// and answered a frame later (T2.0), so this keeps one request open and applies its answer when it arrives.
		WorldProbe::Ticket s_allowedTicket{ 0 };
		RE::NiPoint3       s_allowedFrom{};

		void ComputeAllowed(const Frame::Snapshot& a_snap)
		{
			const auto& cfg = Config::Get();
			const float target = s_target.load();
			if (std::fabs(target) < 0.0005f || !cfg.cameraCollision || CurrentMode() == 4) {
				s_allowed = 1000.0f;
				s_allowedTicket = 0;
				return;
			}
			if (s_allowedTicket != 0) {
				const auto r = WorldProbe::Poll(s_allowedTicket);
				if (!r) {
					return;  // still waiting
				}
				s_allowedTicket = 0;
				if (r->ok) {
					float allowed = 1000.0f;
					if (r->hit) {
						const float dx = r->safePoint.x - s_allowedFrom.x;
						const float dy = r->safePoint.y - s_allowedFrom.y;
						allowed = (std::max)(0.0f, std::sqrt(dx * dx + dy * dy) - cfg.cameraClearance);
					}
					s_allowed = allowed;
				}
				return;
			}
			const auto camera = RE::PlayerCamera::GetSingleton();
			const auto player = RE::PlayerCharacter::GetSingleton();
			if (!camera || !camera->cameraRoot || !player) {
				return;
			}
			const auto pp = player->GetPosition();
			const auto rootPos = camera->cameraRoot->world.translate - RE::NiPoint3{ 0.0f, 0.0f, 0.0f };
			// Use the root position without our own offset: remove what the hook added (the applied offset along right).
			float fx = pp.x - rootPos.x;
			float fy = pp.y - rootPos.y;
			const float len = std::sqrt(fx * fx + fy * fy);
			if (len < 0.05f) {
				return;
			}
			fx /= len;
			fy /= len;
			const float        applied = s_applied.load();
			const RE::NiPoint3 right{ fy, -fx, 0.0f };
			const RE::NiPoint3 from{ rootPos.x - right.x * applied, rootPos.y - right.y * applied, rootPos.z - right.z * applied };
			const float        dir = target < 0.0f ? -1.0f : 1.0f;
			const float        reach = std::fabs(target) + cfg.cameraClearance;
			const RE::NiPoint3 to{ from.x + right.x * dir * reach, from.y + right.y * dir * reach, from.z };
			s_allowedFrom = from;
			s_allowedTicket = WorldProbe::Submit(from, to, cfg.probeRadius);
			(void)a_snap;
		}
	}

	void OnTick(const Frame::Snapshot& a_snap)
	{
		const auto& cfg = Config::Get();
		// The Update hook also carries the world probe's cast queue (T2.0), so it is installed when either is on.
		if (!cfg.enabled || (!cfg.camera && !cfg.worldProbe) || s_faulted.load()) {
			return;
		}

		if (a_snap.valid && cfg.camera) {
			CameraSettings::LogAllOnce();
		}

		if (!s_installed && !s_gaveUp && a_snap.time >= s_nextInstall && RE::PlayerCamera::GetSingleton()) {
			s_nextInstall = a_snap.time + 1.0;
			unsigned long code{ 0 };
			if (!Guarded([] { TryInstallImpl(); }, code)) {
				s_gaveUp = true;
				REX::ERROR("S4: exception 0x{:08X} while installing the camera hook; not installed", code);
			} else if (s_installed && cfg.shoulderOffset != 0.0f) {
				s_target = cfg.shoulderOffset;
			}
		}
		if (s_compat.load() && s_installed.load()) {
			// T4.0: no camera work; only the rate of the call-site pump, every 5 s
			if (a_snap.time >= s_nextStats) {
				s_nextStats = a_snap.time + 5.0;
				COVER_DETAIL("S4 compat: the call-site hook ran {:.1f} times a second on the third-person state ({} calls in all), cast queue available {}", static_cast<double>(s_callsThird.load() - s_statsCallsThird) / 5.0, s_callsThird.load(), WorldProbe::Available());
				s_statsCallsThird = s_callsThird.load();
			}
			return;
		}
		if (s_installed.load() && !s_slotLostLogged.load() && a_snap.time >= s_nextSlotCheck) {
			// M4b11: log once when another plugin writes the slot after us (the hook would then never be called and the own camera would silently be off)
			s_nextSlotCheck = a_snap.time + 2.0;
			std::uintptr_t now{ 0 };
			const auto     slotAddr = kThirdPersonStateVtable.address() + sizeof(void*) * kCameraStateUpdateSlot;
			if (SafeRead(reinterpret_cast<const void*>(slotAddr), now) && now != reinterpret_cast<std::uintptr_t>(&Hook)) {
				s_slotLostLogged = true;
				const auto o = OwnerOfCode(now, Config::Get().cameraChainModules);
				REX::WARN("S4: slot {} no longer holds our hook: it now holds {}. Another plugin wrote it after us; the own camera is not called until the game restarts", kCameraStateUpdateSlot, OwnerText(o, now));
			}
		}
		if (!s_installed || !cfg.camera) {
			return;
		}

		if (cfg.cameraFlipOnDraw && a_snap.valid) {
			static bool wasDrawn = false;
			const auto  player = RE::PlayerCharacter::GetSingleton();
			const bool  drawn = player && (player->actorState2 & 2u) != 0;
			if (drawn && !wasDrawn) {
				Flip();
			}
			wasDrawn = drawn;
		}

		if (a_snap.time >= s_nextAllowed) {
			s_nextAllowed = a_snap.time + 0.05;
			unsigned long code{ 0 };
			if (!Guarded([&] { ComputeAllowed(a_snap); }, code)) {
				s_faulted = true;
				REX::ERROR("S4: exception 0x{:08X} while computing the collision limit; the camera offset is off for this session", code);
			}
		}

		if (s_siteInstalled.load() && !s_siteMissLogged && s_callsThird.load() > 180 && s_siteCalls.load() == 0) {
			s_siteMissLogged = true;
			REX::WARN("S4: the call site at Update + 0x{:X} was not hit once in {} third-person Update calls: the hooked site is not on the per-frame path", static_cast<std::uint32_t>(kThirdPersonPoseCallOffset),
				s_callsThird.load());
		}

		if (a_snap.time >= s_nextStats) {
			const double window = s_nextStats == 0.0 ? 1.0 : 5.0;
			s_nextStats = a_snap.time + 5.0;
			const auto calls = s_calls.load();
			const auto third = s_callsThird.load();
			if (window == 5.0) {
				std::string threads;
				COVER_DETAIL("S4 hook rate: {:.1f} calls/s on cameraStates[third], {:.1f} calls/s in all, {} on other objects, {} applied | site calls {} applied {} | target {:+.2f} applied {:+.3f} allowed {:.2f} | camera index {}",
					static_cast<double>(third - s_statsCallsThird) / window, static_cast<double>(calls - s_statsCalls) / window, s_callsOther.load(), s_siteApplied.load(), s_siteCalls.load(), s_siteApplied.load(),
					s_target.load(), s_applied.load(), s_allowed.load(), a_snap.cameraIndex);
			}
			s_statsCallsThird = third;
			s_statsCalls = calls;
		}
	}

	void SetOffsetTarget(const float a_metres) noexcept
	{
		s_target.store(a_metres);
	}

	float OffsetTarget() noexcept
	{
		return s_target.load();
	}

	void SetMode(const int a_mode) noexcept
	{
		s_mode.store(a_mode);
		COVER_DETAIL("S4 camera offset hook is now {} (0 off, 3 call-site hook, 4 pivot shift)", a_mode);
	}

	int Mode() noexcept
	{
		return s_mode.load() < 0 ? Config::Get().cameraMode : s_mode.load();
	}

	bool Compat() noexcept
	{
		return s_compat.load();
	}

	void Flip() noexcept
	{
		if (s_compat.load()) {
			return;  // compatibility mode: another mod places the camera
		}
		s_target.store(-s_target.load());
		COVER_DETAIL("S4 camera side flipped: target offset is now {:+.2f}", s_target.load());
		if (Config::Get().framing) {
			SwapShoulder();
		}
	}

	Stats GetStats() noexcept
	{
		Stats s;
		s.calls = s_calls.load();
		s.callsThird = s_callsThird.load();
		s.callsOther = s_callsOther.load();
		s.appliedCalls = s_siteApplied.load();
		s.lastThirdTime = s_lastThirdTime.load();
		s.applied = s_applied.load();
		s.allowed = s_allowed.load();
		s.installed = s_installed.load();
		s.compat = s_compat.load();
	s.chained = s_chained.load();
		s.faulted = s_faulted.load();
				s.siteInstalled = s_siteInstalled.load();
		s.siteCalls = s_siteCalls.load();
		s.siteApplied = s_siteApplied.load();
		s.pivotWrites = s_pivotWrites.load();
		s.pivotRestored = s_pivotRestored.load();
		s.pivotChanged = s_pivotChanged.load();
		s.pivotNull = s_pivotNull.load();
		return s;
	}

	void ProbeRequest(const std::string& a_label)
	{
		{
			std::scoped_lock lk(s_probeMutex);
			s_probeLabel = a_label;
			s_lastProbe = ProbeResult{};
		}
		s_probePending.store(true);
	}

	namespace
	{
		void FillNode(RigNode& a_out, void* a_state, const std::ptrdiff_t a_off)
		{
			const auto n = ReadNode(a_state, a_off);
			if (!n) {
				return;
			}
			a_out.present = true;
			a_out.ptr = n;
			const char* nm = n->name.c_str();
			a_out.name = nm ? nm : "(no name)";
			a_out.t = n->local.translate;
			const auto& r = n->local.rotate;
			a_out.rot[0] = r[0].x; a_out.rot[1] = r[0].y; a_out.rot[2] = r[0].z; a_out.rot[3] = r[1].x; a_out.rot[4] = r[1].y; a_out.rot[5] = r[1].z; a_out.rot[6] = r[2].x; a_out.rot[7] = r[2].y; a_out.rot[8] = r[2].z;
		}
	}

	RigState ReadRig()
	{
		RigState      out;
		unsigned long code{ 0 };
		(void)Guarded(
			[&] {
				const auto camera = RE::PlayerCamera::GetSingleton();
				const auto player = RE::PlayerCharacter::GetSingleton();
				if (!camera || !player) {
					return;
				}
				void* st = camera->cameraStates[RE::CameraStates::kThirdPerson];
				if (!st) {
					return;
				}
				FillNode(out.n1F8, st, kTpsBoomNode);
				FillNode(out.n200, st, kTpsPivotNode);
				FillNode(out.n208, st, kTpsFovNode);
				FillNode(out.n210, st, kTpsBoneNode);
				FillNode(out.n218, st, kTpsNode218);
				const auto* c = reinterpret_cast<const char*>(st);
				out.haveFlag = SafeRead(c + kTpsBoneFlag, out.flag291);
				(void)SafeRead(c + kTpsCollisionFactor, out.factor);
				(void)SafeRead(c + kTpsYaw, out.yaw);
				(void)SafeRead(c + kTpsBlend, out.blend);
				(void)SafeRead(c + kThirdPersonStatePosA, out.p190);
				(void)SafeRead(c + kThirdPersonStatePosB, out.p19c);
				if (camera->cameraRoot) {
					out.rootWorld = camera->cameraRoot->world.translate;
				}
				out.player = player->GetPosition();
				const float dx = out.rootWorld.x - out.player.x, dy = out.rootWorld.y - out.player.y, dz = out.rootWorld.z - out.player.z;
				out.rootToPlayerHoriz = std::sqrt(dx * dx + dy * dy);
				out.rootToPlayer = std::sqrt(dx * dx + dy * dy + dz * dz);
				out.ok = true;
			},
			code);
		return out;
	}

	std::string DescribeRig(const char* a_label, const RigState& a_s)
	{
		auto node = [](const char* a_what, const RigNode& a_n, const bool a_rot) {
			if (!a_n.present) {
				return std::format(" {} null;", a_what);
			}
			std::string out = std::format(" {} {} '{}' local t=({:+.4f},{:+.4f},{:+.4f})", a_what, a_n.ptr, a_n.name, a_n.t.x, a_n.t.y, a_n.t.z);
			if (a_rot) {
				out += std::format(" rot rows=({:+.3f},{:+.3f},{:+.3f}) ({:+.3f},{:+.3f},{:+.3f}) ({:+.3f},{:+.3f},{:+.3f})", a_n.rot[0], a_n.rot[1], a_n.rot[2], a_n.rot[3], a_n.rot[4], a_n.rot[5], a_n.rot[6], a_n.rot[7], a_n.rot[8]);
			}
			return out + ";";
		};
		return std::format("T5.0b rig [{}]: {}{}{}{}{} flag+0x291 {} | collision factor {:.4f} yaw {:+.4f} blend {:.4f} | +0x190 ({:+.3f},{:+.3f},{:+.3f}) +0x19C ({:+.3f},{:+.3f},{:+.3f}) | camera root world ({:+.3f},{:+.3f},{:+.3f}) player ({:+.3f},{:+.3f},{:+.3f}) | root to player {:.3f} m (horizontal {:.3f} m){}",
			a_label, a_s.ok ? "" : "UNREADABLE;", node("boom+1F8", a_s.n1F8, true), node("pivot+200", a_s.n200, false), node("fov+208", a_s.n208, false), node("bone+210", a_s.n210, false) + node("node+218", a_s.n218, false),
			a_s.haveFlag ? std::to_string(a_s.flag291) : std::string("unreadable"), a_s.factor, a_s.yaw, a_s.blend, a_s.p190.x, a_s.p190.y, a_s.p190.z, a_s.p19c.x, a_s.p19c.y, a_s.p19c.z, a_s.rootWorld.x, a_s.rootWorld.y, a_s.rootWorld.z, a_s.player.x, a_s.player.y, a_s.player.z,
			a_s.rootToPlayer, a_s.rootToPlayerHoriz, s_rigHold.load() ? std::format(" | hold ON ({} writes)", s_rigWrites.load()) : std::string(" | hold off"));
	}

	void BeginEntryBlend(const float a_seconds)
	{
		if (a_seconds <= 0.0f) {
			return;
		}
		s_enDur = a_seconds;
		s_enStart = Frame::Now();
		s_enHave = false;
		s_enOn = true;
		COVER_DETAIL("T5.2c: the entry blend of the camera position over {:.2f} s", a_seconds);
	}

	void ReleaseRigHold(const float a_seconds)
	{
		if (!s_rigHold.load()) {
			return;
		}
		if (a_seconds <= 0.0f) {
			s_rigHold = false;
			return;
		}
		s_relDur = a_seconds;
		s_relStart = Frame::Now();
		s_relOn = true;
		COVER_DETAIL("T5.2c: the rig hold is eased to the graph's values over {:.2f} s", a_seconds);
	}

	void SetCornerFollow(const bool a_on, const int a_sideDir) noexcept
	{
		s_cornerFollow.store(a_on);
		const int ov = a_on ? (a_sideDir < 0 ? -1 : (a_sideDir > 0 ? 1 : 0)) : 0;
		if (s_frOverride.exchange(ov) != ov && ov != 0) {
			COVER_DETAIL("M4B corner aim: the camera goes to the {} shoulder (the player's own choice returns after the aim)", ov > 0 ? "right" : "left");
		}
	}

	void SetCornerHold(const bool a_on, const float a_heading, const float a_cone) noexcept
	{
		if (a_on != s_chOn.load()) {
			COVER_DETAIL("M4b corner hold in the camera hook: {} (heading {:.1f} deg, cone {:.0f} deg; frames so far: {} writes, {} yaw clamps)", a_on ? "ON" : "off", a_heading * 57.29578f, a_cone * 57.29578f, s_chWrites.load(), s_chClamps.load());
		}
		s_chHeading.store(a_heading);
		s_chCone.store(a_cone);
		s_chOn.store(a_on);
	}

	void SetRigHold(const bool a_on, const RE::NiPoint3& a_boom, const RE::NiPoint3& a_pivot)
	{
		s_relOn = false;
		s_rigBoom[0] = a_boom.x; s_rigBoom[1] = a_boom.y; s_rigBoom[2] = a_boom.z;
		s_rigPivot[0] = a_pivot.x; s_rigPivot[1] = a_pivot.y; s_rigPivot[2] = a_pivot.z;
		s_rigHold = a_on;
		COVER_DETAIL("T5.0b: the rig hold is {} (boom ({:+.4f},{:+.4f},{:+.4f}), pivot ({:+.4f},{:+.4f},{:+.4f}); [Anim] bRigHold {})", a_on ? "ON" : "off", a_boom.x, a_boom.y, a_boom.z, a_pivot.x, a_pivot.y, a_pivot.z, Config::Get().rigHold);
	}

	void SetRigHoldValues(const RE::NiPoint3& a_boom, const RE::NiPoint3& a_pivot) noexcept
	{
		s_rigBoom[0] = a_boom.x; s_rigBoom[1] = a_boom.y; s_rigBoom[2] = a_boom.z;
		s_rigPivot[0] = a_pivot.x; s_rigPivot[1] = a_pivot.y; s_rigPivot[2] = a_pivot.z;
	}

	std::uint64_t RigHoldWrites() noexcept
	{
		return s_rigWrites.load();
	}

	FramingState GetFraming() noexcept
	{
		std::lock_guard lock(s_frMutex);
		auto            s = s_frState;
		s.on = Config::Get().framing && s_installed.load() && !s_compat.load() && !s_frFaulted.load();
		s.situation = s_frSituation.load();
		s.sideTarget = s_frSideDir.load();
		s.faulted = s_frFaulted.load();
		s.writes = s_frWrites.load();
		return s;
	}

	const char* SituationName(const int a_situation) noexcept
	{
		static constexpr const char* kNames[8] = { "exploring", "weapon drawn", "aiming", "sprinting", "low cover", "tall cover", "low cover aiming", "tall cover aiming" };
		return a_situation >= 0 && a_situation < 8 ? kNames[a_situation] : "?";
	}

	void SetShoulder(const int a_dir) noexcept
	{
		const int d = a_dir < 0 ? -1 : 1;
		if (s_frSideDir.exchange(d) != d) {
			COVER_DETAIL("M4A SHOULDER swap: the camera goes to the {} shoulder", d > 0 ? "right" : "left");
		}
	}

	void SwapShoulder() noexcept
	{
		if (s_frOverride.load() != 0) {
			COVER_DETAIL("M4A SHOULDER swap press ignored: the corner aim has the camera on the open side's shoulder");
			return;
		}
		SetShoulder(-s_frSideDir.load());
	}

	CornerStats GetCornerStats() noexcept
	{
		CornerStats s;
		s.frames = s_chWrites.load();
		s.corrections = s_chCorr.load();
		s.maxDevDeg = s_chMaxDev.load();
		s.lateFrames = s_chLate.load();
		s.lateMaxDeg = s_chLateMax.load();
		s.clamps = s_chClamps.load();
		return s;
	}

	void ResetCornerStats() noexcept
	{
		s_chWrites = 0;
		s_chCorr = 0;
		s_chLate = 0;
		s_chClamps = 0;
		s_chMaxDev = 0.0f;
		s_chLateMax = 0.0f;
	}

	void ForceSituation(const int a_situation) noexcept
	{
		s_frForce.store(a_situation);
	}

	void SetCoverKind(const int a_kind, const bool a_aimHeld) noexcept
	{
		s_frCoverKind.store(a_kind);
		s_frCoverAim.store(a_aimHeld);
	}

	void SetCoverCam(const float a_back, const float a_up) noexcept
	{
		s_ccBackTarget = a_back;
		s_ccUpTarget = a_up;
	}

	void CoverCamReset() noexcept
	{
		s_ccBackTarget = 0.0f;
		s_ccUpTarget = 0.0f;
		s_ccReset = true;
	}

	CoverCamState GetCoverCam() noexcept
	{
		CoverCamState s;
		s.backNow = s_ccBackNow.load();
		s.upNow = s_ccUpNow.load();
		s.backTarget = s_ccBackTarget.load();
		s.upTarget = s_ccUpTarget.load();
		s.writes = s_ccWrites.load();
		s.restored = s_ccRestored.load();
		s.leftAlone = s_ccLeftAlone.load();
		s.faulted = s_ccFaulted.load();
		return s;
	}

	CollisionState ReadCollision()
	{
		CollisionState out;
		unsigned long  code{ 0 };
		(void)Guarded(
			[&] {
				const auto camera = RE::PlayerCamera::GetSingleton();
				if (!camera) {
					return;
				}
				const auto st = camera->cameraStates[RE::CameraStates::kThirdPerson];
				if (!st) {
					return;
				}
				RE::NiPoint3 a{}, w{};
				if (!SafeRead(reinterpret_cast<const char*>(st) + kTpsCollisionFactor, out.factor) || !SafeRead(reinterpret_cast<const char*>(st) + kTpsCollisionTimer, out.timer) ||
					!SafeRead(reinterpret_cast<const char*>(st) + kThirdPersonStatePosA, a) || !SafeRead(reinterpret_cast<const char*>(st) + kThirdPersonStatePosB, w)) {
					return;
				}
				out.wantedToFinal = std::sqrt((w.x - a.x) * (w.x - a.x) + (w.y - a.y) * (w.y - a.y) + (w.z - a.z) * (w.z - a.z));
				out.ok = true;
			},
			code);
		return out;
	}

	bool ReadViewYaw(float& a_yaw)
	{
		bool          ok{ false };
		unsigned long code{ 0 };
		(void)Guarded(
			[&] {
				const auto camera = RE::PlayerCamera::GetSingleton();
				if (!camera) {
					return;
				}
				const auto st = camera->cameraStates[RE::CameraStates::kThirdPerson];
				float      yaw{ 0.0f };
				if (st && SafeRead(reinterpret_cast<const char*>(st) + kTpsYaw, yaw) && std::isfinite(yaw)) {
					a_yaw = yaw;
					ok = true;
				}
			},
			code);
		return ok;
	}

	ProbeResult LastProbe()
	{
		std::scoped_lock lk(s_probeMutex);
		return s_lastProbe;
	}

	std::optional<float> GraphBoomY()
	{
		if (!s_rigHold.load() || Frame::Now() - s_graphBoomAt.load() > 0.25) {
			return std::nullopt;
		}
		return s_graphBoomY.load();
	}

	void YawProbeBegin(const char* a_label)
	{
		{
			std::lock_guard lock(s_ypMutex);
			s_ypRows.clear();
			s_ypLabel = a_label;
			s_ypT0 = Frame::Now();
		}
		s_ypOn.store(true);
	}

	YawSummary YawProbeEnd()
	{
		s_ypOn.store(false);
		std::lock_guard lock(s_ypMutex);
		if (s_ypRows.empty()) {
			COVER_DETAIL("P3 YAW {}: no frames", s_ypLabel);
			return {};
		}
		const auto angDiff = [](const float a, const float b) {
			float d = std::fmod(a - b, 6.2831853f);
			if (d > 3.1415927f) {
				d -= 6.2831853f;
			}
			if (d < -3.1415927f) {
				d += 6.2831853f;
			}
			return d;
		};
		float maxSumStep = 0.0f, maxRootJump = 0.0f, minHead = 1e9f, maxSumDev = 0.0f, maxAngDev = 0.0f;
		for (std::size_t i = 0; i < s_ypRows.size(); ++i) {
			const auto& r = s_ypRows[i];
			minHead = (std::min)(minHead, r.dHead);
			maxSumDev = (std::max)(maxSumDev, std::fabs(angDiff(r.sum, s_ypRows.front().sum)));
			maxAngDev = (std::max)(maxAngDev, std::fabs(angDiff(r.angZ, s_ypRows.front().angZ)));
			if (i > 0) {
				const auto& q = s_ypRows[i - 1];
				maxSumStep = (std::max)(maxSumStep, std::fabs(angDiff(r.sum, q.sum)));
				maxRootJump = (std::max)(maxRootJump, std::sqrt((r.rx - q.rx) * (r.rx - q.rx) + (r.ry - q.ry) * (r.ry - q.ry) + (r.rz - q.rz) * (r.rz - q.rz)));
			}
		}
		YawSummary sum{ s_ypRows.size(), minHead, maxRootJump, minHead, maxRootJump };
		{
			std::size_t k0 = 0;
			while (k0 < s_ypRows.size() && s_ypRows[k0].pose == 0) {
				++k0;
			}
			if (k0 > 0 && k0 < s_ypRows.size()) {
				float jump = 0.0f, hmin = 1e9f, bodySpan = 0.0f, bodyMax = 0.0f;
				for (std::size_t i = k0; i < s_ypRows.size(); ++i) {
					const auto& r = s_ypRows[i];
					hmin = (std::min)(hmin, r.dHead);
					bodyMax = (std::max)(bodyMax, std::fabs(angDiff(r.angZ, s_ypRows[k0].angZ)));
					if (i > k0) {
						const auto& q = s_ypRows[i - 1];
						jump = (std::max)(jump, std::sqrt((r.rx - q.rx) * (r.rx - q.rx) + (r.ry - q.ry) * (r.ry - q.ry) + (r.rz - q.rz) * (r.rz - q.rz)));
						bodySpan = (std::max)(bodySpan, std::fabs(angDiff(r.angZ, q.angZ)));
					}
				}
				sum.poseMinHead = hmin;
				sum.poseMaxJump = jump;
				COVER_DETAIL("P3 YAW {} from the pose's first frame (t={:.3f}): the body's angle z strays {:.1f} deg from its value there (largest step {:.1f} deg between two frames), the camera root's largest jump between two frames {:.3f} m, its least distance from the head {:.2f} m", s_ypLabel, s_ypRows[k0].t, bodyMax * 57.29578f, bodySpan * 57.29578f, jump, hmin);
			}
		}
		COVER_DETAIL("P3 YAW {} summary: {} frames in {:.2f} s; the view yaw (angle z + offset) strays at most {:.1f} deg from its first value, the body's angle z {:.1f} deg, the largest step of the view yaw between two frames {:.1f} deg; the camera root's largest jump between two frames {:.3f} m; the camera root's least distance from the head {:.2f} m",
			s_ypLabel, s_ypRows.size(), s_ypRows.back().t, maxSumDev * 57.29578f, maxAngDev * 57.29578f, maxSumStep * 57.29578f, maxRootJump, minHead);
		for (const auto& r : s_ypRows) {
			COVER_DETAIL("P3 YAW {} t={:.3f} angZ={:+.4f} off={:+.4f} sum={:.4f} tpsYaw={:+.4f} root=({:+.2f},{:+.2f},{:+.2f}) dHead={:.2f} pose={} exiting={:g} idleLoc={:g}", s_ypLabel, r.t, r.angZ, r.off, r.sum, r.tpsYaw, r.rx, r.ry, r.rz, r.dHead, r.pose, r.exiting, r.idleLoc);
			COVER_DETAIL("P3 YAW {} t={:.3f} rig(graph values, before the hold writes) boom=({:+.3f},{:+.3f},{:+.3f}) pivot=({:+.3f},{:+.3f},{:+.3f}) factor={:.3f} p190=({:+.1f},{:+.1f},{:+.1f}) p19c=({:+.1f},{:+.1f},{:+.1f}) flag291={} bone={} n218={} blend={:.3f} v1cc=({:+.2f},{:+.2f},{:+.2f})", s_ypLabel, r.t, r.bx, r.by, r.bz, r.px, r.py, r.pz, r.factor, r.ax, r.ay, r.az, r.cx, r.cy, r.cz, r.flag291, r.bone, r.n218, r.blend, r.v1cc[0], r.v1cc[1], r.v1cc[2]);
			COVER_DETAIL("P3 YAW {} t={:.3f} after hold={} cc=({:.2f},{:.2f}) boomL=({:+.3f},{:+.3f},{:+.3f}) boomW=({:+.2f},{:+.2f},{:+.2f}) pivotW=({:+.2f},{:+.2f},{:+.2f}) rootW=({:+.2f},{:+.2f},{:+.2f})", s_ypLabel, r.t, r.hold, r.ccBack, r.ccUp, r.abl[0], r.abl[1], r.abl[2], r.abw[0], r.abw[1], r.abw[2], r.apw[0], r.apw[1], r.apw[2], r.arw[0], r.arw[1], r.arw[2]);
		}
		s_ypRows.clear();
		return sum;
	}

	void ProbeBegin(const char* a_label)
	{
		{
			std::lock_guard lock(s_hpMutex);
			s_hpLabel = a_label;
			s_prCount = 0;
			s_prSumD = 0.0;
		}
		s_hpOn.store(true);
	}

	void ProbeEnd()
	{
		s_hpOn.store(false);
		std::lock_guard lock(s_hpMutex);
		std::string     moved;
		int             nMoved = 0;
		for (int i = 0; i < kPrN && s_prCount > 0; ++i) {
			if (s_prMax[i] - s_prMin[i] > 1e-4f) {
				moved += std::format(" {}[{:+.4g}..{:+.4g}]", ProbeName(i), s_prMin[i], s_prMax[i]);
				++nMoved;
			}
		}
		COVER_DETAIL("P3 HOOKPROBE {}: {} frames at the call-site hook; {} of {} values moved:{}", s_hpLabel, s_prCount, nMoved, kPrN, moved.empty() ? " none" : moved);
		COVER_DETAIL("P3 HOOKPROBE {} pitch: the effective pitch (angle x + high+0x3F4) before the pose call {:+.4f} -> {:+.4f} rad (change {:+.4f}), after the call {:+.4f} -> {:+.4f}; the sum of g[+0x68]*s1*s2 over the frames {:+.4f} (the engine adds -sum to angle x: expected change {:+.4f})",
			s_hpLabel, s_prFirstEff, s_prLastEff, s_prLastEff - s_prFirstEff, s_prFirstEffAfter, s_prLastEffAfter, s_prSumD, -s_prSumD);
	}

	bool ReadTpsFloats(const std::ptrdiff_t a_offset, const int a_count, float* a_out)
	{
		bool          ok{ false };
		unsigned long code{ 0 };
		(void)Guarded(
			[&] {
				const auto camera = RE::PlayerCamera::GetSingleton();
				if (!camera) {
					return;
				}
				const void* st = camera->cameraStates[RE::CameraStates::kThirdPerson];
				if (!st) {
					return;
				}
				const auto* c = reinterpret_cast<const char*>(st);
				ok = true;
				for (int i = 0; i < a_count; ++i) {
					ok = SafeRead(c + a_offset + 4 * i, a_out[i]) && ok;
				}
			},
			code);
		return ok;
	}

	bool ReadRootRot(float (&a_out)[9])
	{
		bool          ok{ false };
		unsigned long code{ 0 };
		(void)Guarded(
			[&] {
				const auto camera = RE::PlayerCamera::GetSingleton();
				if (!camera || !camera->cameraRoot) {
					return;
				}
				const auto& r = camera->cameraRoot.get()->world.rotate;
				for (int i = 0; i < 3; ++i) {
					a_out[i * 3 + 0] = r.entry[i].x;
					a_out[i * 3 + 1] = r.entry[i].y;
					a_out[i * 3 + 2] = r.entry[i].z;
				}
				ok = true;
			},
			code);
		return ok;
	}

	Sample SampleRoot()
	{
		Sample out;
		unsigned long code{ 0 };
		(void)Guarded(
			[&] {
				const auto camera = RE::PlayerCamera::GetSingleton();
				const auto player = RE::PlayerCharacter::GetSingleton();
				if (!camera || !player || !camera->cameraRoot) {
					return;
				}
				const auto* root = camera->cameraRoot.get();
				out.rootWorld = root->world.translate;
				out.rootLocal = root->local.translate;
				out.hasParent = root->parent != nullptr;
				out.player = player->GetPosition();
				const float dx = out.player.x - out.rootWorld.x;
				const float dy = out.player.y - out.rootWorld.y;
				out.distance = std::sqrt(dx * dx + dy * dy);
				out.ok = true;
			},
			code);
		return out;
	}

	bool ForceFirstPerson()
	{
		unsigned long code{ 0 };
		const bool    ok = Guarded(
            [] {
                if (const auto camera = RE::PlayerCamera::GetSingleton()) {
                    camera->ForceFirstPerson();
                }
            },
            code);
		if (!ok) {
			REX::ERROR("S4: exception 0x{:08X} in PlayerCamera::ForceFirstPerson", code);
		}
		return ok;
	}

	bool ForceThirdPerson()
	{
		unsigned long code{ 0 };
		const bool    ok = Guarded(
            [] {
                if (const auto camera = RE::PlayerCamera::GetSingleton()) {
                    camera->ForceThirdPerson();
                }
            },
            code);
		if (!ok) {
			REX::ERROR("S4: exception 0x{:08X} in PlayerCamera::ForceThirdPerson", code);
		}
		return ok;
	}
}
