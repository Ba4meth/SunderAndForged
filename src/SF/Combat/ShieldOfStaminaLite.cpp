#include "SF/Combat/ShieldOfStaminaLite.h"

#include <RE/Skyrim.h>
#include <SKSE/SKSE.h>

#include <cstdint>
#include <mutex>

namespace SF::Combat
{
	namespace
	{
		// Нанести "урон" текущему значению ActorValue (НЕ трогая базу/максимум)
		// В CommonLib это делается через RestoreActorValue(kDamage, ..., -val).
		inline void DamageAV(RE::Actor* a, RE::ActorValue av, float val)
		{
			if (!a || val <= 0.0f) {
				return;
			}

			// ActorValueOwner интерфейс есть через As<>
			if (auto* avo = a->As<RE::ActorValueOwner>()) {
				avo->RestoreActorValue(RE::ACTOR_VALUE_MODIFIER::kDamage, av, -val);
			}
		}

		inline bool IsBlockedHit(const RE::HitData& hitData)
		{
			// Надёжно для NG: через underlying()
			using HITFLAG = RE::HitData::Flag;
			const auto flags = static_cast<std::uint32_t>(hitData.flags.underlying());
			return (flags & static_cast<std::uint32_t>(HITFLAG::kBlocked)) != 0;
		}

		inline bool IsWeaponBlock(const RE::HitData& hitData)
		{
			using HITFLAG = RE::HitData::Flag;
			const auto flags = static_cast<std::uint32_t>(hitData.flags.underlying());
			return (flags & static_cast<std::uint32_t>(HITFLAG::kBlockWithWeapon)) != 0;
		}
	}

	class HitEventHook
	{
	public:
		static void InstallHook()
		{
			static std::once_flag once;
			std::call_once(once, []() {
				// Skyrim SE 1.5.97 (ShieldOfStamina базируется на этом ID)
				REL::Relocation<std::uintptr_t> hook{ REL::ID(37673) };

				// Чуть больше, чтобы хватило на будущие хуки
				SKSE::AllocTrampoline(1 << 8);
				auto& trampoline = SKSE::GetTrampoline();

				_ProcessHit = trampoline.write_call<5>(hook.address() + 0x3C0, ProcessHit);
			});
		}

	private:
		static void ProcessHit(RE::Actor* target, RE::HitData& hitData)
		{
			// Если не блок — вообще не вмешиваемся
			if (!IsBlockedHit(hitData) || !target) {
				_ProcessHit(target, hitData);
				return;
			}

			// Агрессор иногда может быть null (в оригинале тоже проверяют)
			auto aggressor = hitData.aggressor.get();
			if (!aggressor) {
				_ProcessHit(target, hitData);
				return;
			}

			// Базовый урон, который игра собирается нанести
			const float staminaDamageBase = hitData.totalDamage;
			if (staminaDamageBase <= 0.0f) {
				_ProcessHit(target, hitData);
				return;
			}

			// Мультипликаторы — пока “lite”: 1.0 (можно будет вынести в ini)
			// Если хочешь как в оригинале (PC/NPC + shield/weapon + guardBreak) — добавим.
			float staminaDamageMult = 1.0f;

			// Если хочешь разные мульты под щит/оружие — вот точка:
			// const bool weaponBlock = IsWeaponBlock(hitData);
			// staminaDamageMult = weaponBlock ? X : Y;

			const float staminaDamage = staminaDamageBase * staminaDamageMult;
			const float targetStamina = target->GetActorValue(RE::ActorValue::kStamina);

			if (targetStamina <= 0.0f) {
				// Стамины нет — пускай урон идёт в здоровье как обычно
				_ProcessHit(target, hitData);
				return;
			}

			if (targetStamina < staminaDamage) {
				// Стамины не хватает: часть урона поглощается стаминой, остаток — в здоровье.
				// Сколько "базового" урона можно оплатить текущей стаминой:
				const float blockedBaseDamage = targetStamina / staminaDamageMult;

				// Оставляем остаток урона в здоровье:
				hitData.totalDamage = staminaDamageBase - blockedBaseDamage;

				// И списываем всю стамину в ноль:
				DamageAV(target, RE::ActorValue::kStamina, targetStamina);
			} else {
				// Стамины хватает: здоровье НЕ трогаем вообще
				hitData.totalDamage = 0.0f;

				// Списываем нужную стамину
				DamageAV(target, RE::ActorValue::kStamina, staminaDamage);
			}

			_ProcessHit(target, hitData);
		}

		static inline REL::Relocation<decltype(ProcessHit)> _ProcessHit;
	};

	void ShieldOfStaminaLite::Install()
	{
		HitEventHook::InstallHook();
	}
}
