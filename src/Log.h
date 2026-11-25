#pragma once

#include <atomic>

// The two levels of the plugin's log (release 1.0.0, REFERENCE 24). bFullLog 1 (the development default and the user's own copy) writes every line as
// before; bFullLog 0 (the public release) keeps the load and version lines, the hook decisions, faults and warnings, and one line per cover entry and exit.
// A line of detail is written with COVER_DETAIL(...) in place of REX::INFO(...): the same text at the same level, and with the quiet log its arguments are
// not even formatted.

namespace Log
{
	inline std::atomic<bool> g_full{ true };

	[[nodiscard]] inline bool Full() noexcept { return g_full.load(std::memory_order_relaxed); }
	inline void SetFull(const bool a_full) noexcept { g_full.store(a_full, std::memory_order_relaxed); }
}

#define COVER_DETAIL(...)                	do {                                 		if (::Log::Full()) {             			REX::INFO(__VA_ARGS__);      		}                                	} while (false)
