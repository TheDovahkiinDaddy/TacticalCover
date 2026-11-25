#pragma once

#include <excpt.h>

// Structured-exception guards for calls into untyped engine code (PLAN.md rule 3).
//
// A function that contains __try may not also own C++ objects that need unwinding, so the guarded body is
// passed in as a callable and the guard itself holds nothing. C++ exceptions are not faults here and keep
// propagating (0xE06D7363 is what MSVC raises for `throw`).

namespace Engine
{
	inline constexpr unsigned long kCppExceptionCode = 0xE06D7363;

	[[nodiscard]] inline int FaultFilter(const unsigned long a_code, unsigned long& a_out) noexcept
	{
		a_out = a_code;
		return a_code == kCppExceptionCode ? EXCEPTION_CONTINUE_SEARCH : EXCEPTION_EXECUTE_HANDLER;
	}

	// Runs a_fn(). Returns false, with the exception code in a_code, if it raised a hardware or OS fault.
	template <class F>
	[[nodiscard]] bool Guarded(F&& a_fn, unsigned long& a_code)
	{
		__try {
			a_fn();
			return true;
		} __except (FaultFilter(GetExceptionCode(), a_code)) {
			return false;
		}
	}

	// memcpy that reports an unreadable source instead of crashing. Returns the bytes copied (0 on a fault).
	[[nodiscard]] inline std::size_t SafeCopy(void* a_to, const void* a_from, const std::size_t a_bytes) noexcept
	{
		__try {
			std::memcpy(a_to, a_from, a_bytes);
			return a_bytes;
		} __except (EXCEPTION_EXECUTE_HANDLER) {
			return 0;
		}
	}

	// Reads one value of type T from an address that may be invalid.
	template <class T>
	[[nodiscard]] inline bool SafeRead(const void* a_from, T& a_out) noexcept
	{
		return SafeCopy(std::addressof(a_out), a_from, sizeof(T)) == sizeof(T);
	}
}
