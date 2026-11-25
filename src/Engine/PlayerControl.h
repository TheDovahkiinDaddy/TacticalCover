#pragma once

// S5: player control measurements. Reads of the player's state through the animation-graph variables and the
// actor flags, the "is cover allowed right now" conditions of PLAN.md section 3.2, and an eased SetPosition test.
// Everything is guarded and behind [PlayerControl] bPlayerControl. The test sequence itself lives in Engine/G2Script.

namespace Engine::Frame
{
	struct Snapshot;
}

namespace Engine::PlayerControl
{
	// One animation-graph variable of the player, tried as int, then bool, then float (the order S1 used).
	[[nodiscard]] std::optional<double> ReadVar(const char* a_name);

	// "name=value" for each of a_names that resolves, space separated; names that do not resolve are listed after "unresolved:".
	[[nodiscard]] std::string DescribeVars(std::span<const char* const> a_names);

	// The variables S5 watches: stance, weapon, aiming, jumping, swimming, gravity, mantling, bleedout, dialogue.
	[[nodiscard]] std::span<const char* const> WatchedVars();

	// Raw actor flags, for the log.
	[[nodiscard]] std::string ActorFlags();

	// PLAN.md section 3.2 "cover is not allowed": returns the reasons that apply now (empty = allowed), each named,
	// and notes in a_undetectable which conditions have no reading yet.
	[[nodiscard]] std::vector<std::string> NotAllowed(const Frame::Snapshot& a_snap);
	[[nodiscard]] std::string               DetectableConditions();

	// Eased move of the player along the ground with Actor::SetPosition at a fixed 60 Hz timer (the SFSE task runs
	// 600 to 800 times a second, so the steps are gated by the steady clock, rule 5).
	class EasedMove
	{
	public:
		struct Result
		{
			bool  done{ false };
			bool  faulted{ false };
			int   steps{ 0 };
			float maxErr{ 0.0f };      // largest horizontal distance between where we put the player and where the game had it one step later
			float meanErr{ 0.0f };
			float zMin{ 0.0f };        // vertical range relative to the start height over the whole test
			float zMax{ 0.0f };
			float endErr{ 0.0f };      // horizontal distance from the target when the last write was done and the game had one more step
			float driftAfter{ 0.0f };  // horizontal distance from the target 0.6 s after we stopped writing
			float zAfter{ 0.0f };      // height change after we stopped writing (relative to start)
		};

		// Starts a move from the player's current position by a_delta (metres, horizontal) over a_seconds.
		void Begin(const RE::NiPoint3& a_delta, float a_seconds, bool a_updateController);
		// Call from every tick; returns true when finished (also true after a fault, see Result::faulted).
		bool Step(double a_now);
		[[nodiscard]] const Result& GetResult() const noexcept { return m_result; }
		[[nodiscard]] std::string Describe() const;

	private:
		enum class Phase
		{
			Idle,
			Moving,
			Hold,
			Watch,
			Done,
		};
		Phase        m_phase{ Phase::Idle };
		RE::NiPoint3 m_from{};
		RE::NiPoint3 m_to{};
		RE::NiPoint3 m_lastTarget{};
		float        m_seconds{ 0.4f };
		bool         m_ctl{ true };
		double       m_start{ 0.0 };
		double       m_nextStep{ 0.0 };
		double       m_phaseStart{ 0.0 };
		bool         m_started{ false };
		float        m_errSum{ 0.0f };
		Result       m_result{};
	};
}
