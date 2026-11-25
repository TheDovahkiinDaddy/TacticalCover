#include "Engine/AudioProbe.h"

#include "Config.h"
#include "Core/PluginNames.h"
#include "Core/ReloadLearn.h"
#include "Engine/CoverPose.h"
#include "Engine/Frame.h"
#include "Engine/Guard.h"
#include "Engine/Offsets.h"
#include "Engine/PlayerControl.h"
#include "Engine/ScopeAim.h"

#include <intrin.h>
#include <algorithm>
#include <fstream>
#include <map>
#include <mutex>
#include <sstream>

namespace Engine::AudioProbe
{
	namespace
	{
#include "Engine/ReloadFoleyIds.inc"

		using ById_t = std::uint32_t (*)(std::uint32_t, std::uint64_t, std::uint32_t, void*, void*, std::uint32_t, void*, std::uint32_t);
		using ByName_t = std::uint32_t (*)(const char*, std::uint64_t, std::uint32_t, void*, void*, std::uint32_t, void*, std::uint32_t);

		ById_t                     s_origId{ nullptr };
		ByName_t                   s_origName{ nullptr };
		std::atomic<bool>          s_installed{ false };
		std::atomic<bool>          s_tried{ false };
		std::atomic<bool>          s_armed{ false };
		std::atomic<bool>          s_faulted{ false };
		std::atomic<int>           s_sites{ 0 };
		std::atomic<std::uint64_t> s_calls{ 0 };
		std::atomic<std::uint64_t> s_logged{ 0 };
		std::atomic<std::uint64_t> s_windowStart{ 0 };
		std::atomic<int>           s_windowCount{ 0 };
		std::atomic<int>           s_learned{ 0 };
		std::atomic<int>           s_replays{ 0 };
		std::atomic<int>           s_posted{ 0 };
		std::atomic<int>           s_cancelled{ 0 };
		std::atomic<int>           s_ended{ 0 };

		// every post by id that the engine makes, kept for a few seconds (the learning reads it from the frame tick)
		struct Rec
		{
			double        t;
			std::uint32_t id;
			std::uint64_t obj;
			std::uint32_t flags;
		};
		std::mutex      s_ringMutex;
		std::deque<Rec> s_ring;

		[[nodiscard]] std::uintptr_t ModuleBase()
		{
			return REX::FModule::GetExecutingModule().GetBaseAddress();
		}

		[[nodiscard]] bool WantProbe() { return Config::Get().audioProbe != 0 || Config::Get().poseReloadSound != 0; }

		[[nodiscard]] bool ShouldLog()
		{
			const int mode = Config::Get().audioProbe;
			if (mode == 0) {
				return false;
			}
			if (mode == 1) {
				return s_armed.load();
			}
			// mode 2: at most 60 lines per second
			const auto sec = static_cast<std::uint64_t>(Frame::Now());
			if (s_windowStart.exchange(sec) != sec) {
				s_windowCount = 0;
			}
			return s_windowCount.fetch_add(1) < 60;
		}

		void LogCall(const std::uintptr_t a_ret, const char* a_name, const std::uint32_t a_id, const std::uint64_t a_obj, const std::uint32_t a_flags, void* a_cb, const std::uint32_t a_ext, const std::uint32_t a_pid, const std::uint32_t a_res)
		{
			char text[100]{};
			if (a_name) {
				const auto n = SafeCopy(text, a_name, sizeof(text) - 1);
				if (n == 0) {
					std::memcpy(text, "<unreadable>", 13);
				}
				text[sizeof(text) - 1] = 0;
			}
			const auto n = ++s_logged;
			if (a_name) {
				COVER_DETAIL("AUDIO #{} t={:.3f} site RVA 0x{:X} BY NAME '{}' obj=0x{:X} flags=0x{:X} cb={} externals={} playingId={} -> {}", n, Frame::Now(), a_ret - ModuleBase(), text, a_obj, a_flags, a_cb, a_ext, a_pid, a_res);
			} else {
				COVER_DETAIL("AUDIO #{} t={:.3f} site RVA 0x{:X} BY ID {} (0x{:08X}) obj=0x{:X} flags=0x{:X} cb={} externals={} playingId={} -> {}", n, Frame::Now(), a_ret - ModuleBase(), a_id, a_id, a_obj, a_flags, a_cb, a_ext, a_pid, a_res);
			}
		}

		std::uint32_t HookById(const std::uint32_t a_id, const std::uint64_t a_obj, const std::uint32_t a_flags, void* a_cb, void* a_cookie, const std::uint32_t a_ext, void* a_src, const std::uint32_t a_pid)
		{
			const auto ret = reinterpret_cast<std::uintptr_t>(_ReturnAddress());
			const auto res = s_origId(a_id, a_obj, a_flags, a_cb, a_cookie, a_ext, a_src, a_pid);
			++s_calls;
			{
				std::lock_guard lk(s_ringMutex);
				s_ring.push_back({ Frame::Now(), a_id, a_obj, a_flags });
				while (s_ring.size() > 600 || (!s_ring.empty() && s_ring.front().t < Frame::Now() - 12.0)) {
					s_ring.pop_front();
				}
			}
			if (ShouldLog()) {
				LogCall(ret, nullptr, a_id, a_obj, a_flags, a_cb, a_ext, a_pid, res);
			}
			return res;
		}

		std::uint32_t HookByName(const char* a_name, const std::uint64_t a_obj, const std::uint32_t a_flags, void* a_cb, void* a_cookie, const std::uint32_t a_ext, void* a_src, const std::uint32_t a_pid)
		{
			const auto ret = reinterpret_cast<std::uintptr_t>(_ReturnAddress());
			const auto res = s_origName(a_name, a_obj, a_flags, a_cb, a_cookie, a_ext, a_src, a_pid);
			++s_calls;
			if (ShouldLog()) {
				LogCall(ret, a_name, 0, a_obj, a_flags, a_cb, a_ext, a_pid, res);
			}
			return res;
		}

		// the five bytes at fn + a_off must be `call rel32` to a_expect
		[[nodiscard]] bool CheckSite(const REL::ID a_fn, const std::uint32_t a_off, const std::uintptr_t a_expectRva)
		{
			const auto                  site = a_fn.address() + a_off;
			std::array<std::uint8_t, 5> b{};
			if (SafeCopy(b.data(), reinterpret_cast<const void*>(site), b.size()) != b.size()) {
				REX::ERROR("AUDIO: cannot read the five bytes at ID {} + 0x{:X}", a_fn.id(), a_off);
				return false;
			}
			std::int32_t rel{ 0 };
			std::memcpy(&rel, b.data() + 1, sizeof(rel));
			const auto target = site + 5 + static_cast<std::intptr_t>(rel);
			const bool ok = b[0] == 0xE8 && target - ModuleBase() == a_expectRva;
			COVER_DETAIL("AUDIO: site ID {} + 0x{:X} (RVA 0x{:X}): {:02X} {:02X} {:02X} {:02X} {:02X} -> call target RVA 0x{:X} (expected 0x{:X}): {}", a_fn.id(), a_off, site - ModuleBase(), b[0], b[1], b[2], b[3], b[4], target - ModuleBase(), a_expectRva, ok ? "ok" : "REFUSED");
			return ok;
		}

		template <std::uint32_t Off>
		void HookNameSite(const REL::ID a_fn)
		{
			if (!CheckSite(a_fn, Off, kAkPostEventByNameRva)) {
				return;
			}
			REL::Relocation<std::uintptr_t> r{ a_fn };
			(void)r.write_call<5, Off>(reinterpret_cast<std::uintptr_t>(&HookByName));
			++s_sites;
		}

		template <std::uint32_t Off>
		void HookIdSite(const REL::ID a_fn)
		{
			if (!CheckSite(a_fn, Off, kAkPostEventRva)) {
				return;
			}
			REL::Relocation<std::uintptr_t> r{ a_fn };
			(void)r.write_call<5, Off>(reinterpret_cast<std::uintptr_t>(&HookById));
			++s_sites;
		}

		// ---- the cure: learn the engine's reload sequence, replay it in the pose -------------------------------------------------------------------

		using Event = Core::ReloadEvent;  // id, seconds after the reload began
		struct Learned
		{
			std::uint32_t weapon{ 0 };
			double        plainDur{ 0.0 };   // from the whole reloads of the history (Core/ReloadLearn.h), never from a cut one
			double        poseDurLow{ 0.0 };
			double        poseDurTall{ 0.0 };
			std::vector<Event>              ev;    // the replay list: what recurs in the history
			std::vector<Core::ReloadLearn>  hist;  // the last learns of this weapon
		};
		std::vector<Learned> s_seqs;
		std::uint64_t        s_playerObj{ 0 };  // the game object the engine posts the player's reload on (found from a whitelisted event)
		std::uint32_t        s_postFlags{ 0x1 };  // AK_EndOfEvent: the plugin's own posts report their end

		struct Replay
		{
			bool                active{ false };
			double              t0{ 0.0 };
			double              dur{ 0.0 };
			std::uint32_t       weapon{ 0 };
			bool                tall{ false };
			std::size_t         next{ 0 };
			std::vector<Event>  plan;  // dt already scaled to the pose reload
		};
		Replay s_replay;

		struct Plain
		{
			bool          active{ false };
			double        t0{ 0.0 };
			std::uint32_t weapon{ 0 };
		};
		Plain s_plain;

		bool s_prevReloading{ false };

		[[nodiscard]] bool Whitelisted(const std::uint32_t a_id)
		{
			return std::binary_search(std::begin(kReloadFoleyIds), std::end(kReloadFoleyIds), a_id);
		}

		[[nodiscard]] Learned* Find(std::uint32_t a_weapon);

		[[nodiscard]] std::filesystem::path CachePath() { return Config::Directory() / Core::ReloadSoundFileName(Config::BaseName()); }  // CoverSF.reloadsound.txt, TacticalCover.reloadsound.txt

		void SaveCache()
		{
			if (Config::Get().poseReloadSoundFile == 0) {
				return;
			}
			// written beside the file and renamed over it, so that a game that dies in the middle of the write leaves the old file whole (23.23: a crash truncated it once)
			const auto tmpPath = std::filesystem::path(CachePath().string() + ".tmp");
			std::ofstream f(tmpPath, std::ios::trunc);
			if (!f) {
				REX::WARN("AUDIO: the reload sound cache {} cannot be written", CachePath().string());
				return;
			}
			f << "# " << Config::DisplayName() << " reload sound cache (23.13, 23.18): per weapon a line: weapon form id, plain reload seconds, pose seconds low, pose seconds tall, then the replay list as event id and seconds pairs;\n";
			f << "# then its learns, lines 'h' + weapon form id + the reload's length + the events as id and seconds pairs (the replay list is what recurs in them)\n";
			if (s_playerObj != 0) {
				f << "player " << std::hex << s_playerObj << std::dec << '\n';
			}
			f << "# 's' + weapon form id: a weapon with a scope (23.23): the aim is locked in the cover pose for it\n";
			for (const auto k : ScopeAim::PersistedKeys()) {
				f << "s " << std::hex << k << std::dec << '\n';
			}
			for (const auto& l : s_seqs) {
				if (l.weapon == 0) {
					continue;  // a sequence without a weapon key belongs to no weapon: never written
				}
				f << std::hex << l.weapon << std::dec << ' ' << l.plainDur << ' ' << l.poseDurLow << ' ' << l.poseDurTall << ' ' << l.ev.size();
				for (const auto& e : l.ev) {
					f << ' ' << e.id << ' ' << e.dt;
				}
				f << '\n';
				for (const auto& h : l.hist) {
					f << "h " << std::hex << l.weapon << std::dec << ' ' << h.dur << ' ' << h.ev.size();
					for (const auto& e : h.ev) {
						f << ' ' << e.id << ' ' << e.dt;
					}
					f << '\n';
				}
			}
			const bool good = f.good();
			f.close();
			std::error_code ec;
			if (good) {
				std::filesystem::rename(tmpPath, CachePath(), ec);
			}
			if (!good || ec) {
				REX::WARN("AUDIO: the reload sound cache {} could not be replaced ({}): the old file stays", CachePath().string(), good ? ec.message() : std::string("the write failed"));
				std::filesystem::remove(tmpPath, ec);
			}
		}

		void LoadCache()
		{
			if (Config::Get().poseReloadSoundFile == 0) {
				return;
			}
			std::ifstream f(CachePath());
			if (!f) {
				return;
			}
			std::string line;
			while (std::getline(f, line)) {
				if (line.empty() || line[0] == '#') {
					continue;
				}
				if (line.rfind("player ", 0) == 0) {
					std::istringstream pin(line.substr(7));
					std::uint64_t      obj{ 0 };
					pin >> std::hex >> obj;
					if (obj != 0 && s_playerObj == 0) {
						s_playerObj = obj;
						COVER_DETAIL("AUDIO: the player's game object 0x{:X} is read from the cache file", obj);
					}
					continue;
				}
				if (line.rfind("s ", 0) == 0) {
					std::istringstream sin(line.substr(2));
					std::uint32_t      k{ 0 };
					sin >> std::hex >> k;
					if (k != 0) {
						ScopeAim::LoadPersisted(k);
					}
					continue;
				}
				if (line.rfind("h ", 0) == 0) {
					std::istringstream hin(line.substr(2));
					std::uint32_t      w{ 0 };
					Core::ReloadLearn  h;
					std::size_t        hn{ 0 };
					hin >> std::hex >> w >> std::dec >> h.dur >> hn;
					for (std::size_t i = 0; i < hn && hin; ++i) {
						Event e{};
						hin >> e.id >> e.dt;
						h.ev.push_back(e);
					}
					if (w != 0 && h.dur > 0.5 && (hin || hin.eof())) {
						if (auto* lw = Find(w)) {
							Core::AddReloadLearn(lw->hist, std::move(h));
						}
					}
					continue;
				}
				std::istringstream in(line);
				Learned            l;
				std::size_t        n{ 0 };
				in >> std::hex >> l.weapon >> std::dec >> l.plainDur >> l.poseDurLow >> l.poseDurTall >> n;
				for (std::size_t i = 0; i < n && in; ++i) {
					Event e{};
					in >> e.id >> e.dt;
					l.ev.push_back(e);
				}
				if (in || in.eof()) {
					if (!l.ev.empty() && l.plainDur > 0.5 && l.weapon != 0) {  // a line without a weapon key (the first test file of 2026-11-19 00:07) is not read as some weapon's
						s_seqs.push_back(std::move(l));
					}
				}
			}
			for (auto& l : s_seqs) {
				if (l.hist.empty()) {
					// an old-format file: the entry is one learn (the next ordinary reload takes its strays out)
					l.hist.push_back({ l.plainDur, l.ev });
				} else {
					const auto seq = Core::ResolveReload(l.hist);
					if (!seq.events.empty() && seq.plainDur > 0.5) {
						l.ev = seq.events;
						l.plainDur = seq.plainDur;
					}
				}
			}
			s_learned = static_cast<int>(s_seqs.size());
			COVER_DETAIL("AUDIO: the reload sound cache holds {} weapon(s)", s_seqs.size());
		}

		[[nodiscard]] Learned* Find(const std::uint32_t a_weapon)
		{
			for (auto& l : s_seqs) {
				if (l.weapon == a_weapon) {
					return &l;
				}
			}
			return nullptr;
		}

		std::atomic<std::uint32_t> s_equipped{ 0 };       // the form id of the weapon the player equipped last (ActorItemEquipped); 0 until one was seen
		std::atomic<int>           s_equipEvents{ 0 };
		std::vector<std::uint32_t> s_silentLogged;        // weapons for which "no sequence learned" was said already

		class EquipSink : public RE::BSTEventSink<RE::ActorItemEquipped::Event>
		{
		public:
			RE::BSEventNotifyControl ProcessEvent(const RE::ActorItemEquipped::Event& a_event, RE::BSTEventSource<RE::ActorItemEquipped::Event>*) override
			{
				unsigned long code{ 0 };
				(void)Guarded([&] {
					const auto player = RE::PlayerCharacter::GetSingleton();
					if (!player || !a_event.item || a_event.actor.get() != static_cast<RE::Actor*>(player)) {
						return;
					}
					const auto type = a_event.item->GetFormType();
					const auto id = a_event.item->GetFormID();
					++s_equipEvents;
					if (type == RE::FormType::kWEAP) {
						const auto old = s_equipped.exchange(id);
						COVER_DETAIL("AUDIO equip: the player equipped weapon 0x{:08X} (before: 0x{:08X}); it is the key of the reload sequences", id, old);
					} else {
						COVER_DETAIL("AUDIO equip: the player equipped item 0x{:08X} of form type {} (not a weapon: the key stays 0x{:08X})", id, static_cast<int>(type), s_equipped.load());
					}
				}, code);
				if (code != 0) {
					REX::ERROR("AUDIO equip: exception 0x{:08X} in the equip event sink", code);
				}
				return RE::BSEventNotifyControl::kContinue;
			}
		};
		EquipSink s_equipSink;

		// the weapon the player's process holds (MiddleHighProcessData::lastBoundWeapon, offset 0x450 by CommonLibSF, not yet verified on 1.16.244: the log shows whether the key is stable; the form id is the key, 0 when unreadable)
		[[nodiscard]] std::uint32_t LegacyWeaponKey()
		{
			const auto player = RE::PlayerCharacter::GetSingleton();
			if (!player) {
				return 0;
			}
			RE::AIProcess* proc{ nullptr };
			if (!SafeRead(&player->currentProcess, proc) || !proc) {
				return 0;
			}
			RE::MiddleHighProcessData* mh{ nullptr };
			if (!SafeRead(&proc->middleHigh, mh) || !mh) {
				return 0;
			}
			RE::TESObjectWEAP* w{ nullptr };
			if (!SafeRead(&mh->lastBoundWeapon, w) || !w) {
				return 0;
			}
			std::uint32_t id{ 0 };
			if (!SafeRead(&w->formID, id)) {
				return 0;
			}
			return id;
		}

		[[nodiscard]] bool AsForm(std::uint64_t a_q, std::uint32_t& a_id, int& a_type);  // below

		// the weapon in the hands read from the process data (Offsets.h kMhEquippedItemsData), 0 when it is not found
		[[nodiscard]] std::uint32_t DirectWeaponKey()
		{
			const auto player = RE::PlayerCharacter::GetSingleton();
			if (!player) {
				return 0;
			}
			RE::AIProcess* proc{ nullptr };
			if (!SafeRead(&player->currentProcess, proc) || !proc) {
				return 0;
			}
			RE::MiddleHighProcessData* mh{ nullptr };
			if (!SafeRead(&proc->middleHigh, mh) || !mh) {
				return 0;
			}
			std::uint64_t data{ 0 };
			if (!SafeRead(reinterpret_cast<const std::byte*>(mh) + kMhEquippedItemsData, data) || data < 0x10000) {
				return 0;
			}
			for (std::size_t off = 0; off < kEquippedItemsScanBytes; off += 8) {
				std::uint64_t q{ 0 };
				std::uint32_t id{ 0 };
				int           type{ 0 };
				if (SafeRead(reinterpret_cast<const void*>(data + off), q) && AsForm(q, id, type) && type == static_cast<int>(RE::FormType::kWEAP)) {
					return id;
				}
			}
			return 0;
		}

		// the key of the learned sequences: the weapon in the hands (process data), else the weapon of the last equip event, else 0 (one bucket, never written to the file)
		[[nodiscard]] std::uint32_t WeaponKey()
		{
			unsigned long code{ 0 };
			std::uint32_t k{ 0 };
			if (!Guarded([&] { k = DirectWeaponKey(); }, code)) {
				k = 0;
			}
			return k != 0 ? k : s_equipped.load();
		}

		void FinishPlain(const double a_now)
		{
			const double dur = a_now - s_plain.t0;
			std::vector<Rec> recs;
			{
				std::lock_guard lk(s_ringMutex);
				for (const auto& r : s_ring) {
					if (r.t >= s_plain.t0 - 4.0 && r.t <= a_now + 0.3) {
						recs.push_back(r);
					}
				}
			}
			{
				for (const auto& r : recs) {
					if (r.t >= s_plain.t0 - 0.3 && Whitelisted(r.id)) {
						if (s_playerObj != r.obj) {
							COVER_DETAIL("AUDIO learn: the player's game object is now 0x{:X} (was 0x{:X})", r.obj, s_playerObj);
						}
						s_playerObj = r.obj;
						COVER_DETAIL("AUDIO learn: the player's game object is 0x{:X} (the engine posted the reload event {} (0x{:08X}) on it)", r.obj, r.id, r.id);
						break;
					}
				}
			}
			if (s_playerObj == 0) {
				COVER_DETAIL("AUDIO learn: the reload ran {:.2f} s and no event of the reload table was posted: nothing learned", dur);
				return;
			}
			std::vector<std::uint32_t> before;
			for (const auto& r : recs) {
				if (r.obj == s_playerObj && r.t < s_plain.t0 - 0.15) {
					before.push_back(r.id);
				}
			}
			Core::ReloadLearn learn;
			learn.dur = dur;
			for (const auto& r : recs) {
				if (r.obj != s_playerObj || r.t < s_plain.t0 - 0.15 || r.t > a_now + 0.3) {
					continue;
				}
				if (r.flags != 0x100009) {
					continue;  // the engine's own posting style for animation events
				}
				if (!Whitelisted(r.id) && std::find(before.begin(), before.end(), r.id) != before.end()) {
					continue;  // something the player's object posts all the time
				}
				learn.ev.push_back({ r.id, (std::max)(0.0, r.t - s_plain.t0) });
			}
			if (learn.ev.empty() || dur < 1.0) {
				COVER_DETAIL("AUDIO learn: weapon 0x{:08X}, the reload ran {:.2f} s, {} event(s): nothing kept", s_plain.weapon, dur, learn.ev.size());
				return;
			}
			std::string thisOne;
			for (const auto& e : learn.ev) {
				thisOne += std::format(" {}@{:.2f}", e.id, e.dt);
			}
			Learned* entry = Find(s_plain.weapon);
			if (!entry) {
				s_seqs.push_back({});
				entry = &s_seqs.back();
				entry->weapon = s_plain.weapon;
			}
			Core::AddReloadLearn(entry->hist, std::move(learn));
			const auto seq = Core::ResolveReload(entry->hist);
			entry->plainDur = seq.plainDur;
			entry->ev = seq.events;
			s_learned = static_cast<int>(s_seqs.size());
			std::string kept;
			for (const auto& e : entry->ev) {
				kept += std::format(" {}@{:.2f}", e.id, e.dt);
			}
			COVER_DETAIL("AUDIO learn: weapon 0x{:08X} (equip event key 0x{:08X}): this plain reload of {:.2f} s posted{} on object 0x{:X}; {} learn(s) of the weapon in all; the replay list is now {} event(s) over a plain length of {:.2f} s:{}", entry->weapon, s_equipped.load(), dur, thisOne, s_playerObj, entry->hist.size(), entry->ev.size(), entry->plainDur, kept);
			SaveCache();
		}

		void EndCb(const std::uint32_t a_type, void* a_info)
		{
			// AK callback: type 1 = AK_EndOfEvent; the info is {cookie, gameObjectID} then playingID and eventID
			std::uint32_t pid{ 0 }, eid{ 0 };
			(void)SafeRead(static_cast<const std::byte*>(a_info) + 16, pid);
			(void)SafeRead(static_cast<const std::byte*>(a_info) + 20, eid);
			++s_ended;
			COVER_DETAIL("AUDIO END t={:.3f} callback type {}: playingId {} event {} (0x{:08X}) ended", Frame::Now(), a_type, pid, eid, eid);
		}

		[[nodiscard]] std::uint32_t PostOwn(const std::uint32_t a_id, const std::uint64_t a_obj, const std::size_t a_index)
		{
			std::uint32_t res{ 0 };
			unsigned long code{ 0 };
			if (!Guarded([&] { res = s_origId(a_id, a_obj, s_postFlags, reinterpret_cast<void*>(&EndCb), reinterpret_cast<void*>(a_index + 1), 0, nullptr, 0); }, code)) {
				s_faulted = true;
				REX::ERROR("AUDIO: exception 0x{:08X} while posting event {}: the reload sound is off for this session", code, a_id);
				return 0;
			}
			return res;
		}

		void CancelReplay(const double a_now, const char* a_why)
		{
			if (!s_replay.active) {
				return;
			}
			++s_cancelled;
			COVER_DETAIL("AUDIO replay cancelled {:.2f} s into the reload ({}): {} of {} event(s) had been posted", a_now - s_replay.t0, a_why, s_replay.next, s_replay.plan.size());
			s_replay.active = false;
		}

		void Install()
		{
			COVER_DETAIL("AUDIO: installing the Wwise post hooks (iAudioProbe {}, iPoseReloadSound {})", Config::Get().audioProbe, Config::Get().poseReloadSound);
			// the forward pointers first: a hooked site must never run with an empty one (both targets were checked by CheckSite)
			s_origId = reinterpret_cast<ById_t>(ModuleBase() + kAkPostEventRva);
			s_origName = reinterpret_cast<ByName_t>(ModuleBase() + kAkPostEventByNameRva);
			HookNameSite<0x236>(kAudioWrapperAId);
			HookNameSite<0x2CE>(kAudioWrapperAId);
			HookIdSite<0x2FD>(kAudioWrapperAId);
			HookNameSite<0x1A4>(kAudioWrapperBId);
			HookIdSite<0x1CB>(kAudioWrapperBId);
			s_installed = s_sites.load() > 0;
			REX::INFO("AUDIO: {} of 5 call sites hooked", s_sites.load());
			if (const auto src = RE::ActorItemEquipped::Event::GetEventSource()) {
				src->RegisterSink(&s_equipSink);
				COVER_DETAIL("AUDIO: the equip event sink is registered (the weapon key)");
			} else {
				REX::WARN("AUDIO: no equip event source: the weapon key stays 0");
			}
			LoadCache();
		}
	}

	void Tick()
	{
		if (s_tried.load() || !WantProbe() || !Config::Get().enabled) {
			return;
		}
		s_tried = true;
		unsigned long code{ 0 };
		if (!Guarded([] { Install(); }, code)) {
			REX::ERROR("AUDIO: exception 0x{:08X} while installing the hooks", code);
			s_faulted = true;
		}
	}

	void OnTick(const double a_now)
	{
		if (!s_installed.load() || s_faulted.load() || Config::Get().poseReloadSound == 0) {
			return;
		}
		unsigned long code{ 0 };
		const bool ok = Guarded([&] {
			const auto   rv = PlayerControl::ReadVar("bIsReloading");
			const bool   reloading = rv && *rv > 0.5;
			const bool   posed = CoverPose::GetInfo().state == CoverPose::State::Posed;
			const bool   rising = reloading && !s_prevReloading;
			const bool   falling = !reloading && s_prevReloading;
			s_prevReloading = reloading;

			if (rising) {
				const auto weapon = WeaponKey();
				if (!posed) {
					s_plain = { true, a_now, weapon };
					CancelReplay(a_now, "a plain reload began");
				} else if (const auto* l = Find(weapon); l && s_playerObj != 0) {
					const bool   tall = CoverPose::StanceField() == 5;
					const double known = tall ? l->poseDurTall : l->poseDurLow;
					const double dur = known > 0.5 ? known : l->plainDur * (tall ? 0.87 : 0.67);
					s_replay = {};
					s_replay.active = true;
					s_replay.t0 = a_now;
					s_replay.dur = dur;
					s_replay.weapon = weapon;
					s_replay.tall = tall;
					std::string desc;
					for (const auto& e : l->ev) {
						// plain proportion: the HUD's ammunition count changed 3.9 s into the 5.1 s plain reload (the BoltClose event, 75 %) and about 2.4 s into the 3.43 s pose reload (70 %) in the m4c recording; run 3 used a stretch of 1.10 (events 0.2 to 0.4 s later than that marker), this is the same without the stretch (not run)
						const double f = (std::min)(0.95, e.dt / l->plainDur);
						s_replay.plan.push_back({ e.id, f * dur });
						desc += std::format(" {}@{:.2f}", e.id, f * dur);
					}
					++s_replays;
					COVER_DETAIL("AUDIO replay: a reload began in the {} pose, weapon 0x{:08X}: {} event(s) planned over {:.2f} s ({}) on object 0x{:X}:{}", tall ? "tall" : "low", weapon, s_replay.plan.size(), dur, known > 0.5 ? "the last pose reload's length" : "estimated from the plain reload", s_playerObj, desc);
				} else {
					if (std::find(s_silentLogged.begin(), s_silentLogged.end(), weapon) == s_silentLogged.end()) {
						s_silentLogged.push_back(weapon);
						COVER_DETAIL("AUDIO replay: a reload began in the pose, weapon 0x{:08X}: no sequence learned for it (reload once outside cover first): silent (said once for this weapon)", weapon);
					}
				}
			}

			if (s_replay.active) {
				if (!posed) {
					CancelReplay(a_now, "the pose ended");
				} else if (const auto nowKey = WeaponKey(); nowKey != 0 && s_replay.weapon != 0 && nowKey != s_replay.weapon) {
					CancelReplay(a_now, "the weapon changed (W4)");  // REFERENCE 23.22: a switch inside a reload ends the sound of the old weapon's reload
				} else if (falling) {
					// the reload is over: keep its length for the next one; whatever was not posted yet is dropped
					const double len = a_now - s_replay.t0;
					const bool   complete = s_replay.next >= s_replay.plan.size();
					if (auto* l = Find(s_replay.weapon); l && len > 1.0 && complete) {
						(s_replay.tall ? l->poseDurTall : l->poseDurLow) = len;
						SaveCache();
					}
					if (complete) {
						COVER_DETAIL("AUDIO replay done: all {} event(s) posted, the reload ran {:.2f} s", s_replay.plan.size(), len);
						s_replay.active = false;
					} else {
						CancelReplay(a_now, "the reload ended first");
					}
				} else {
					while (s_replay.next < s_replay.plan.size() && a_now - s_replay.t0 >= s_replay.plan[s_replay.next].dt) {
						const auto& e = s_replay.plan[s_replay.next];
						const auto  res = PostOwn(e.id, s_playerObj, s_replay.next);
						++s_posted;
						COVER_DETAIL("AUDIO replay: posted event {} (0x{:08X}) on object 0x{:X} {:.3f} s into the reload (planned {:.3f} s): playing id {}", e.id, e.id, s_playerObj, a_now - s_replay.t0, e.dt, res);
						++s_replay.next;
					}
				}
			}

			if (s_plain.active && falling && !posed) {
				s_plain.active = false;
				FinishPlain(a_now);
			} else if (s_plain.active && falling) {
				s_plain.active = false;
			}
		}, code);
		if (!ok) {
			s_faulted = true;
			REX::ERROR("AUDIO: exception 0x{:08X} in the reload sound tick: it is off for this session", code);
		}
	}

	namespace
	{
		struct ScanHit
		{
			std::string   path;
			std::uint32_t id;
			int           type;
		};

		[[nodiscard]] bool PlausiblePtr(const std::uint64_t a_q) { return a_q > 0x10000 && a_q < 0x00007FFFFFFFFFFFull && (a_q & 7) == 0; }

		// a_q points at a form: formID at +0x28, formType at +0x2E, and the game's own lookup of that id returns the same pointer
		bool AsForm(const std::uint64_t a_q, std::uint32_t& a_id, int& a_type)
		{
			std::uint32_t id{ 0 };
			std::uint8_t  type{ 0 };
			if (!SafeRead(reinterpret_cast<const void*>(a_q + 0x28), id) || id == 0 || !SafeRead(reinterpret_cast<const void*>(a_q + 0x2E), type) || type > 0xA0) {
				return false;
			}
			if (RE::TESForm::LookupByID(id) != reinterpret_cast<RE::TESForm*>(a_q)) {
				return false;
			}
			a_id = id;
			a_type = type;
			return true;
		}

		void ScanRegion(const std::uint64_t a_base, const std::size_t a_bytes, const std::string& a_path, const int a_depth, std::vector<ScanHit>& a_out, std::map<int, int>& a_types)
		{
			for (std::size_t off = 0; off < a_bytes; off += 8) {
				std::uint64_t q{ 0 };
				if (!SafeRead(reinterpret_cast<const void*>(a_base + off), q) || !PlausiblePtr(q)) {
					continue;
				}
				std::uint32_t id{ 0 };
				int           type{ 0 };
				const auto    here = a_path + std::format("+0x{:X}", off);
				if (AsForm(q, id, type)) {
					++a_types[type];
					if (type == static_cast<int>(RE::FormType::kWEAP)) {
						a_out.push_back({ here, id, type });
					}
				} else if (a_depth > 0) {
					ScanRegion(q, a_depth >= 2 ? 0x700 : 0x100, here + "->", a_depth - 1, a_out, a_types);
				}
			}
		}
	}

	void ScanWeapon(const char* a_label)
	{
		unsigned long code{ 0 };
		const bool    ok = Guarded([&] {
			const auto player = RE::PlayerCharacter::GetSingleton();
			if (!player) {
				return;
			}
			std::vector<ScanHit> hits;
			std::map<int, int>   types;
			RE::AIProcess*       proc{ nullptr };
			if (SafeRead(&player->currentProcess, proc) && proc) {
				ScanRegion(reinterpret_cast<std::uint64_t>(proc), 0x800, "proc", 2, hits, types);
			}
			ScanRegion(reinterpret_cast<std::uint64_t>(player), 0x1000, "player", 1, hits, types);
			std::string all;
			for (const auto& [t, n] : types) {
				all += std::format(" type {}: {}", t, n);
			}
			COVER_DETAIL("AUDIO scan [{}]: {} weapon pointer(s) found; pointers to forms by type:{}", a_label, hits.size(), all);
			for (const auto& h : hits) {
				COVER_DETAIL("AUDIO scan [{}]: weapon 0x{:08X} at {}", a_label, h.id, h.path);
			}
		}, code);
		if (!ok) {
			REX::ERROR("AUDIO scan [{}]: exception 0x{:08X}", a_label, code);
		}
	}

	std::uint32_t CurrentWeapon() { return WeaponKey(); }

	void FlushCache()
	{
		if (s_installed.load()) {
			SaveCache();
		}
	}

	void DebugForget()
	{
		// rig only: what a fresh start does before the first reload: memory empty, then the file read
		s_seqs.clear();
		s_playerObj = 0;
		s_silentLogged.clear();
		s_learned = 0;
		s_replay = {};
		COVER_DETAIL("AUDIO rig: the memory is cleared as at a fresh start; the file is read again");
		LoadCache();
		COVER_DETAIL("AUDIO rig: after the file: {} weapon(s), the player's object 0x{:X}", s_seqs.size(), s_playerObj);
	}

	void Arm(const bool a_on, const char* a_label)
	{
		s_armed = a_on;
		COVER_DETAIL("AUDIO probe {}: {} (calls so far {})", a_on ? "ARMED" : "disarmed", a_label ? a_label : "", s_calls.load());
	}

	Info GetInfo() noexcept
	{
		return { s_installed.load(), s_sites.load(), s_calls.load(), s_logged.load(), s_learned.load(), s_replays.load(), s_posted.load(), s_cancelled.load(), s_ended.load(), s_faulted.load(), s_equipped.load(), s_equipEvents.load() };
	}
}
