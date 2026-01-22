#include "SF/Plugin.h"

#include "SF/Events/LockpickBlocker.h"
#include "SF/Combat/ShieldOfStaminaLite.h"
#include "SF/Combat/LightAttackStaminaCost.h"
#include "SF/Combat/DualWielding.h"
#include "SF/Movement/JumpStaminaCost.h"

#include <SKSE/SKSE.h>
#include <spdlog/sinks/basic_file_sink.h>

#include <filesystem>

namespace SF
{
	namespace
	{
		void InitLog()
		{
			auto path = SKSE::log::log_directory();
			if (!path) {
				return;
			}

			*path /= "Sunderandforged.log";

			auto sink = std::make_shared<spdlog::sinks::basic_file_sink_mt>(
				path->string(), true);

			auto logger = std::make_shared<spdlog::logger>(
				"global log", std::move(sink));

			spdlog::set_default_logger(std::move(logger));
			spdlog::set_level(spdlog::level::trace);
			spdlog::flush_on(spdlog::level::trace);
		}
	}

	void Plugin::Init(const SKSE::LoadInterface* skse)
	{
		// 🔴 ВАЖНО: логгер должен быть инициализирован ДО SKSE::Init
		InitLog();

		SKSE::Init(skse);

		SKSE::log::warn("Sunderandforged: Plugin Init OK");

		// Всё, что нужно делать после загрузки данных
		if (auto* msg = SKSE::GetMessagingInterface()) {
			msg->RegisterListener([](SKSE::MessagingInterface::Message* m) {
				if (m && m->type == SKSE::MessagingInterface::kDataLoaded) {
					SKSE::log::warn("Sunderandforged: DataLoaded");

					Events::LockpickBlocker::Install();
					Combat::ShieldOfStaminaLite::Install();
					Combat::LightAttackStaminaCost::Install();
					Combat::DualWielding::Install();
					Movement::JumpStaminaCost::Install();
				}
			});
		}
	}
}
