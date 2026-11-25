#include "Engine/ScopeAim.h"

#include <atomic>
#include <map>
#include <mutex>
#include <set>

#include "Config.h"
#include "Engine/AudioProbe.h"
#include "Engine/CoverPose.h"
#include "Engine/PlayerControl.h"
#include "Engine/TallAim.h"

namespace Engine::ScopeAim
{
	namespace
	{
		std::mutex                       s_mutex;  // the tick and the hook thread
		std::map<std::uint32_t, bool>    s_cache;   // bHasScope per weapon key, read outside the pose
		std::set<std::uint32_t>          s_learned; // the camera left third person while the aim control was held, or read from the cache file
		std::set<std::uint32_t>          s_persisted;  // what the cache file holds (read at start, and every weapon learned since)
		std::atomic<bool>                s_dirty{ false };  // the file has to be written (done at the start of the next tick, outside the mutex: the writer asks PersistedKeys())
		std::uint32_t                    s_key{ 0 };
		std::uint32_t                    s_prevKey{ 0 };
		double                           s_tKey{ -10.0 };
		double                           s_tEnded{ -10.0 };  // the key change the aim end was already made for
		double                           s_nextRead{ 0.0 };
		double                           s_nextPoll{ 0.0 };
		bool                             s_prevThird{ true };
		bool                             s_varReadable{ false };
		double                           s_hasScope{ -1.0 };
		double                           s_inScopes{ -1.0 };
		bool                             s_byVar{ false };
		bool                             s_byLearn{ false };
		std::atomic<bool>                s_scopedNow{ false };
		std::atomic<bool>                s_series{ false };       // a press was held back: its repeats and its release are too
		std::atomic<bool>                s_noticeDue{ false };
		bool                             s_noticeShown{ false };
		std::set<std::uint32_t>          s_disagreeLogged;
		Info                             s_info;

		[[nodiscard]] bool PoseOn() { return CoverPose::Active(); }
	}

	Info GetInfo()
	{
		std::lock_guard lock(s_mutex);
		Info            i = s_info;
		i.key = s_key;
		i.scoped = s_scopedNow.load();
		i.byVar = s_byVar;
		i.byLearn = s_byLearn;
		i.varReadable = s_varReadable;
		i.hasScope = s_hasScope;
		i.inScopes = s_inScopes;
		return i;
	}

	bool Locked()
	{
		return Config::Get().poseScopeAim == 2 && PoseOn() && s_scopedNow.load();
	}

	bool TakeNotice()
	{
		return s_noticeDue.exchange(false);
	}

	void Forget()
	{
		std::lock_guard lock(s_mutex);
		s_cache.clear();
		s_learned.clear();  // memory only: what the cache file holds is read again at the next start
		s_disagreeLogged.clear();
		COVER_DETAIL("SCOPE AIM: the cache and what was learned are forgotten (rig)");
	}

	void ForgetLearned()
	{
		std::lock_guard lock(s_mutex);
		s_learned.clear();
		COVER_DETAIL("SCOPE AIM: what was learned is forgotten (rig); the bHasScope cache stays");
	}

	void LearnForRig(const std::uint32_t a_key, const bool a_persist)
	{
		std::lock_guard lock(s_mutex);
		if (s_learned.insert(a_key).second) {
			++s_info.learned;
		}
		if (a_persist && s_persisted.insert(a_key).second) {
			s_dirty = true;
		}
		COVER_DETAIL("SCOPE AIM: weapon 0x{:08X} is marked scoped by the rig (as if it had been aimed in the open before){}", a_key, a_persist ? "; it goes into the cache file" : "");
	}

	void LoadPersisted(const std::uint32_t a_key)
	{
		std::lock_guard lock(s_mutex);
		s_persisted.insert(a_key);
		s_learned.insert(a_key);
		COVER_DETAIL("SCOPE AIM: weapon 0x{:08X} is read from the cache file as a weapon with a scope", a_key);
	}

	std::vector<std::uint32_t> PersistedKeys()
	{
		std::lock_guard lock(s_mutex);
		return { s_persisted.begin(), s_persisted.end() };
	}

	bool KnownScoped(const std::uint32_t a_key)
	{
		std::lock_guard lock(s_mutex);
		const auto      src = Config::Get().scopeSource;
		const auto      c = s_cache.find(a_key);
		return ((src & 1) != 0 && c != s_cache.end() && c->second) || ((src & 2) != 0 && s_learned.contains(a_key));
	}

	Plan OnAim(const bool a_down, const bool a_pressEdge)
	{
		Plan plan;
		if (Config::Get().poseScopeAim != 2) {
			return plan;
		}
		if (s_series.load()) {
			plan.swallow = true;  // the held-back press: its repeats and its release never reach the game
			if (!a_down) {
				s_series = false;
				COVER_DETAIL("SCOPE AIM: the held-back aim control was released");
			}
			return plan;
		}
		if (a_down && a_pressEdge && Locked()) {
			s_series = true;
			plan.swallow = true;
			{
				std::lock_guard lock(s_mutex);
				++s_info.locks;
			}
			COVER_DETAIL("SCOPE AIM: the aim press is held back: weapon 0x{:08X} has a scope (by {}), the pose is {}; no aim from cover", s_key, s_byVar ? "bHasScope" : "learning", CoverPose::ToString(CoverPose::GetInfo().state));
			if (!s_noticeShown) {
				s_noticeShown = true;
				s_noticeDue = true;
				std::lock_guard lock(s_mutex);
				++s_info.notices;
			}
		}
		return plan;
	}

	void Tick(const double a_now, const bool a_third, const bool a_aimHeld)
	{
		const auto& cfg = Config::Get();
		if (cfg.poseScopeAim != 2) {
			return;
		}
		if (a_now < s_nextPoll) {
			return;
		}
		s_nextPoll = a_now + 0.02;
		if (s_dirty.exchange(false)) {
			Engine::AudioProbe::FlushCache();  // before the mutex is taken: the writer reads PersistedKeys()
		}
		std::lock_guard lock(s_mutex);
		const bool      pose = PoseOn();
		if (!CoverPose::Busy()) {
			s_noticeShown = false;
		}
		const auto key = Engine::AudioProbe::CurrentWeapon();
		if (key != s_key) {
			s_prevKey = s_key;
			s_key = key;
			s_tKey = a_now;
		}
		// bHasScope and bInScopes by name: the weapon's own graph variables. Cached per weapon key, outside the pose only (the pose runs another graph) and not before the key has been the same
		// for 0.6 s (the graph may still hold the last weapon's value just after a switch)
		if (a_now >= s_nextRead) {
			s_nextRead = a_now + 0.1;
			const auto hs = PlayerControl::ReadVar("bHasScope");
			const auto is = PlayerControl::ReadVar("bInScopes");
			s_varReadable = hs.has_value();
			s_hasScope = hs ? *hs : -1.0;
			s_inScopes = is ? *is : -1.0;
			if (hs && key != 0 && !CoverPose::Busy() && a_now - s_tKey >= 0.6) {
				const bool v = *hs > 0.5;
				const auto c = s_cache.find(key);
				if (c == s_cache.end() || c->second != v) {
					COVER_DETAIL("SCOPE AIM: weapon 0x{:08X}: bHasScope reads {:g} (bInScopes {:g}) outside the pose: cached as {}", key, *hs, is ? *is : -1.0, v ? "scoped" : "no scope");
					s_cache[key] = v;
				}
			} else if (hs && key != 0 && pose) {
				const auto c = s_cache.find(key);
				if (c != s_cache.end() && c->second != (*hs > 0.5) && s_disagreeLogged.insert(key).second) {
					COVER_DETAIL("SCOPE AIM: weapon 0x{:08X}: in the pose bHasScope reads {:g} but the cache says {}", key, *hs, c->second ? "scoped" : "no scope");
				}
			}
		}
		// learning: the camera left third person while the aim control was held
		if (s_prevThird && !a_third && a_aimHeld && key != 0 && (cfg.scopeSource & 2) != 0 && s_learned.insert(key).second) {
			++s_info.learned;
			if (s_persisted.insert(key).second) {
				s_dirty = true;
			}
			COVER_DETAIL("SCOPE AIM: learned: the camera left third person while the aim control was held: weapon 0x{:08X} is marked scoped for this session", key);
		}
		s_prevThird = a_third;
		const auto src = cfg.scopeSource;
		const auto c = s_cache.find(key);
		s_byVar = (src & 1) != 0 && c != s_cache.end() && c->second;
		s_byLearn = (src & 2) != 0 && s_learned.contains(key);
		const bool scoped = key != 0 && (s_byVar || s_byLearn);
		const bool was = s_scopedNow.exchange(scoped);
		if (scoped != was) {
			COVER_DETAIL("SCOPE AIM: weapon 0x{:08X} is {} (by {}, bHasScope reads {:g}, pose {})", key, scoped ? "scoped" : "not scoped", s_byVar ? "bHasScope" : (s_byLearn ? "learning" : "nothing"), s_hasScope, CoverPose::ToString(CoverPose::GetInfo().state));
		}
		// a switch inside the pose to a scoped weapon while the aim is up: the aim ends at the switch (the tall pose's SightedEnd), never by the scope view later
		if (pose && key != 0 && s_prevKey != 0 && key != s_prevKey && s_tEnded != s_tKey && a_now - s_tKey < 0.5 && scoped) {
			s_tEnded = s_tKey;
			const auto ta = TallAim::GetInfo().state;
			const bool aiming = a_aimHeld || CoverPose::VarValue("iIsSighted") > 0.5 || CoverPose::VarValue("iSyncSighted") > 0.5 || ta == TallAim::State::Into || ta == TallAim::State::Aiming;
			COVER_DETAIL("SCOPE AIM: the weapon changed in the pose, 0x{:08X} -> 0x{:08X} (scoped): aim control {}, iIsSighted {:g}, iSyncSighted {:g}, tall aim {}{}", s_prevKey, key, a_aimHeld ? "held" : "up", CoverPose::VarValue("iIsSighted"),
				CoverPose::VarValue("iSyncSighted"), TallAim::ToString(ta), aiming ? ": the aim is ended now" : "");
			if (aiming) {
				++s_info.ends;
				TallAim::RequestEnd("a scoped weapon arrived while the aim was held");
			}
		}
	}
}
