#include "Engine/EquipPending.h"

#include <atomic>

#include "Config.h"
#include "Engine/AudioProbe.h"
#include "Engine/CoverPose.h"
#include "Engine/Frame.h"
#include "Engine/Guard.h"
#include "Engine/Offsets.h"
#include "Engine/PlayerControl.h"

namespace Engine::EquipPending
{
	namespace
	{
		std::atomic<bool> s_faulted{ false };
		int               s_writes{ 0 };
		int               s_readsOne{ 0 };
		std::uint32_t     s_lastKey{ 0 };
		double            s_tKey{ -10.0 };
		bool              s_haveLast{ false };
		Fields            s_last;
		double            s_lastCover{ -9.0 }, s_lastEquipping{ -9.0 };
		int               s_lastWs{ -9 };
		int               s_lastPose{ -9 };

		// the process data of the player: AIProcess at Actor + 0x228 (the lib's currentProcess), middleHigh at AIProcess + 0x8, high at AIProcess + 0x10 (the lib's AIProcess; the listing
		// equipsend_101325.txt reads rax = [actor + 0x228], [rax + 8] and [rax + 0x10] the same way)
		[[nodiscard]] bool Bases(const std::byte*& a_mh, const std::byte*& a_high)
		{
			const auto player = RE::PlayerCharacter::GetSingleton();
			if (!player) {
				return false;
			}
			RE::AIProcess* proc{ nullptr };
			if (!SafeRead(&player->currentProcess, proc) || !proc) {
				return false;
			}
			const auto base = reinterpret_cast<const std::byte*>(proc);
			std::uintptr_t mh{ 0 }, hi{ 0 };
			if (!SafeRead(base + 0x8, mh) || !SafeRead(base + 0x10, hi)) {
				return false;
			}
			a_mh = reinterpret_cast<const std::byte*>(mh);
			a_high = reinterpret_cast<const std::byte*>(hi);
			return a_mh != nullptr;
		}

		[[nodiscard]] bool Same(const Fields& a, const Fields& b)
		{
			return a.b584 == b.b584 && a.b585 == b.b585 && a.b586 == b.b586 && a.d2D0 == b.d2D0 && a.d2F0 == b.d2F0 && a.d3B0 == b.d3B0 && a.d3D0 == b.d3D0 && a.h566 == b.h566 && a.h57E == b.h57E && a.h583 == b.h583;
		}

		[[nodiscard]] std::string Text(const Fields& f)
		{
			return std::format("mh+584 0x{:02X} +585 0x{:02X} +586 0x{:02X} | +2D0 {} +2F0 {} +3B0 0x{:X} +3D0 {} | high+566 0x{:02X} +57E 0x{:02X} +583 0x{:02X}", f.b584, f.b585, f.b586, f.d2D0, f.d2F0, f.d3B0, f.d3D0, f.h566, f.h57E, f.h583);
		}

		void Fault(const char* a_what, const unsigned long a_code)
		{
			s_faulted = true;
			REX::ERROR("EQUIP: exception 0x{:08X} in {}: iEquipFieldProbe and iPoseEquipPending are off for this session", a_code, a_what);
		}
	}

	Fields Read()
	{
		Fields f;
		const std::byte *mh{ nullptr }, *hi{ nullptr };
		if (!Bases(mh, hi)) {
			return f;
		}
		bool ok = SafeRead(mh + kMhDrawFlagsByte, f.b584) && SafeRead(mh + kMhDrawFlagsByte + 1, f.b585) && SafeRead(mh + kMhEquipPendingByte, f.b586) && SafeRead(mh + kMhEquipField2D0, f.d2D0) && SafeRead(mh + kMhEquipField2F0, f.d2F0) &&
		          SafeRead(mh + kMhEquipField3B0, f.d3B0) && SafeRead(mh + kMhEquipField3D0, f.d3D0);
		if (hi) {
			ok = ok && SafeRead(hi + kHighEquipByte566, f.h566) && SafeRead(hi + kHighEquipByte57E, f.h57E) && SafeRead(hi + kHighEquipByte583, f.h583);
		}
		f.ok = ok;
		return f;
	}

	Info GetInfo()
	{
		return { s_writes, s_readsOne, s_faulted.load() };
	}

	void OnTick(const double a_now)
	{
		const auto& cfg = Config::Get();
		if ((cfg.equipFieldProbe == 0 && cfg.poseEquipPending == 0) || s_faulted.load() || !cfg.enabled) {
			return;
		}
		unsigned long code{ 0 };
		if (!Guarded([&] {
				const auto key = Engine::AudioProbe::CurrentWeapon();
				if (key != 0 && key != s_lastKey) {
					if (s_lastKey != 0) {
						s_tKey = a_now;
						const auto f = Read();
						COVER_DETAIL("EQUIPFIELDS KEY: the weapon key changed 0x{:08X} -> 0x{:08X} (pose {}): {}", s_lastKey, key, CoverPose::ToString(CoverPose::GetInfo().state), f.ok ? Text(f) : std::string("(unreadable)"));
					}
					s_lastKey = key;
				}
				const auto f = Read();
				if (!f.ok) {
					return;
				}
				if (f.b586 == 1) {
					++s_readsOne;
				}
				if (cfg.poseEquipPending != 0 && f.b586 == 1 && CoverPose::Active()) {
					const std::byte *mh{ nullptr }, *hi{ nullptr };
					if (Bases(mh, hi)) {
						const std::uint8_t zero = 0;
						const bool wrote = SafeCopy(const_cast<std::byte*>(mh) + kMhEquipPendingByte, &zero, 1) == 1;
						if (wrote) {
							++s_writes;
						}
						const auto ws = CoverPose::WeaponState();
						COVER_DETAIL("EQUIP PENDING: the byte at middleHigh+0x586 read 1 {:.3f} s after the weapon key changed (pose {}, weapon state {}, bIsInCover {:g}, bIsEquipping {:g}): written 0 ({}); {}", a_now - s_tKey, CoverPose::ToString(CoverPose::GetInfo().state), ws,
							PlayerControl::ReadVar("bIsInCover").value_or(-9.0), PlayerControl::ReadVar("bIsEquipping").value_or(-9.0), wrote ? "done" : "FAILED", Text(f));
					}
				}
				if (cfg.equipFieldProbe != 0) {
					const double cov = PlayerControl::ReadVar("bIsInCover").value_or(-9.0);
					const double eq = PlayerControl::ReadVar("bIsEquipping").value_or(-9.0);
					const int    ws = CoverPose::WeaponState();
					const int    pose = static_cast<int>(CoverPose::GetInfo().state);
					if (!s_haveLast || !Same(f, s_last) || cov != s_lastCover || eq != s_lastEquipping || ws != s_lastWs || pose != s_lastPose) {
						COVER_DETAIL("EQUIPFIELDS {:+.3f} (since the key change): {} | bIsInCover {:g} bIsEquipping {:g} ws {} pose {}", a_now - s_tKey, Text(f), cov, eq, ws, CoverPose::ToString(CoverPose::GetInfo().state));
						s_last = f;
						s_haveLast = true;
						s_lastCover = cov;
						s_lastEquipping = eq;
						s_lastWs = ws;
						s_lastPose = pose;
					}
				}
			}, code)) {
			Fault("the tick", code);
		}
	}
}
