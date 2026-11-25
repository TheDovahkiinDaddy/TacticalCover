#include "Engine/AudioProbe.h"
#include "Engine/Frame.h"

#include "Config.h"
#include "Engine/Camera.h"
#include "Engine/Cover.h"
#include "Engine/EquipPending.h"
#include "Engine/CoverSense.h"
#include "Engine/Guard.h"
#include "Engine/Input.h"
#include "Engine/Offsets.h"
#include "Engine/RewriteTest.h"
#include "Engine/Rig.h"
#include "Engine/WorldProbe.h"

#include "REX/W32/KERNEL32.h"

namespace Engine::Frame
{
	namespace
	{
		using Clock = std::chrono::steady_clock;

		constexpr float kPi = 3.14159265358979f;

		// Main thread / task thread only.
		bool              s_disabled{ false };
		bool              s_started{ false };
		Clock::time_point s_start{};
		Clock::time_point s_lastTick{};
		Snapshot          s_snap{};

		// One-second window for the log line.
		Clock::time_point         s_windowStart{};
		std::uint32_t             s_windowTicks{ 0 };
		double                    s_windowDtSum{ 0.0 };
		float                     s_windowDtMax{ 0.0f };
		std::vector<std::uint32_t> s_windowThreads;
		bool                      s_haveWindowPos{ false };
		RE::NiPoint3              s_windowPos{};

		// Change tracking for the state-change lines.
		bool          s_cameraLayoutLogged{ false };
		bool          s_wasPaused{ false };
		std::string   s_lastStateKey;
		std::uint32_t s_stateLines{ 0 };
		Clock::time_point s_lastGraphRead{};
		std::string   s_graphVars;

		constexpr std::uint32_t kMaxStateLines = 300;

		[[nodiscard]] std::uintptr_t ModuleBase()
		{
			return REX::FModule::GetExecutingModule().GetBaseAddress();
		}

		// RVA of an object's vtable pointer, or 0 when it cannot be read.
		[[nodiscard]] std::uintptr_t VtableRva(const void* a_object)
		{
			std::uintptr_t vptr{ 0 };
			if (!a_object || !SafeRead(a_object, vptr) || vptr < ModuleBase()) {
				return 0;
			}
			return vptr - ModuleBase();
		}

		[[nodiscard]] std::string_view CameraName(const int a_index) noexcept
		{
			switch (a_index) {
			case RE::CameraStates::kFirstPerson: return "first";
			case RE::CameraStates::kIronSights: return "ironsights";
			case RE::CameraStates::kTween: return "tween";
			case RE::CameraStates::kVehicle: return "vehicle";
			case RE::CameraStates::kFreeWalk: return "freewalk";
			case RE::CameraStates::kDialogue: return "dialogue";
			case RE::CameraStates::kPhotoMode: return "photo";
			case RE::CameraStates::kThirdPerson: return "third";
			case RE::CameraStates::kFurniture: return "furniture";
			case RE::CameraStates::kBleedout: return "bleedout";
			default: return a_index < 0 ? "unknown" : "other";
			}
		}

		// Once, when the camera first exists: proves the cameraStates layout taken from CommonLibSF. Exactly one
		// entry should equal currentState, and the vtable RVAs let a later spike tell the states apart.
		void LogCameraLayout(const RE::PlayerCamera& a_camera)
		{
			s_cameraLayoutLogged = true;
			int matches{ 0 };
			for (std::uint32_t i = 0; i < RE::CameraStates::kTotal; ++i) {
				const bool current = a_camera.cameraStates[i] && a_camera.cameraStates[i] == a_camera.currentState;
				matches += current ? 1 : 0;
				COVER_DETAIL("S1 camera state [{:2}] {:<10} ptr={} vtableRVA=0x{:X}{}", i, CameraName(static_cast<int>(i)),
					a_camera.cameraStates[i], VtableRva(a_camera.cameraStates[i]), current ? "  <== currentState" : "");
			}
			COVER_DETAIL("S1 camera layout: PlayerCamera={} currentState={} matches={} (1 means the cameraStates layout from CommonLibSF holds on this runtime)",
				static_cast<const void*>(&a_camera), a_camera.currentState, matches);
		}

		[[nodiscard]] int CameraIndex(const RE::PlayerCamera& a_camera)
		{
			if (!a_camera.currentState) {
				return -1;
			}
			for (std::uint32_t i = 0; i < RE::CameraStates::kTotal; ++i) {
				if (a_camera.cameraStates[i] == a_camera.currentState) {
					return static_cast<int>(i);
				}
			}
			return -1;
		}

		// Reads a few animation-graph variables of the player through the IAnimationGraphManagerHolder virtuals.
		// Names are the graph variables listed in docs/REFERENCE.md section 3.4 and found in Starfield.exe. A
		// variable that the player's graph does not have simply does not resolve. Values are logged raw: what
		// they mean is S5's question.
		[[nodiscard]] std::string ReadGraphVars(RE::PlayerCharacter& a_player)
		{
			static constexpr std::array names{
				"iSyncWeaponDrawState", "iSyncSighted", "iIsSighted", "iSyncSprintState", "iSyncJumpState",
				"iSyncGunDown", "iIsGunDown", "iSyncGravity", "iSyncActorStance", "iIsInSneak", "bIsSneaking",
				"iSyncRelaxedReadyAlert", "iSyncRelaxReadySneak", "iSyncIdleLocomotion", "bIsInCover", "iIsPlayer"
			};

			std::string out;
			for (const char* name : names) {
				const RE::BSFixedString key{ name };
				std::int32_t            i{ 0 };
				bool                    b{ false };
				float                   f{ 0.0f };
				if (a_player.GetGraphVariableImplInt(key, i)) {
					out += std::format(" {}=i{}", name, i);
				} else if (a_player.GetGraphVariableImplBool(key, b)) {
					out += std::format(" {}=b{}", name, b ? 1 : 0);
				} else if (a_player.GetGraphVariableImplFloat(key, f)) {
					out += std::format(" {}=f{:.2f}", name, f);
				}
			}
			return out.empty() ? " (none resolved)" : out;
		}

		void TickImpl()
		{
			const auto& cfg = Config::Get();
			const auto  now = Clock::now();
			if (!s_started) {
				s_started = true;
				s_start = now;
				s_lastTick = now;
				s_windowStart = now;
				COVER_DETAIL("S1 frame tick started on thread {}", REX::W32::GetCurrentThreadId());
			}

			const float dt = std::chrono::duration<float>(now - s_lastTick).count();
			s_lastTick = now;

			Snapshot snap{};
			snap.time = std::chrono::duration<double>(now - s_start).count();
			snap.dt = dt;

			const auto player = RE::PlayerCharacter::GetSingleton();
			const auto camera = RE::PlayerCamera::GetSingleton();
			const auto main = RE::Main::GetSingleton();
			const auto ui = RE::UI::GetSingleton();

			snap.paused = main && main->isGameMenuPaused;
			if (snap.paused != s_wasPaused) {
				s_wasPaused = snap.paused;
				COVER_DETAIL("S1 game {} (Main::isGameMenuPaused={}, UI::menusVisible={})", snap.paused ? "paused" : "resumed",
					snap.paused, ui && ui->IsMenusVisible());
			}

			if (camera && !s_cameraLayoutLogged) {
				LogCameraLayout(*camera);
			}
			if (camera) {
				snap.cameraIndex = CameraIndex(*camera);
			}

			if (player && player->parentCell) {
				snap.valid = true;
				snap.position = player->GetPosition();
				snap.angle = player->GetAngle();
			}

			s_snap = snap;

			// Window bookkeeping runs even while paused, so tasks/s shows whether tasks keep running in menus.
			++s_windowTicks;
			s_windowDtSum += dt;
			s_windowDtMax = (std::max)(s_windowDtMax, dt);
			const auto tid = REX::W32::GetCurrentThreadId();
			if (std::ranges::find(s_windowThreads, tid) == s_windowThreads.end()) {
				s_windowThreads.push_back(tid);
			}

			if (cfg.frameTick && snap.valid && !snap.paused) {
				// Graph variables at about 10 Hz; log a line when the player's state key changes.
				if (now - s_lastGraphRead >= std::chrono::milliseconds(100)) {
					s_lastGraphRead = now;
					s_graphVars = ReadGraphVars(*player);

					const auto key = std::format("aState=0x{:08X}/0x{:08X} cam={}{}", player->actorState1, player->actorState2,
						CameraName(snap.cameraIndex), s_graphVars);
					if (key != s_lastStateKey) {
						if (!s_lastStateKey.empty() && s_stateLines < kMaxStateLines) {
							++s_stateLines;
							COVER_DETAIL("S1 state change at t={:.2f}s: {}", snap.time, key);
						}
						s_lastStateKey = key;
					}
				}
			}

			const auto windowMs = std::chrono::duration_cast<std::chrono::milliseconds>(now - s_windowStart).count();
			if (windowMs >= static_cast<long long>(cfg.logIntervalMs)) {
				const double secs = static_cast<double>(windowMs) / 1000.0;
				std::string  threads;
				for (const auto t : s_windowThreads) {
					threads += (threads.empty() ? "" : ",") + std::to_string(t);
				}

				if (cfg.frameTick) {
					if (!snap.valid) {
						COVER_DETAIL("S1 t={:.1f}s no player cell (menu or loading) | tasks={:.0f}/s avgDt={:.1f}ms maxDt={:.1f}ms thr=[{}] paused={}",
							snap.time, s_windowTicks / secs, 1000.0 * s_windowDtSum / s_windowTicks, 1000.0 * s_windowDtMax, threads, snap.paused);
						s_haveWindowPos = false;
					} else {
						float speed{ 0.0f };
						float dz{ 0.0f };
						float moveYawDeg{ 0.0f };
						bool  moved{ false };
						if (s_haveWindowPos) {
							const float dx = snap.position.x - s_windowPos.x;
							const float dy = snap.position.y - s_windowPos.y;
							dz = snap.position.z - s_windowPos.z;
							const float horiz = std::sqrt(dx * dx + dy * dy);
							speed = horiz / static_cast<float>(secs);
							moved = horiz > 0.05f;
							moveYawDeg = std::atan2(dx, dy) * 180.0f / kPi;  // atan2(dx, dy): 0 along +y, positive toward +x
						}
						s_windowPos = snap.position;
						s_haveWindowPos = true;

						COVER_DETAIL("S1 t={:.1f}s tasks={:.0f}/s avgDt={:.1f}ms maxDt={:.1f}ms thr=[{}] | pos=({:.2f},{:.2f},{:.2f}) ang=({:.3f},{:.3f},{:.3f}rad={:.1f}deg) | speed={:.2f}u/s dz={:+.2f} moveDir={}deg | cam={}({}) paused={} menus={} | aState=0x{:08X}/0x{:08X} |{} | in: {}",
							snap.time, s_windowTicks / secs, 1000.0 * s_windowDtSum / s_windowTicks, 1000.0 * s_windowDtMax, threads,
							snap.position.x, snap.position.y, snap.position.z,
							snap.angle.x, snap.angle.y, snap.angle.z, snap.angle.z * 180.0f / kPi,
							speed, dz, moved ? std::format("{:.1f}", moveYawDeg) : std::string("-"),
							CameraName(snap.cameraIndex), snap.cameraIndex, snap.paused, ui && ui->IsMenusVisible(),
							player->actorState1, player->actorState2, s_graphVars, Input::StatusLine());
					}
				}

				s_windowStart = now;
				s_windowTicks = 0;
				s_windowDtSum = 0.0;
				s_windowDtMax = 0.0f;
				s_windowThreads.clear();
			}
		}
	}

	void Tick()
	{
		const auto& cfg = Config::Get();
		if (!cfg.enabled || s_disabled) {
			return;
		}

		unsigned long code{ 0 };
		if (cfg.frameTick || cfg.inputHook || cfg.worldProbe) {
			if (!Guarded([] { TickImpl(); }, code)) {
				s_disabled = true;
				REX::ERROR("S1: exception 0x{:08X} while reading the player or camera; frame tick disabled for this session", code);
				return;
			}
		}

		// The other spikes run after S1 has produced the snapshot, each behind its own guard and switch.
		if (Input::ConsumeReloadRequest()) {
			Config::Reload();
		}
		Input::OnTick(s_snap);
		WorldProbe::OnTick(s_snap);
		CoverSense::OnTick(s_snap);
		Cover::OnTick(s_snap);
		Camera::OnTick(s_snap);
		RewriteTest::OnTick(s_snap);
		Rig::OnTick(s_snap);
		AudioProbe::Tick();
		AudioProbe::OnTick(s_snap.time);
		EquipPending::OnTick(Now());  // W7: unthrottled (iEquipFieldProbe, iPoseEquipPending: 0 = nothing)
	}

	const Snapshot& Latest() noexcept
	{
		return s_snap;
	}

	double Now() noexcept
	{
		return s_started ? std::chrono::duration<double>(Clock::now() - s_start).count() : 0.0;
	}
}
