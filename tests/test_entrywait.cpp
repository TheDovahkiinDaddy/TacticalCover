// Engine-free tests for Core/EntryWait: when the pose's stance call is made (REFERENCE 23.32).

#include "Core/EntryWait.h"

#include <cstdio>

namespace
{
	int g_failures = 0;

#define CHECK(cond)                                                     \
	do {                                                                \
		if (!(cond)) {                                                  \
			std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
			++g_failures;                                               \
		}                                                               \
	} while (false)

	using namespace Core;
}

int RunEntryWaitTests()
{
	g_failures = 0;
	StanceCallInput in;

	// off: the call comes at once, whatever the byte says (the accepted behaviour)
	in = {};
	in.pendingByte = 2;
	CHECK(DecideStanceCall(in).call);
	in.switchWait = 0.2;
	in.sinceSwitch = 0.1;
	CHECK(!DecideStanceCall(in).call);
	in.sinceSwitch = 0.2;
	CHECK(DecideStanceCall(in).call);

	// on: the first call waits for the byte
	in = {};
	in.waitForByte = true;
	in.pendingByte = 2;
	in.sinceSwitch = 0.03;
	CHECK(!DecideStanceCall(in).call);
	in.sinceSwitch = 1.0;
	CHECK(!DecideStanceCall(in).call);
	in.pendingByte = 1;
	CHECK(!DecideStanceCall(in).call);
	in.pendingByte = 0;
	in.sinceSwitch = 0.03;
	CHECK(DecideStanceCall(in).call);
	// the limit: the call is made as before, the cover must not hang
	in.pendingByte = 2;
	in.sinceSwitch = 1.49;
	CHECK(!DecideStanceCall(in).call);
	in.sinceSwitch = 1.5;
	CHECK(DecideStanceCall(in).call);
	// an unreadable byte is no reason to wait
	in.pendingByte = -1;
	in.sinceSwitch = 0.03;
	CHECK(DecideStanceCall(in).call);
	// 23.36: mode 2, the first call also waits for the equip byte (0x586)
	in = {};
	in.waitForByte = true;
	in.waitForEquipByte = true;
	in.pendingByte = 0;
	in.equipByte = 1;
	in.sinceSwitch = 0.05;
	CHECK(!DecideStanceCall(in).call);
	in.sinceSwitch = 1.49;
	CHECK(!DecideStanceCall(in).call);
	in.sinceSwitch = 1.5;
	CHECK(DecideStanceCall(in).call);  // the limit: as before, the cover must not hang
	in.sinceSwitch = 0.05;
	in.equipByte = 0;
	CHECK(DecideStanceCall(in).call);
	in.equipByte = -1;  // unreadable: no reason to wait
	CHECK(DecideStanceCall(in).call);
	in.pendingByte = 2;
	in.equipByte = 0;
	CHECK(!DecideStanceCall(in).call);  // the first byte still counts
	// mode 1 ignores the equip byte
	in = {};
	in.waitForByte = true;
	in.pendingByte = 0;
	in.equipByte = 1;
	in.sinceSwitch = 0.05;
	CHECK(DecideStanceCall(in).call);
	// iPoseNoRepeat: the first call as before, never a repeat
	in = {};
	in.noRepeat = true;
	CHECK(DecideStanceCall(in).call);
	in.callsMade = 1;
	in.sinceLastCall = 0.6;
	CHECK(!DecideStanceCall(in).call);
	in.sinceLastCall = 5.0;
	CHECK(!DecideStanceCall(in).call);
	in.noRepeat = false;
	CHECK(DecideStanceCall(in).call);

	// the least wait counts first
	in = {};
	in.waitForByte = true;
	in.pendingByte = 0;
	in.switchWait = 0.2;
	in.sinceSwitch = 0.1;
	CHECK(!DecideStanceCall(in).call);

	// repeats: as before when off
	in = {};
	in.callsMade = 1;
	in.pendingByte = 2;
	in.sinceLastCall = 0.5;
	CHECK(!DecideStanceCall(in).call);
	in.sinceLastCall = 0.51;
	CHECK(DecideStanceCall(in).call);
	in.callsMade = 3;
	CHECK(!DecideStanceCall(in).call);
	// repeats with the switch on: only with the byte clear, or after the limit
	in = {};
	in.callsMade = 1;
	in.waitForByte = true;
	in.sinceLastCall = 0.6;
	in.pendingByte = 2;
	CHECK(!DecideStanceCall(in).call);
	in.pendingByte = 0;
	CHECK(DecideStanceCall(in).call);
	in.pendingByte = 2;
	in.sinceLastCall = 1.5;
	CHECK(DecideStanceCall(in).call);
	in.pendingByte = -1;
	in.sinceLastCall = 0.6;
	CHECK(DecideStanceCall(in).call);
	in.callsMade = 2;
	CHECK(DecideStanceCall(in).call);
	in.callsMade = 3;
	CHECK(!DecideStanceCall(in).call);

	return g_failures;
}
