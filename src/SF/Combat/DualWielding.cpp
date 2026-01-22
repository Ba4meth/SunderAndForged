#include "SF/Combat/DualWielding.h"

#include <RE/Skyrim.h>
#include <SKSE/SKSE.h>

#include <atomic>
#include <cctype>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>

#include <windows.h>

namespace SF::Combat
{
	namespace
	{
		// ================= CONFIG =================
		std::atomic<int> g_keyBlock{ 47 };
		std::atomic<int> g_keyParry{ 48 };
		static constexpr const char* kConfigRelPath = "Data/SKSE/Plugins/SunderForge.json";

		// ================= AUTO-RELOAD =================
		std::atomic<bool> g_configLoaded{ false };
		std::uint64_t g_lastCheckTickMs = 0;
		std::filesystem::file_time_type g_lastWriteTime{};
		bool g_hasLastWriteTime = false;

		// ================= PARRY =================
		static constexpr float kParryStaminaCost = 20.0f;

		std::atomic<bool> g_pendingParryVisual{ false };
		std::uint64_t g_pendingParryVisualUntilMs = 0;

		// анти-дребезг/автоповтор
		std::atomic<std::uint64_t> g_lastParryPressMs{ 0 };

		// ================= HELPERS =================
		static RE::PlayerCharacter* Player()
		{
			return RE::PlayerCharacter::GetSingleton();
		}

		static bool IsInMenuMode()
		{
			if (auto ui = RE::UI::GetSingleton()) {
				return ui->GameIsPaused();
			}
			return false;
		}

		static bool GetGraphBool(RE::Actor* a, const char* name, bool& out)
		{
			if (!a) {
				return false;
			}
			return a->GetGraphVariableBool(name, out);
		}

		static bool IsPowerAttacking(RE::Actor* a)
		{
			if (!a) {
				return false;
			}
			bool v = false;
			if (GetGraphBool(a, "IsPowerAttacking", v) && v) return true;
			if (GetGraphBool(a, "bInPowerAttack", v) && v) return true;
			return false;
		}

		static void InterruptAttackSoft(RE::Actor* a)
		{
			if (!a) {
				return;
			}
			a->NotifyAnimationGraph("Interrupt");
			a->NotifyAnimationGraph("attackStop");
			a->NotifyAnimationGraph("AttackStop");
		}

		// ================= STAMINA =================
		static RE::ActorValueOwner* AVO(RE::Actor* a)
		{
			return a ? a->As<RE::ActorValueOwner>() : nullptr;
		}

		static float GetStamina(RE::Actor* a)
		{
			auto* avo = AVO(a);
			return avo ? avo->GetActorValue(RE::ActorValue::kStamina) : 0.0f;
		}

		static bool CanAfford(RE::Actor* a, float amount)
		{
			return GetStamina(a) + 1e-3f >= amount;
		}

		// ВАЖНО:
		// kDamage слой легко “перетирается/нормализуется” чужими системами (типа твоего LightAttackStaminaCost),
		// поэтому тут используем kPermanent (это реально уменьшает стамину).
		static void DrainStaminaPermanent(RE::Actor* actor, float amount)
		{
			if (!actor || amount <= 0.0f) {
				return;
			}
			auto* avo = AVO(actor);
			if (!avo) {
				return;
			}
			avo->RestoreActorValue(RE::ACTOR_VALUE_MODIFIER::kPermanent, RE::ActorValue::kStamina, -amount);
		}

		static void ScheduleDrainInTwoTicks(RE::ActorHandle h, float amount)
		{
			auto* task = SKSE::GetTaskInterface();
			if (!task) {
				return;
			}

			task->AddTask([h, amount]() {
				auto* task2 = SKSE::GetTaskInterface();
				if (!task2) {
					return;
				}

				task2->AddTask([h, amount]() {
					auto ptr = h.get();
					auto* actor = ptr ? ptr.get() : nullptr;
					if (!actor) {
						return;
					}
					DrainStaminaPermanent(actor, amount);
				});
			});
		}

		// ================= VISUAL =================
		static void ExecuteParryVisual(RE::Actor* a)
		{
			if (!a) {
				return;
			}
			a->NotifyAnimationGraph("bashStart");
			a->NotifyAnimationGraph("bashStop");
		}

		static void ProcessPendingParryVisual()
		{
			if (!g_pendingParryVisual.load(std::memory_order_acquire)) {
				return;
			}

			auto* pl = Player();
			if (!pl) {
				g_pendingParryVisual.store(false, std::memory_order_release);
				return;
			}

			const auto now = GetTickCount64();
			if (now < g_pendingParryVisualUntilMs) {
				return;
			}

			g_pendingParryVisual.store(false, std::memory_order_release);
			ExecuteParryVisual(pl);
		}

		// ================= CONFIG IO =================
		static std::filesystem::path GetRuntimeDir()
		{
			wchar_t path[MAX_PATH]{};
			const DWORD len = GetModuleFileNameW(nullptr, path, MAX_PATH);
			if (!len) {
				return std::filesystem::current_path();
			}
			return std::filesystem::path(path).parent_path();
		}

		static std::filesystem::path GetConfigPath()
		{
			return GetRuntimeDir() / kConfigRelPath;
		}

		static std::string ReadAllText(const std::filesystem::path& p)
		{
			std::ifstream ifs(p, std::ios::binary);
			if (!ifs.is_open()) {
				return {};
			}
			std::string s;
			ifs.seekg(0, std::ios::end);
			s.resize(static_cast<size_t>(ifs.tellg()));
			ifs.seekg(0, std::ios::beg);
			ifs.read(s.data(), static_cast<std::streamsize>(s.size()));
			return s;
		}

		static bool ExtractInt(const std::string& text, const char* key, int& out)
		{
			const std::string needle = std::string("\"") + key + "\"";
			auto pos = text.find(needle);
			if (pos == std::string::npos) {
				return false;
			}
			pos = text.find(':', pos);
			if (pos == std::string::npos) {
				return false;
			}
			pos++;
			while (pos < text.size() && std::isspace(static_cast<unsigned char>(text[pos]))) {
				pos++;
			}

			bool neg = false;
			if (pos < text.size() && text[pos] == '-') {
				neg = true;
				pos++;
			}

			long long val = 0;
			bool any = false;
			while (pos < text.size() && text[pos] >= '0' && text[pos] <= '9') {
				any = true;
				val = (val * 10) + (text[pos] - '0');
				pos++;
			}

			if (!any) {
				return false;
			}

			out = static_cast<int>(neg ? -val : val);
			return true;
		}

		static void LoadConfig(bool logAlways)
		{
			const auto p = GetConfigPath();
			const auto text = ReadAllText(p);

			if (text.empty()) {
				if (logAlways) {
					SKSE::log::info("DualWielding: config not found ({}), using defaults BlockKey={}, ParryKey={}",
						p.string(), g_keyBlock.load(), g_keyParry.load());
				}
				g_configLoaded.store(true, std::memory_order_release);
				return;
			}

			bool changed = false;
			int v = 0;

			if (ExtractInt(text, "BlockKey", v)) {
				if (g_keyBlock.load() != v) {
					g_keyBlock.store(v, std::memory_order_release);
					changed = true;
				}
			}
			if (ExtractInt(text, "BashKey", v)) {
				if (g_keyParry.load() != v) {
					g_keyParry.store(v, std::memory_order_release);
					changed = true;
				}
			}

			if (logAlways || changed) {
				SKSE::log::info("DualWielding: loaded config {} -> BlockKey={}, ParryKey={}",
					p.string(), g_keyBlock.load(), g_keyParry.load());
			}

			g_configLoaded.store(true, std::memory_order_release);
		}

		static void MaybeReloadConfig()
		{
			const std::uint64_t now = GetTickCount64();
			if (now - g_lastCheckTickMs < 1000) {
				return;
			}
			g_lastCheckTickMs = now;

			const auto p = GetConfigPath();

			std::error_code ec;
			const auto wt = std::filesystem::last_write_time(p, ec);
			if (ec) {
				if (!g_configLoaded.load(std::memory_order_acquire)) {
					LoadConfig(true);
				}
				return;
			}

			if (!g_hasLastWriteTime) {
				g_lastWriteTime = wt;
				g_hasLastWriteTime = true;
				if (!g_configLoaded.load(std::memory_order_acquire)) {
					LoadConfig(true);
				}
				return;
			}

			if (wt != g_lastWriteTime) {
				g_lastWriteTime = wt;
				LoadConfig(true);
			}
		}

		// ================= INPUT =================
		static void OnParryPressed()
		{
			auto* pl = Player();
			if (!pl || IsInMenuMode()) {
				return;
			}

			const auto now = GetTickCount64();
			const auto last = g_lastParryPressMs.load(std::memory_order_relaxed);
			if (now - last < 120) {
				return;
			}
			g_lastParryPressMs.store(now, std::memory_order_relaxed);

			if (!CanAfford(pl, kParryStaminaCost)) {
				return;
			}

			(void)IsPowerAttacking(pl);

			InterruptAttackSoft(pl);

			// списываем гарантированно (через 2 тика) и “жёстко”
			ScheduleDrainInTwoTicks(pl->GetHandle(), kParryStaminaCost);

			g_pendingParryVisual.store(true, std::memory_order_release);
			g_pendingParryVisualUntilMs = now + 40;
		}

		static void OnKeyDown(int key)
		{
			if (key == g_keyParry.load(std::memory_order_acquire)) {
				OnParryPressed();
			}
		}

		class InputSink final : public RE::BSTEventSink<RE::InputEvent*>
		{
		public:
			RE::BSEventNotifyControl ProcessEvent(
				RE::InputEvent* const* a_events,
				RE::BSTEventSource<RE::InputEvent*>*) override
			{
				if (!a_events) {
					return RE::BSEventNotifyControl::kContinue;
				}

				MaybeReloadConfig();

				if (!IsInMenuMode()) {
					ProcessPendingParryVisual();
				}

				if (IsInMenuMode()) {
					return RE::BSEventNotifyControl::kContinue;
				}

				for (auto e = *a_events; e; e = e->next) {
					if (e->eventType != RE::INPUT_EVENT_TYPE::kButton) {
						continue;
					}

					auto* b = e->AsButtonEvent();
					if (!b) {
						continue;
					}

					if (b->IsDown()) {
						OnKeyDown(static_cast<int>(b->GetIDCode()));
					}
				}

				return RE::BSEventNotifyControl::kContinue;
			}
		};

		InputSink g_sink;
		std::atomic<bool> g_installed{ false };

		static void InstallInput()
		{
			if (g_installed.exchange(true, std::memory_order_acq_rel)) {
				return;
			}

			LoadConfig(true);

			auto* mgr = RE::BSInputDeviceManager::GetSingleton();
			if (mgr) {
				mgr->AddEventSink(&g_sink);
				SKSE::log::info("DualWielding: input sink installed (parry drains stamina via kPermanent, 2 ticks delayed)");
			} else {
				SKSE::log::error("DualWielding: BSInputDeviceManager not available");
			}
		}
	}

	void DualWielding::Install()
	{
		InstallInput();
	}
}

