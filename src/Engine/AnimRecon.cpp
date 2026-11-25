#include "Engine/AnimRecon.h"

#include "Engine/GraphEvents.h"
#include "Engine/Guard.h"
#include "Engine/Offsets.h"
#include "Engine/PlayerControl.h"

namespace Engine::AnimRecon
{
	namespace
	{
		// docs/REFERENCE.md section 3.2 and 3.4 (variables of cover.agx, gunactions.agx and the player's graph).
		constexpr std::array<const char*, 17> kCoverVars{
			"bIsInCover", "iIsPlayer", "iSyncRightLeft", "iSyncStandingCrouching", "iSyncCoverStates", "iPartialCover", "iIsSighted", "iSyncSighted",
			"iIsBlindFiring", "bIsEnteringCover", "bIsExitingCover", "bIsShuffling", "iSyncGravity", "Direction", "fDesiredHeading", "iSyncRelaxedReadyAlert",
			"iSyncActorStance"
		};

		constexpr RE::TESFormID kAnimArchetypePlayer = 0x0006B508;  // tools/esm_subgraphs.py, Starfield.esm 1.16.244: AnimArchetypePlayer[0006B508]

		RE::BGSKeyword* s_removedKeyword{ nullptr };

		[[nodiscard]] RE::BGSKeyword* Keyword()
		{
			return RE::TESForm::LookupByID<RE::BGSKeyword>(kAnimArchetypePlayer);
		}
	}

	std::span<const char* const> CoverVars()
	{
		return kCoverVars;
	}

	void LogCoverVars(const char* a_label)
	{
		COVER_DETAIL("S6 {}: {} | {} | cam={}", a_label, PlayerControl::DescribeVars(kCoverVars), PlayerControl::ActorFlags(),
			RE::PlayerCamera::GetSingleton() && RE::PlayerCamera::GetSingleton()->currentState ? "ok" : "none");
	}

	bool SendEvent(const char* a_event, bool& a_accepted)
	{
		a_accepted = false;
		const auto player = RE::PlayerCharacter::GetSingleton();
		if (!player) {
			return false;
		}
		const RE::BSFixedString name{ a_event };
		unsigned long           code{ 0 };
		GraphEvents::SetOwn(true);
	const bool              ok = Guarded([&] { a_accepted = player->NotifyAnimationGraphImpl(name); }, code);
	GraphEvents::SetOwn(false);
		if (!ok) {
			REX::ERROR("S6: exception 0x{:08X} in NotifyAnimationGraphImpl('{}')", code, a_event);
		}
		return ok;
	}

	std::string InspectArchetypeKeyword()
	{
		std::string out;
		unsigned long code{ 0 };
		const bool ok = Guarded(
			[&] {
				const auto kw = Keyword();
				const auto player = RE::PlayerCharacter::GetSingleton();
				if (!kw || !player) {
					out = std::format("keyword {:08X} {}", kAnimArchetypePlayer, kw ? "found" : "NOT FOUND by LookupByID");
					return;
				}
				const bool onRef = player->HasKeyword(kw);
				const auto npc = player->GetNPC();
				const bool onBase = npc && static_cast<RE::BGSKeywordForm*>(npc)->HasKeyword(kw);
				const auto race = npc ? npc->GetRace() : nullptr;
				const bool onRace = race && static_cast<RE::BGSKeywordForm*>(race)->HasKeyword(kw);
				out = std::format("AnimArchetypePlayer {:08X} ({}): HasKeyword on the player reference={} on the actor base={} on the race={} | base keyword count {}", kAnimArchetypePlayer,
					static_cast<const void*>(kw), onRef, onBase, onRace, npc ? static_cast<RE::BGSKeywordForm*>(npc)->GetNumKeywords() : 0u);
			},
			code);
		if (!ok) {
			out = std::format("exception 0x{:08X} while inspecting the keyword", code);
		}
		return out;
	}

	bool RemoveArchetypeFromBase()
	{
		bool          changed{ false };
		unsigned long code{ 0 };
		const bool    ok = Guarded(
            [&] {
                const auto kw = Keyword();
                const auto player = RE::PlayerCharacter::GetSingleton();
                const auto npc = player ? player->GetNPC() : nullptr;
                if (!kw || !npc) {
                    return;
                }
                changed = static_cast<RE::BGSKeywordForm*>(npc)->RemoveKeyword(kw);
                if (changed) {
                    s_removedKeyword = kw;
                }
            },
            code);
		if (!ok) {
			REX::ERROR("S6: exception 0x{:08X} while removing the keyword from the actor base", code);
		}
		return ok && changed;
	}

	bool RestoreArchetypeOnBase()
	{
		bool          changed{ false };
		unsigned long code{ 0 };
		const bool    ok = Guarded(
            [&] {
                const auto player = RE::PlayerCharacter::GetSingleton();
                const auto npc = player ? player->GetNPC() : nullptr;
                if (!s_removedKeyword || !npc) {
                    return;
                }
                changed = static_cast<RE::BGSKeywordForm*>(npc)->AddKeyword(s_removedKeyword);
                s_removedKeyword = nullptr;
            },
            code);
		if (!ok) {
			REX::ERROR("S6: exception 0x{:08X} while putting the keyword back on the actor base", code);
		}
		return ok && changed;
	}

	namespace
	{
		bool s_archetypeChanged{ false };
		int  s_changeChecked{ 0 };  // 0 not checked, 1 verified, -1 refused

		[[nodiscard]] bool ChangeVerified()
		{
			if (s_changeChecked == 0) {
				const auto rva = kChangeAnimArchetype.address() - REX::FModule::GetExecutingModule().GetBaseAddress();
				s_changeChecked = rva == kChangeAnimArchetypeRva ? 1 : -1;
				REX::INFO("S6 route A: ActorUtils::ChangeAnimArchetype ID {} resolves to RVA 0x{:X} (expected 0x{:X}) {}", kChangeAnimArchetype.id(), rva, kChangeAnimArchetypeRva,
					s_changeChecked == 1 ? "- OK" : "- MISMATCH, the function is not called");
			}
			return s_changeChecked == 1;
		}
	}

	bool ChangeArchetype(const RE::TESFormID a_keywordFormId, bool& a_returned)
	{
		a_returned = false;
		if (!ChangeVerified()) {
			return false;
		}
		unsigned long code{ 0 };
		bool          ran{ false };
		const bool    ok = Guarded(
            [&] {
                const auto player = RE::PlayerCharacter::GetSingleton();
                RE::BGSKeyword* kw = a_keywordFormId ? RE::TESForm::LookupByID<RE::BGSKeyword>(a_keywordFormId) : nullptr;
                if (!player || (a_keywordFormId && !kw)) {
                    return;
                }
                using func_t = bool (*)(RE::Actor*, RE::BGSKeyword*);
                const auto fn = reinterpret_cast<func_t>(kChangeAnimArchetype.address());
                a_returned = fn(player, kw);
                ran = true;
            },
            code);
		if (!ok) {
			REX::ERROR("S6 route A: exception 0x{:08X} in ActorUtils::ChangeAnimArchetype({:08X})", code, a_keywordFormId);
			return false;
		}
		if (ran && a_keywordFormId != kAnimArchetypePlayer) {
			s_archetypeChanged = true;
		}
		return ran;
	}

	bool ArchetypeChanged() noexcept
	{
		return s_archetypeChanged;
	}

	bool RestoreArchetype(bool& a_returned)
	{
		const bool ran = ChangeArchetype(kAnimArchetypePlayer, a_returned);
		if (ran) {
			s_archetypeChanged = false;
		}
		return ran;
	}

	bool DrawWeapon(const bool a_draw)
	{
		unsigned long code{ 0 };
		const bool    ok = Guarded(
            [&] {
                if (const auto player = RE::PlayerCharacter::GetSingleton()) {
                    player->DrawWeaponMagicHands(a_draw);
                }
            },
            code);
		if (!ok) {
			REX::ERROR("S6: exception 0x{:08X} in Actor::DrawWeaponMagicHands({})", code, a_draw);
		}
		return ok;
	}

	RE::NiPoint3 PlayerPosition()
	{
		const auto player = RE::PlayerCharacter::GetSingleton();
		return player ? player->GetPosition() : RE::NiPoint3{};
	}
}
