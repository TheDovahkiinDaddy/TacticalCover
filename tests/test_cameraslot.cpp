// Engine-free tests for Core/CameraSlot: who may share slot 12 of the ThirdPersonState vtable (M4b11).

#include "Core/CameraSlot.h"

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

	CameraSlotInput Held(const std::string& a_owner, const std::string& a_list = "DDC.dll", const int a_mode = 1)
	{
		CameraSlotInput in;
		in.slotHoldsGameUpdate = false;
		in.owner = a_owner;
		in.allowList = a_list;
		in.chainMode = a_mode;
		return in;
	}
}

int RunCameraSlotTests()
{
	g_failures = 0;

	// the list
	CHECK(ParseModuleList("DDC.dll").size() == 1);
	CHECK(ParseModuleList("DDC.dll, Other.DLL;  third.dll").size() == 3);
	CHECK(ParseModuleList("DDC.dll, Other.DLL;  third.dll")[1] == "other.dll");
	CHECK(ParseModuleList("").empty());
	CHECK(ParseModuleList(" , ;").empty());
	CHECK(ModuleInList("ddc.DLL", "DDC.dll"));
	CHECK(ModuleInList("C:\\Games\\Mods\\DDC\\SFSE\\Plugins\\DDC.dll", "x.dll DDC.dll"));
	CHECK(!ModuleInList("DDC2.dll", "DDC.dll"));
	CHECK(!ModuleInList("", "DDC.dll"));
	CHECK(!ModuleInList("DDC.dll", ""));

	// the game's own function in the slot: the hook goes in as always
	{
		CameraSlotInput in;
		in.slotHoldsGameUpdate = true;
		const auto r = DecideCameraSlot(in);
		CHECK(r.decision == CameraSlotDecision::Own);
		in.chainMode = 0;
		CHECK(DecideCameraSlot(in).decision == CameraSlotDecision::Own);
	}
	// Stride loaded: always stand down, whatever the slot and the list say (the old behaviour)
	{
		auto in = Held("DDC.dll");
		in.strideLoaded = true;
		CHECK(DecideCameraSlot(in).decision == CameraSlotDecision::Stand);
		in.slotHoldsGameUpdate = true;
		CHECK(DecideCameraSlot(in).decision == CameraSlotDecision::Stand);
		in = Held("DyMC.dll", "DyMC.dll DDC.dll");
		in.strideLoaded = true;
		CHECK(DecideCameraSlot(in).decision == CameraSlotDecision::Stand);
	}
	// an allow-listed owner: chain
	{
		const auto r = DecideCameraSlot(Held("DDC.dll"));
		CHECK(r.decision == CameraSlotDecision::Chain);
		CHECK(r.reason.find("ddc.dll") != std::string::npos);
		CHECK(DecideCameraSlot(Held("ddc.DLL")).decision == CameraSlotDecision::Chain);
		CHECK(DecideCameraSlot(Held("D:\\x\\DDC.dll")).decision == CameraSlotDecision::Chain);
	}
	// the switch at 0 is the old behaviour
	CHECK(DecideCameraSlot(Held("DDC.dll", "DDC.dll", 0)).decision == CameraSlotDecision::Stand);
	// not on the list, unknown owner, empty list
	CHECK(DecideCameraSlot(Held("Other.dll")).decision == CameraSlotDecision::Stand);
	CHECK(DecideCameraSlot(Held("")).decision == CameraSlotDecision::Stand);
	CHECK(DecideCameraSlot(Held("DDC.dll", "")).decision == CameraSlotDecision::Stand);
	// DyMC.dll is never chained, even when someone puts it on the list
	CHECK(NeverChained("DyMC.dll"));
	CHECK(NeverChained("dymc.DLL"));
	CHECK(!NeverChained("DDC.dll"));
	{
		const auto r = DecideCameraSlot(Held("DyMC.dll", "DyMC.dll DDC.dll"));
		CHECK(r.decision == CameraSlotDecision::Stand);
		CHECK(r.reason.find("never chained") != std::string::npos);
	}

	std::printf("CameraSlot tests: %s\n", g_failures == 0 ? "ok" : "FAILED");
	return g_failures;
}
