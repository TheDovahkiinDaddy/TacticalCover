#pragma once

// When the pose's stance call is made (REFERENCE 23.31 and 23.32). Engine-free and unit-tested.
//
// The user's first long play showed the first stance call bringing the cover state in only when the archetype-pending byte (middleHigh + 0x585) had already
// gone back to 0; with the byte still 2 none of 23 first calls worked. With iPoseWaitByte 1 the first call waits for that byte, up to a limit after which
// the call is made as it was before (the cover must not hang on a byte that never clears). A repeat call is made after the usual 0.5 s, and with the
// switch on only when the byte reads 0 too (or the limit has passed since the last call).

namespace Core
{
	struct StanceCallInput
	{
		int    callsMade{ 0 };        // stance calls made in this pose so far
		double sinceSwitch{ 0.0 };    // seconds since the archetype was cleared
		double sinceLastCall{ 0.0 };  // seconds since the last stance call (unused for the first)
		double switchWait{ 0.0 };     // fPoseSwitchWait: the least wait before the first call
		bool   waitForByte{ false };  // iPoseWaitByte
		int    pendingByte{ -1 };     // middleHigh + 0x585: 0 none, 1 or 2 pending, -1 unreadable
		bool   waitForEquipByte{ false };  // iPoseWaitByte 2 (23.35): the first call also waits for middleHigh + 0x586 (the equip owed byte) to read 0
		int    equipByte{ -1 };       // middleHigh + 0x586: 0 none, 1 owed, -1 unreadable
		bool   noRepeat{ false };     // iPoseNoRepeat (23.36): no repeat call at all; the session ends as a failed one does (the caller)
		double maxWait{ 1.5 };        // fPoseWaitByteMax: the longest the byte is waited for (from the clear for the first call, from the last call for a repeat)
		double repeatAfter{ 0.5 };    // the wait before a repeat call
		int    maxCalls{ 3 };
	};

	struct StanceCallResult
	{
		bool        call{ false };
		const char* reason{ "" };  // why: for the log, a string literal
	};

	[[nodiscard]] StanceCallResult DecideStanceCall(const StanceCallInput& a_in);
}
