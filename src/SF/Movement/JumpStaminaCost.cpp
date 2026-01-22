#include "JumpStaminaCost.h"

#include <RE/Skyrim.h>
#include <SKSE/SKSE.h>

#include <algorithm>
#include <cstdint>
#include <mutex>

namespace SF::Movement
{
	namespace
	{
		static constexpr float kJumpStaminaCost = 5.0f;

		// тег строго JumpUp
		static const RE::BSFixedString kTagJumpUp{ "JumpUp" };

		inline float GetStamina(RE::Actor* a_actor)
		{
			auto* avo = a_actor ? a_actor->As<RE::ActorValueOwner>() : nullptr;
			if (!avo) {
				return 0.0f;
			}
			return std::max(0.0f, avo->GetActorValue(RE::ActorValue::kStamina));
		}

		// Списываем "текущую" стамину через damage-modifier (то же, что обычный расход/урон пула)
		inline void SpendStamina(RE::Actor* a_actor, float a_amount)
		{
			if (!a_actor || a_amount <= 0.0f) {
				return;
			}

			auto* avo = a_actor->As<RE::ActorValueOwner>();
			if (!avo) {
				return;
			}

			// kDamage: положительное значение "лечит" (уменьшает damage),
			// отрицательное значение "ранит" (увеличивает damage) => уменьшает текущую стамину.
			avo->RestoreActorValue(
				RE::ACTOR_VALUE_MODIFIER::kDamage,
				RE::ActorValue::kStamina,
				-a_amount);
		}

		// Проверка "в воздухе": самый совместимый вариант для SE 1.5.97/NG
		inline bool IsInAir(RE::Actor* a_actor)
		{
			return a_actor ? a_actor->IsInMidair() : false;
		}

		struct PlayerJumpState
		{
			bool spentThisAir{ false };  // уже списали за текущий “полет”
		};

		class JumpAnimEventSink final : public RE::BSTEventSink<RE::BSAnimationGraphEvent>
		{
		public:
			static JumpAnimEventSink* GetSingleton()
			{
				static JumpAnimEventSink inst;
				return std::addressof(inst);
			}

			RE::BSEventNotifyControl ProcessEvent(
				const RE::BSAnimationGraphEvent* a_event,
				RE::BSTEventSource<RE::BSAnimationGraphEvent>*) override
			{
				if (!a_event) {
					return RE::BSEventNotifyControl::kContinue;
				}

				auto* holder = a_event->holder;
				auto* actor = holder ? const_cast<RE::Actor*>(holder->As<RE::Actor>()) : nullptr;
				if (!actor || !actor->IsPlayerRef()) {
					return RE::BSEventNotifyControl::kContinue;
				}

				// если игрок на земле — разрешаем следующее списание
				if (!IsInAir(actor)) {
					_state.spentThisAir = false;
				}

				// интересует строго JumpUp
				if (a_event->tag != kTagJumpUp) {
					return RE::BSEventNotifyControl::kContinue;
				}

				// уже списали за этот прыжок — игнор
				if (_state.spentThisAir) {
					return RE::BSEventNotifyControl::kContinue;
				}

				// ВАЖНО: списание AV делаем на главном потоке.
				_state.spentThisAir = true;

				auto* task = SKSE::GetTaskInterface();
				if (!task) {
					SKSE::log::warn("[JumpStaminaCost] TaskInterface is null");
					return RE::BSEventNotifyControl::kContinue;
				}

				task->AddTask([]() {
					auto* pc = RE::PlayerCharacter::GetSingleton();
					if (!pc) {
						return;
					}

					const float before = GetStamina(pc);
					SpendStamina(pc, kJumpStaminaCost);
					const float after = GetStamina(pc);

					SKSE::log::info("[JumpStaminaCost] JumpUp stamina {} -> {}", before, after);
				});

				return RE::BSEventNotifyControl::kContinue;
			}

		private:
			PlayerJumpState _state;
		};

		class PlayerLoadedSink final : public RE::BSTEventSink<RE::TESObjectLoadedEvent>
		{
		public:
			static PlayerLoadedSink* GetSingleton()
			{
				static PlayerLoadedSink inst;
				return std::addressof(inst);
			}

			RE::BSEventNotifyControl ProcessEvent(
				const RE::TESObjectLoadedEvent* a_event,
				RE::BSTEventSource<RE::TESObjectLoadedEvent>*) override
			{
				if (!a_event || !a_event->loaded) {
					return RE::BSEventNotifyControl::kContinue;
				}

				auto* pc = RE::PlayerCharacter::GetSingleton();
				if (!pc) {
					return RE::BSEventNotifyControl::kContinue;
				}
				if (a_event->formID != pc->GetFormID()) {
					return RE::BSEventNotifyControl::kContinue;
				}

				pc->AddAnimationGraphEventSink(JumpAnimEventSink::GetSingleton());
				SKSE::log::info("[JumpStaminaCost] Reattached anim sink on player load");
				return RE::BSEventNotifyControl::kContinue;
			}
		};

		inline void AttachToPlayerNow()
		{
			if (auto* pc = RE::PlayerCharacter::GetSingleton()) {
				pc->AddAnimationGraphEventSink(JumpAnimEventSink::GetSingleton());
			}
		}
	}

	void JumpStaminaCost::Install()
	{
		static std::once_flag once;
		std::call_once(once, []() {
			auto* sourceHolder = RE::ScriptEventSourceHolder::GetSingleton();
			if (!sourceHolder) {
				SKSE::log::warn("[JumpStaminaCost] ScriptEventSourceHolder is null");
				return;
			}

			sourceHolder->AddEventSink(PlayerLoadedSink::GetSingleton());
			AttachToPlayerNow();

			SKSE::log::info("[JumpStaminaCost] Installed (JumpUp only, main-thread AV spend)");
		});
	}
}
