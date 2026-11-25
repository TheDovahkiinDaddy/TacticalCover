#include "Engine/PlayerControl.h"

#include "Config.h"
#include "Engine/Frame.h"
#include "Engine/Guard.h"

namespace Engine::PlayerControl
{
	namespace
	{
		constexpr std::array<const char*, 15> kWatched{
			"iSyncActorStance", "iSyncRelaxedReadyAlert", "iSyncWeaponDrawState", "iSyncGunDown", "iSyncSighted", "iIsSighted", "iSyncSprintState",
			"iSyncJumpState", "iSyncSwimState", "iSyncGravity", "iSyncBleedout", "bIsMantling", "bIsDialogueOpen", "bIsSneaking", "iSyncRelaxReadySneak"
		};
	}

	std::optional<double> ReadVar(const char* a_name)
	{
		const auto player = RE::PlayerCharacter::GetSingleton();
		if (!player) {
			return std::nullopt;
		}
		const RE::BSFixedString key{ a_name };
		std::int32_t            i{ 0 };
		bool                    b{ false };
		float                   f{ 0.0f };
		std::optional<double>   out;
		unsigned long           code{ 0 };
		(void)Guarded(
			[&] {
				if (player->GetGraphVariableImplInt(key, i)) {
					out = static_cast<double>(i);
				} else if (player->GetGraphVariableImplBool(key, b)) {
					out = b ? 1.0 : 0.0;
				} else if (player->GetGraphVariableImplFloat(key, f)) {
					out = static_cast<double>(f);
				}
			},
			code);
		return out;
	}

	std::string DescribeVars(const std::span<const char* const> a_names)
	{
		std::string have;
		std::string missing;
		for (const char* name : a_names) {
			if (const auto v = ReadVar(name)) {
				have += std::format(" {}={:g}", name, *v);
			} else {
				missing += std::format(" {}", name);
			}
		}
		return std::format("{}{}", have.empty() ? " (none resolved)" : have, missing.empty() ? "" : std::format(" | unresolved:{}", missing));
	}

	std::span<const char* const> WatchedVars()
	{
		return kWatched;
	}

	std::string ActorFlags()
	{
		const auto player = RE::PlayerCharacter::GetSingleton();
		return player ? std::format("aState=0x{:08X}/0x{:08X}", player->actorState1, player->actorState2) : std::string("aState=?");
	}

	std::vector<std::string> NotAllowed(const Frame::Snapshot& a_snap)
	{
		std::vector<std::string> why;
		// Camera state objects: first person, and every state that means the player is not walking about in third person.
		switch (a_snap.cameraIndex) {
		case RE::CameraStates::kThirdPerson:
			break;
		case RE::CameraStates::kFirstPerson:
			why.emplace_back("first person");
			break;
		case RE::CameraStates::kDialogue:
			why.emplace_back("dialogue");
			break;
		case RE::CameraStates::kFurniture:
			why.emplace_back("furniture/seated");
			break;
		case RE::CameraStates::kVehicle:
		case RE::CameraStates::kFlight:
		case RE::CameraStates::kShipFarTravel:
		case RE::CameraStates::kShipAction:
		case RE::CameraStates::kShipTargeting:
		case RE::CameraStates::kShipCombatOrbit:
			why.emplace_back("piloting/driving");
			break;
		case RE::CameraStates::kBleedout:
			why.emplace_back("bleedout");
			break;
		case RE::CameraStates::kIronSights:
			why.emplace_back("ironsights camera");
			break;
		default:
			why.emplace_back(std::format("camera state {}", a_snap.cameraIndex));
			break;
		}
		if (a_snap.paused) {
			why.emplace_back("menu/paused");
		}
		const auto jump = ReadVar("iSyncJumpState");
		if (jump && *jump != 0.0) {
			why.emplace_back(std::format("airborne (iSyncJumpState={:g})", *jump));
		}
		if (const auto swim = ReadVar("iSyncSwimState"); swim && *swim != 0.0) {
			why.emplace_back(std::format("swimming (iSyncSwimState={:g})", *swim));
		}
		if (const auto mantle = ReadVar("bIsMantling"); mantle && *mantle != 0.0) {
			why.emplace_back("mantling");
		}
		if (const auto bleed = ReadVar("iSyncBleedout"); bleed && *bleed != 0.0) {
			why.emplace_back("bleedout (graph)");
		}
		if (const auto talk = ReadVar("bIsDialogueOpen"); talk && *talk != 0.0) {
			why.emplace_back("dialogue (graph)");
		}
		return why;
	}

	std::string DetectableConditions()
	{
		std::string out;
		const auto  add = [&](const char* label, const char* source, const bool ok) { out += std::format("{}{}={}", out.empty() ? "" : "; ", label, ok ? source : "NO READING"); };
		add("first person", "camera state", true);
		add("dialogue", "camera state / bIsDialogueOpen", true);
		add("seated", "camera state kFurniture", true);
		add("piloting/driving", "camera states vehicle+ship", true);
		add("menu/paused", "Main::isGameMenuPaused", true);
		add("airborne", "iSyncJumpState", ReadVar("iSyncJumpState").has_value());
		add("swimming", "iSyncSwimState", ReadVar("iSyncSwimState").has_value());
		add("mantling", "bIsMantling", ReadVar("bIsMantling").has_value());
		add("zero gravity", "iSyncGravity (value of the normal state unknown)", ReadVar("iSyncGravity").has_value());
		add("boosting", "(none found)", false);
		add("controls disabled by the game", "(none found)", false);
		return out;
	}

	// ---- eased SetPosition ------------------------------------------------------------------------------------

	namespace
	{
		float Horizontal(const RE::NiPoint3& a, const RE::NiPoint3& b)
		{
			return std::sqrt((a.x - b.x) * (a.x - b.x) + (a.y - b.y) * (a.y - b.y));
		}
		float Smooth(const float t)
		{
			const float c = std::clamp(t, 0.0f, 1.0f);
			return c * c * (3.0f - 2.0f * c);
		}
		constexpr double kStepSeconds = 1.0 / 60.0;
		constexpr double kHoldSeconds = 0.2;   // keep writing the final target
		constexpr double kWatchSeconds = 0.6;  // then watch whether the game moves the player away
	}

	void EasedMove::Begin(const RE::NiPoint3& a_delta, const float a_seconds, const bool a_updateController)
	{
		m_result = {};
		m_started = false;
		m_phase = Phase::Moving;
		m_seconds = a_seconds;
		m_ctl = a_updateController;
		m_errSum = 0.0f;
		const auto player = RE::PlayerCharacter::GetSingleton();
		if (!player) {
			m_phase = Phase::Done;
			m_result.done = true;
			m_result.faulted = true;
			return;
		}
		m_from = player->GetPosition();
		m_to = m_from + a_delta;
		m_lastTarget = m_from;
	}

	bool EasedMove::Step(const double a_now)
	{
		if (m_phase == Phase::Idle || m_phase == Phase::Done) {
			return true;
		}
		if (!m_started) {
			m_started = true;
			m_start = a_now;
			m_nextStep = a_now;
			m_phaseStart = a_now;
			m_result.zMin = m_result.zMax = 0.0f;
		}
		if (a_now < m_nextStep) {
			return false;
		}
		m_nextStep = a_now + kStepSeconds;

		const auto player = RE::PlayerCharacter::GetSingleton();
		if (!player) {
			m_phase = Phase::Done;
			m_result.done = m_result.faulted = true;
			return true;
		}
		const auto actual = player->GetPosition();
		const float dz = actual.z - m_from.z;
		m_result.zMin = (std::min)(m_result.zMin, dz);
		m_result.zMax = (std::max)(m_result.zMax, dz);

		switch (m_phase) {
		case Phase::Moving:
		case Phase::Hold: {
			if (m_result.steps > 0) {
				// where the game left the player compared with where we put them one step earlier
				const float err = Horizontal(actual, m_lastTarget);
				m_result.maxErr = (std::max)(m_result.maxErr, err);
				m_errSum += err;
			}
			const double t = a_now - m_start;
			const float  u = Smooth(static_cast<float>(t / m_seconds));
			const RE::NiPoint3 target{ m_from.x + (m_to.x - m_from.x) * u, m_from.y + (m_to.y - m_from.y) * u, m_from.z };
			unsigned long code{ 0 };
			const bool ok = Guarded([&] { player->SetPosition(target, m_ctl); }, code);
			if (!ok) {
				REX::ERROR("S5: exception 0x{:08X} in Actor::SetPosition; the eased move stops", code);
				m_phase = Phase::Done;
				m_result.done = m_result.faulted = true;
				return true;
			}
			m_lastTarget = target;
			++m_result.steps;
			if (m_phase == Phase::Moving && t >= m_seconds) {
				m_phase = Phase::Hold;
				m_phaseStart = a_now;
			} else if (m_phase == Phase::Hold && a_now - m_phaseStart >= kHoldSeconds) {
				m_phase = Phase::Watch;
				m_phaseStart = a_now;
				m_result.meanErr = m_result.steps > 1 ? m_errSum / static_cast<float>(m_result.steps - 1) : 0.0f;
			}
			return false;
		}
		case Phase::Watch:
			if (m_result.endErr == 0.0f) {
				m_result.endErr = Horizontal(actual, m_to);
			}
			if (a_now - m_phaseStart >= kWatchSeconds) {
				m_result.driftAfter = Horizontal(actual, m_to);
				m_result.zAfter = dz;
				m_result.done = true;
				m_phase = Phase::Done;
				return true;
			}
			return false;
		default:
			return true;
		}
	}

	std::string EasedMove::Describe() const
	{
		const auto& r = m_result;
		return std::format("ctl={} steps={} step error max {:.3f} m mean {:.3f} m | height range {:+.3f}..{:+.3f} m | at the target after the last write {:.3f} m, 0.6 s after we stopped {:.3f} m (height {:+.3f}){}",
			m_ctl ? "true" : "false", r.steps, r.maxErr, r.meanErr, r.zMin, r.zMax, r.endErr, r.driftAfter, r.zAfter, r.faulted ? " FAULT" : "");
	}
}
