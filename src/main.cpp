#include "Config.h"
#include "Engine/Frame.h"
#include "Engine/LoadWatch.h"
#include "Engine/Offsets.h"

namespace
{
	[[nodiscard]] constexpr std::string_view MessageName(const std::uint32_t a_type) noexcept
	{
		switch (a_type) {
		case SFSE::MessagingInterface::kPostLoad:
			return "PostLoad";
		case SFSE::MessagingInterface::kPostPostLoad:
			return "PostPostLoad";
		case SFSE::MessagingInterface::kPostDataLoad:
			return "PostDataLoad";
		case SFSE::MessagingInterface::kPostPostDataLoad:
			return "PostPostDataLoad";
		case Engine::SfseMessage::kPostLoadGame:
			return "PostLoadGame";
		default:
			return "unknown";
		}
	}

	// Logs every message SFSE sends us, so the log itself shows which ones this SFSE build delivers.
	// Phase 0 expects 0 to 3 and never 7 (see Engine/Offsets.h).
	void OnSfseMessage(SFSE::MessagingInterface::Message* a_msg)
	{
		if (a_msg) {
			REX::INFO("SFSE message {} ({}), dataLen={}", a_msg->type, MessageName(a_msg->type), a_msg->dataLen);
		}
	}
}

SFSE_PLUGIN_LOAD(const SFSE::LoadInterface* a_sfse)
{
	const auto runtime = a_sfse->RuntimeVersion();
	const bool supported = runtime == Engine::kPinnedRuntime;

	// Logging always starts so that a refusal is visible. The hook store only runs on the pinned runtime.
	// The log is named after the DLL (CoverSF.log, TacticalCover.log), not after the plugin's display name, which has a space in the release.
	SFSE::Init(a_sfse, { .logName = Config::BaseName().c_str(), .trampoline = supported, .trampolineSize = 512, .hook = supported });  // the trampoline is for the S4 call-site hook

	REX::INFO("{} {} loaded: game runtime {}, SFSE {}",
		SFSE::GetPluginName(), SFSE::GetPluginVersion(), runtime, REL::Version::unpack(SFSE::GetSFSEVersion()));

	if (!supported) {
		REX::ERROR("Unsupported game runtime {}: this build supports only {}. Nothing was installed; {} is inactive.",
			runtime, Engine::kPinnedRuntime, Config::DisplayName());
		return false;
	}

	if (const auto messaging = SFSE::GetMessagingInterface()) {
		if (!messaging->RegisterListener(OnSfseMessage)) {
			REX::WARN("Could not register the SFSE message listener");
		}
	} else {
		REX::WARN("SFSE messaging interface is unavailable");
	}

	Config::Load();

	if (const auto tasks = SFSE::GetTaskInterface()) {
		tasks->AddPermanentTask([]() {
			Engine::LoadWatch::Tick();
			Engine::Frame::Tick();
		});
	} else {
		REX::WARN("SFSE task interface is unavailable; the load watcher and the frame tick are not running");
	}

	return true;
}
