#include "Engine/LoadWatch.h"

#include <excpt.h>

namespace Engine::LoadWatch
{
	namespace
	{
		using Clock = std::chrono::steady_clock;

		constexpr unsigned long kCppExceptionCode = 0xE06D7363;  // MSVC `throw`: not a fault, let it propagate
		constexpr std::uint32_t kMaxReports = 40;                // keeps the log short while walking across exterior cells
		constexpr auto          kSettleTime = std::chrono::milliseconds(1500);

		// Main thread only.
		bool              s_disabled{ false };
		const void*       s_lastCell{ nullptr };
		bool              s_settlePending{ false };
		Clock::time_point s_changedAt{};
		std::uint32_t     s_reports{ 0 };

		[[nodiscard]] bool CanReport()
		{
			if (s_reports < kMaxReports) {
				++s_reports;
				return true;
			}

			if (s_reports == kMaxReports) {
				++s_reports;
				COVER_DETAIL("LoadWatch: reached {} log lines; it keeps running but stays silent", kMaxReports);
			}
			return false;
		}

		void LogPosition(const std::string_view a_label, const RE::PlayerCharacter& a_player, const void* a_cell)
		{
			// Raw engine values. Units and heading convention are confirmed in Phase 1 (S1).
			const auto pos = a_player.GetPosition();
			COVER_DETAIL("{}: parentCell={} position=({:.2f}, {:.2f}, {:.2f})", a_label, a_cell, pos.x, pos.y, pos.z);
		}

		void TickImpl()
		{
			const auto player = RE::PlayerCharacter::GetSingleton();
			if (!player) {
				return;
			}

			const void* cell = player->parentCell;  // pointer value only, never dereferenced
			const auto  now = Clock::now();

			if (cell != s_lastCell) {
				const bool hadCell = s_lastCell != nullptr;
				s_lastCell = cell;
				s_changedAt = now;
				s_settlePending = cell != nullptr;

				if (CanReport()) {
					if (cell) {
						LogPosition(hadCell ? "Player cell changed" : "Player entered a cell (game loaded or started)", *player, cell);
					} else {
						COVER_DETAIL("Player cell cleared (no game loaded: main menu or loading screen)");
					}
				}
			} else if (s_settlePending && now - s_changedAt >= kSettleTime) {
				s_settlePending = false;
				if (CanReport()) {
					LogPosition("Player position after settling", *player, cell);
				}
			}
		}

		// Structured exception handling needs a function with no C++ objects that require unwinding, so this
		// wrapper does nothing else. C++ exceptions are not faults here and keep propagating.
		bool CallGuarded(void (*a_fn)(), unsigned long& a_code)
		{
			__try {
				a_fn();
				return true;
			} __except (a_code = GetExceptionCode(), a_code == kCppExceptionCode ? EXCEPTION_CONTINUE_SEARCH : EXCEPTION_EXECUTE_HANDLER) {
				return false;
			}
		}
	}

	void Tick()
	{
		if (s_disabled) {
			return;
		}

		unsigned long code{ 0 };
		if (!CallGuarded(&TickImpl, code)) {
			s_disabled = true;
			REX::ERROR("LoadWatch: exception 0x{:08X} while reading the player; watcher disabled for this session", code);
		}
	}
}
