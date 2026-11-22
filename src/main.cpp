#include "PCH.h"

#include "PhotoBake.h"
#include "Hooks.h"
#include "IconData.h"
#include "Minimap.h"
#include "Settings.h"
#include "SettingsMenu.h"

namespace
{
	void SetupLog()
	{
		auto dir = logger::log_directory();
		if (!dir)
			return;
		auto path = *dir / "ModernMinimap.log";
		auto sink = std::make_shared<spdlog::sinks::basic_file_sink_mt>(path.string(), true);
		auto log = std::make_shared<spdlog::logger>("global", std::move(sink));
		log->set_level(spdlog::level::info);
		log->flush_on(spdlog::level::info);
		log->set_pattern("[%Y-%m-%d %H:%M:%S] [%l] %v");
		spdlog::set_default_logger(std::move(log));
	}

	void OnMessage(SKSE::MessagingInterface::Message* a_msg)
	{
		switch (a_msg->type) {
		case SKSE::MessagingInterface::kDataLoaded:
			MM::Minimap::SetMainThread();
			MM::Hooks::Install();
			MM::Hooks::RegisterInput();
			MM::IconData::RegisterEvents();
			MM::UI::Register();
			if (MM::Bake::Supported())
				MM::Bake::StartOnLoadIfRequested();
			break;
		case SKSE::MessagingInterface::kPreLoadGame:
		case SKSE::MessagingInterface::kPostLoadGame:
		case SKSE::MessagingInterface::kNewGame:
			MM::IconData::RequestFlush();
			break;
		default:
			break;
		}
	}
}

SKSE_PLUGIN_LOAD(const SKSE::LoadInterface* a_skse)
{
	SetupLog();
	SKSE::Init(a_skse, false);
	logger::info("Modern Minimap {} loading", SKSE::PluginDeclaration::GetSingleton()->GetVersion().string());

	MM::Settings::Load();
	if (!MM::Settings::Get()->enabled)
		logger::info("disabled in ini (can be turned on from the settings menu)");
	if (MM::Bake::Supported())
		MM::Bake::InstallActorHooksIfRequested();  // before any plugin record is read
	else
		logger::info("Skyrim VR: experimental build, town photo bake off");
	SKSE::GetMessagingInterface()->RegisterListener(OnMessage);
	return true;
}
