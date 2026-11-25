#include "Engine/CameraSettings.h"

#include "Config.h"
#include "Engine/Guard.h"

namespace Engine::CameraSettings
{
	namespace
	{
		// From tools\exe_strings.py on Starfield.exe 1.16.244 (pattern ':Camera$'), 2026-11-16.
		constexpr std::array<const char*, 16> kNames{
			"fOverShoulderPosX:Camera", "fOverShoulderPosZ:Camera", "fOverShoulderCombatPosX:Camera", "fOverShoulderCombatAddY:Camera", "fOverShoulderCombatPosZ:Camera",
			"fOverShoulderMeleeCombatPosX:Camera", "fOverShoulderMeleeCombatAddY:Camera", "fOverShoulderMeleeCombatPosZ:Camera", "fOverShoulderHorsePosX:Camera",
			"fShoulderDollySpeed:Camera", "fOverShoulderOffsetCollisionScalar:Camera", "f3rdPersonDistanceLerpSpeed:Camera", "bTestLeftSideCameras:Camera",
			"b3rdPersonCameraCollision:Camera", "f3rdPersonCollisionDelay:Camera", "fVehicleThirdPersonFOVOffset:Camera"
		};

		bool s_logged{ false };
		bool s_broken{ false };

		[[nodiscard]] RE::Setting* Find(const char* a_name)
		{
			return RE::GetINISetting(a_name);
		}
	}

	std::span<const char* const> Names()
	{
		return kNames;
	}

	void LogAllOnce()
	{
		if (s_logged || !Config::Get().cameraSettings || s_broken) {
			return;
		}
		unsigned long code{ 0 };
		if (!Guarded(
				[&] {
					if (!RE::INISettingCollection::GetSingleton()) {
						return;  // not there yet: try again on a later call
					}
					s_logged = true;
					for (const char* name : kNames) {
						const auto* s = Find(name);
						if (!s) {
							COVER_DETAIL("S4 camera setting {}: NOT FOUND", name);
							continue;
						}
						switch (s->GetType()) {
						case RE::Setting::Type::kFloat:
							COVER_DETAIL("S4 camera setting {}: found at {} type float value {:.4f} default {:.4f}", name, static_cast<const void*>(s), s->GetFloat(), s->GetFloat(true));
							break;
						case RE::Setting::Type::kBool:
							COVER_DETAIL("S4 camera setting {}: found at {} type bool value {} default {}", name, static_cast<const void*>(s), s->GetBool(), s->GetBool(true));
							break;
						case RE::Setting::Type::kInt:
							COVER_DETAIL("S4 camera setting {}: found at {} type int value {} default {}", name, static_cast<const void*>(s), s->GetInt(), s->GetInt(true));
							break;
						default:
							COVER_DETAIL("S4 camera setting {}: found at {} type {}", name, static_cast<const void*>(s), static_cast<int>(s->GetType()));
							break;
						}
					}
				},
				code)) {
			s_broken = true;
			s_logged = true;
			REX::ERROR("S4: exception 0x{:08X} while reading the camera INI settings; the settings route is off for this session", code);
		}
	}

	bool GetFloat(const char* a_name, float& a_out)
	{
		if (s_broken || !Config::Get().cameraSettings) {
			return false;
		}
		bool          ok{ false };
		unsigned long code{ 0 };
		if (!Guarded(
				[&] {
					if (const auto* s = Find(a_name); s && s->GetType() == RE::Setting::Type::kFloat) {
						a_out = s->GetFloat();
						ok = true;
					}
				},
				code)) {
			s_broken = true;
			REX::ERROR("S4: exception 0x{:08X} reading camera setting {}; the settings route is off for this session", code, a_name);
			return false;
		}
		return ok;
	}

	bool GetBool(const char* a_name, bool& a_out)
	{
		if (s_broken || !Config::Get().cameraSettings) {
			return false;
		}
		bool          ok{ false };
		unsigned long code{ 0 };
		if (!Guarded(
				[&] {
					if (const auto* s = Find(a_name); s && s->GetType() == RE::Setting::Type::kBool) {
						a_out = s->GetBool();
						ok = true;
					}
				},
				code)) {
			s_broken = true;
			REX::ERROR("S4: exception 0x{:08X} reading camera setting {}; the settings route is off for this session", code, a_name);
			return false;
		}
		return ok;
	}

	bool SetFloat(const char* a_name, const float a_value)
	{
		if (s_broken || !Config::Get().cameraSettings) {
			return false;
		}
		bool          ok{ false };
		unsigned long code{ 0 };
		if (!Guarded(
				[&] {
					if (auto* s = Find(a_name); s && s->GetType() == RE::Setting::Type::kFloat) {
						s->SetFloat(a_value);
						ok = true;
					}
				},
				code)) {
			s_broken = true;
			REX::ERROR("S4: exception 0x{:08X} writing camera setting {}; the settings route is off for this session", code, a_name);
			return false;
		}
		return ok;
	}

	bool SetBool(const char* a_name, const bool a_value)
	{
		if (s_broken || !Config::Get().cameraSettings) {
			return false;
		}
		bool          ok{ false };
		unsigned long code{ 0 };
		if (!Guarded(
				[&] {
					if (auto* s = Find(a_name); s && s->GetType() == RE::Setting::Type::kBool) {
						s->SetBool(a_value);
						ok = true;
					}
				},
				code)) {
			s_broken = true;
			REX::ERROR("S4: exception 0x{:08X} writing camera setting {}; the settings route is off for this session", code, a_name);
			return false;
		}
		return ok;
	}
}
