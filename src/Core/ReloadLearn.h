#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

// The reload sound's learning (REFERENCE 23.13, 23.14, 23.18), engine-free.
//
// Every ordinary reload of a weapon is one "learn": the events the engine posted for it (id and seconds after the reload began) and how long the
// reload ran. A weapon's replay list is not the last learn: it is what recurs in the last learns. The rule:
//  - two events are the same event when the id is equal and the times are within 0.35 s (chained);
//  - an event is PRESENT in a learn that holds it; it is ABSENT from a learn that does not, when that learn lasted past the event's time (a reload that was cut at
//    2.5 s says nothing about an event at 3.8 s: it is neither present nor absent there);
//  - an event stays in the list while it is present in more learns than it is absent from. One learn alone therefore gives its whole list; a second learn that
//    lacks an event (a stray) takes it out; a cut learn that lacks the tail does not;
//  - the plain length is the median of the lengths of the learns that lasted at least 92 % of the longest: a cut reload never sets it.
namespace Core
{
	struct ReloadEvent
	{
		std::uint32_t id{ 0 };
		double        dt{ 0.0 };  // seconds after the reload began
	};

	struct ReloadLearn
	{
		double                   dur{ 0.0 };  // how long the reload ran
		std::vector<ReloadEvent> ev;
	};

	struct ReloadSequence
	{
		double                   plainDur{ 0.0 };
		std::vector<ReloadEvent> events;  // by time, then id
	};

	inline constexpr std::size_t kReloadLearnWindow = 8;      // the last learns that count
	inline constexpr double      kReloadMatchWindow = 0.35;   // s: the same id within this is the same event
	inline constexpr double      kReloadObserveMargin = 0.05; // s: a learn observes an event that is this far inside its length
	inline constexpr double      kReloadCompleteShare = 0.92; // a learn at least this share of the longest is a whole reload

	// Appends a learn and keeps the last kReloadLearnWindow.
	void AddReloadLearn(std::vector<ReloadLearn>& a_history, ReloadLearn a_learn);

	// The replay list and the plain length of a history (empty history: empty list, length 0).
	[[nodiscard]] ReloadSequence ResolveReload(const std::vector<ReloadLearn>& a_history);
}
