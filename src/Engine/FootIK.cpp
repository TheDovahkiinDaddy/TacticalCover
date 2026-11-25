#include "Engine/FootIK.h"

#include "Engine/Guard.h"

namespace Engine::FootIK
{
	namespace
	{
		constexpr const char* kHumanoid = "bHumanoidFootIKEnable:Animation";
		constexpr const char* kFeet = "bEnableFootIK:Animation";

		bool s_probed{ false };
		bool s_found{ false };
		bool s_broken{ false };
		bool s_haveOrig{ false };
		bool s_origHumanoid{ true };
		bool s_origFeet{ true };
		int  s_changes{ 0 };

		[[nodiscard]] RE::Setting* Find(const char* a_name) { return RE::GetINISetting(a_name); }

		void Fault(const unsigned long a_code, const char* a_what)
		{
			s_broken = true;
			REX::ERROR("FOOTIK: exception 0x{:08X} while {}: the foot IK switch is off for this session", a_code, a_what);
		}
	}

	bool Probe()
	{
		if (s_broken) {
			return false;
		}
		if (s_probed) {
			return s_found;
		}
		unsigned long code{ 0 };
		if (!Guarded([&] {
				if (!RE::INISettingCollection::GetSingleton()) {
					return;  // not there yet: a later call tries again
				}
				s_probed = true;
				const auto* h = Find(kHumanoid);
				const auto* f = Find(kFeet);
				REX::INFO("FOOTIK: {} {} (type {}, value {}); {} {} (type {}, value {})", kHumanoid, h ? "found" : "NOT FOUND", h ? static_cast<int>(h->GetType()) : -1, h && h->GetType() == RE::Setting::Type::kBool ? h->GetBool() : false, kFeet, f ? "found" : "NOT FOUND",
					f ? static_cast<int>(f->GetType()) : -1, f && f->GetType() == RE::Setting::Type::kBool ? f->GetBool() : false);
				if (h && f && h->GetType() == RE::Setting::Type::kBool && f->GetType() == RE::Setting::Type::kBool) {
					s_found = true;
					if (!s_haveOrig) {
						s_origHumanoid = h->GetBool();
						s_origFeet = f->GetBool();
						s_haveOrig = true;
					}
				}
			},
				code)) {
			Fault(code, "looking up the foot IK settings");
			return false;
		}
		return s_found;
	}

	bool Set(const bool a_on, const char* a_why)
	{
		if (!Probe()) {
			return false;
		}
		unsigned long code{ 0 };
		bool          ok{ false };
		if (!Guarded([&] {
				auto* h = Find(kHumanoid);
				auto* f = Find(kFeet);
				if (!h || !f || h->GetType() != RE::Setting::Type::kBool || f->GetType() != RE::Setting::Type::kBool) {
					return;
				}
				const bool bh = h->GetBool();
				const bool bf = f->GetBool();
				if (bh == a_on && bf == a_on) {
					return;
				}
				h->SetBool(a_on);
				f->SetBool(a_on);
				++s_changes;
				COVER_DETAIL("FOOTIK: {} -> {} ({}: bHumanoidFootIKEnable {} -> {}, bEnableFootIK {} -> {}; originals {} / {})", a_on ? "ON" : "OFF", a_on ? "on" : "off", a_why ? a_why : "", bh, h->GetBool(), bf, f->GetBool(), s_origHumanoid, s_origFeet);
				ok = true;
			},
				code)) {
			Fault(code, "writing the foot IK settings");
			return false;
		}
		(void)ok;
		return true;
	}

	bool SetEach(const bool a_humanoid, const bool a_feet, const char* a_why)
	{
		if (!Probe()) {
			return false;
		}
		unsigned long code{ 0 };
		if (!Guarded([&] {
				auto* h = Find(kHumanoid);
				auto* f = Find(kFeet);
				if (!h || !f || h->GetType() != RE::Setting::Type::kBool || f->GetType() != RE::Setting::Type::kBool) {
					return;
				}
				const bool bh = h->GetBool();
				const bool bf = f->GetBool();
				if (bh == a_humanoid && bf == a_feet) {
					return;
				}
				h->SetBool(a_humanoid);
				f->SetBool(a_feet);
				++s_changes;
				COVER_DETAIL("FOOTIK: humanoid {} / feet {} ({}: bHumanoidFootIKEnable {} -> {}, bEnableFootIK {} -> {}; originals {} / {})", a_humanoid ? "on" : "off", a_feet ? "on" : "off", a_why ? a_why : "", bh, a_humanoid, bf, a_feet, s_origHumanoid, s_origFeet);
			},
				code)) {
			Fault(code, "writing the foot IK settings");
			return false;
		}
		return true;
	}

	void Restore(const char* a_why)
	{
		if (!s_found || !s_haveOrig) {
			return;  // (a fault leaves the switch broken, but the values are still put back)
		}
		unsigned long code{ 0 };
		if (!Guarded([&] {
				auto* h = Find(kHumanoid);
				auto* f = Find(kFeet);
				if (!h || !f || h->GetType() != RE::Setting::Type::kBool || f->GetType() != RE::Setting::Type::kBool) {
					return;
				}
				if (h->GetBool() == s_origHumanoid && f->GetBool() == s_origFeet) {
					return;
				}
				COVER_DETAIL("FOOTIK: restored to the original values ({}: bHumanoidFootIKEnable {} -> {}, bEnableFootIK {} -> {})", a_why ? a_why : "", h->GetBool(), s_origHumanoid, f->GetBool(), s_origFeet);
				h->SetBool(s_origHumanoid);
				f->SetBool(s_origFeet);
			},
				code)) {
			Fault(code, "restoring the foot IK settings");
		}
	}

	Info FootIKInfoImpl()
	{
		Info i;
		i.found = s_found;
		i.broken = s_broken;
		i.changes = s_changes;
		i.origHumanoid = s_origHumanoid;
		i.origFeet = s_origFeet;
		unsigned long code{ 0 };
		(void)Guarded([&] {
			if (!s_found) {
				return;
			}
			const auto* h = Find(kHumanoid);
			const auto* f = Find(kFeet);
			if (h && f && h->GetType() == RE::Setting::Type::kBool && f->GetType() == RE::Setting::Type::kBool) {
				i.humanoid = h->GetBool();
				i.feet = f->GetBool();
				i.changed = i.humanoid != s_origHumanoid || i.feet != s_origFeet;
			}
		},
			code);
		return i;
	}

	Info GetInfo() { return FootIKInfoImpl(); }
}
