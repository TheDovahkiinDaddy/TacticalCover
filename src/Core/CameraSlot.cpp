#include "Core/CameraSlot.h"

#include <algorithm>
#include <cctype>

namespace Core
{
	namespace
	{
		[[nodiscard]] std::string FileNameLower(const std::string& a_text)
		{
			auto s = a_text;
			if (const auto slash = s.find_last_of("\\/"); slash != std::string::npos) {
				s = s.substr(slash + 1);
			}
			std::transform(s.begin(), s.end(), s.begin(), [](const unsigned char c) { return static_cast<char>(std::tolower(c)); });
			return s;
		}
	}

	std::vector<std::string> ParseModuleList(const std::string& a_text)
	{
		std::vector<std::string> out;
		std::string              cur;
		const auto               flush = [&] {
            if (!cur.empty()) {
                out.push_back(FileNameLower(cur));
                cur.clear();
            }
		};
		for (const char c : a_text) {
			if (c == ',' || c == ';' || c == ' ' || c == '\t' || c == '\r' || c == '\n') {
				flush();
			} else {
				cur.push_back(c);
			}
		}
		flush();
		out.erase(std::remove_if(out.begin(), out.end(), [](const std::string& s) { return s.empty(); }), out.end());
		return out;
	}

	bool ModuleInList(const std::string& a_name, const std::string& a_list)
	{
		const auto name = FileNameLower(a_name);
		if (name.empty()) {
			return false;
		}
		const auto list = ParseModuleList(a_list);
		return std::find(list.begin(), list.end(), name) != list.end();
	}

	bool NeverChained(const std::string& a_name)
	{
		// Stride's camera mod owns the whole third-person camera (REFERENCE 19): chaining behind or in front of it was never proved, and the user's own rule is to leave it alone
		return FileNameLower(a_name) == "dymc.dll";
	}

	CameraSlotResult DecideCameraSlot(const CameraSlotInput& a_in)
	{
		CameraSlotResult r;
		if (a_in.strideLoaded) {
			r.decision = CameraSlotDecision::Stand;
			r.reason = "DyMC.dll (Stride) is loaded in this process: it owns the third-person camera";
			return r;
		}
		if (a_in.slotHoldsGameUpdate) {
			r.decision = CameraSlotDecision::Own;
			r.reason = "slot 12 holds the game's own ThirdPersonState::Update";
			return r;
		}
		if (a_in.chainMode == 0) {
			r.reason = "slot 12 does not hold the game function and iCameraChain is 0";
			return r;
		}
		if (a_in.owner.empty()) {
			r.reason = "slot 12 does not hold the game function and the module that owns the code in it cannot be told";
			return r;
		}
		if (NeverChained(a_in.owner)) {
			r.reason = "slot 12 is held by " + FileNameLower(a_in.owner) + ", which is never chained";
			return r;
		}
		if (!ModuleInList(a_in.owner, a_in.allowList)) {
			r.reason = "slot 12 is held by " + FileNameLower(a_in.owner) + ", which is not on sCameraChainModules";
			return r;
		}
		r.decision = CameraSlotDecision::Chain;
		r.reason = "slot 12 is held by " + FileNameLower(a_in.owner) + ", which is on sCameraChainModules: our hook is put in front of it and calls its function as the original";
		return r;
	}
}
