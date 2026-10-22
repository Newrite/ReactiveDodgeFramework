#include "game/ActionCapture.h"
#include "game/Api.h"
#include "game/Effects.h"
#include "game/GraphCoordinator.h"
#include "game/GraphGate.h"
#include "game/InputCollector.h"
#include "game/JumpGate.h"
#include "game/LegacyDodge.h"
#include "game/LockOn.h"
#include "game/Menu.h"
#include "game/MotionHook.h"
#include "game/Settings.h"
#include "game/UpdateHook.h"

namespace
{
	void SetupLog()
	{
		auto path = logger::log_directory();
		if (!path) {
			return;
		}
		*path /= "ReactiveDodgeFramework.log"sv;

		auto sink = std::make_shared<spdlog::sinks::basic_file_sink_mt>(path->string(), true);
		auto log = std::make_shared<spdlog::logger>("global log"s, std::move(sink));
		log->set_level(spdlog::level::info);
		log->flush_on(spdlog::level::info);
		spdlog::set_default_logger(std::move(log));
		spdlog::set_pattern("[%H:%M:%S.%e] [%l] %v"s);
	}

	void OnNewSession()
	{
		rdf::game::update::Reset();
		rdf::game::graph::RegisterSink(RE::PlayerCharacter::GetSingleton());
		rdf::game::legacy::Suppress();
	}

	void OnMessage(SKSE::MessagingInterface::Message* message)
	{
		switch (message->type) {
		case SKSE::MessagingInterface::kPostPostLoad:
			rdf::game::lockon::Init();
			if (rdf::game::Settings::Get().iFrames && rdf::game::Settings::Get().usePrecision) {
				rdf::game::effects::InstallPrecisionIFrames();
			}
			break;
		case SKSE::MessagingInterface::kDataLoaded:
			rdf::game::input::Install();
			rdf::game::menu::Install();
			break;
		case SKSE::MessagingInterface::kPostLoadGame:
		case SKSE::MessagingInterface::kNewGame:
			OnNewSession();
			break;
		default:
			break;
		}
	}
}

SKSEPluginLoad(const SKSE::LoadInterface* a_skse)
{
	SetupLog();

	const auto* plugin = SKSE::PluginDeclaration::GetSingleton();
	logger::info("{} v{} is loading...", plugin->GetName(), plugin->GetVersion());

	SKSE::Init(a_skse, { .log = false, .trampoline = true, .trampolineSize = 512 });

	rdf::game::Settings::Load();
	rdf::game::graph::Init();
	rdf::game::gate::Init();
	rdf::game::update::Configure();

	if (!rdf::game::motion::Install()) {
		logger::error("root-motion hook not installed: dodges will not move the character");
	}
	if (rdf::game::Settings::Get().carryDecay > 0.0f) {
		rdf::game::motion::InstallSpeedFloor();
	}
	if (rdf::game::Settings::Get().iFrames) {
		if (!rdf::game::effects::InstallIFrames()) {
			logger::error("i-frame hook not installed: dodges will not grant invulnerability");
		}
		if (rdf::game::Settings::Get().explosionIFrames) {
			rdf::game::effects::InstallExplosionIFrames();
		}
		if (rdf::game::Settings::Get().projectileIFrames) {
			rdf::game::effects::InstallProjectileIFrames();
		}
	}
	rdf::game::update::Install();
	rdf::game::jump::Install();
	rdf::game::capture::Install();
	rdf::game::gate::Install();
	rdf::game::api::Install();

	SKSE::GetMessagingInterface()->RegisterListener(OnMessage);

	logger::info("{} has finished loading.", plugin->GetName());
	return true;
}
