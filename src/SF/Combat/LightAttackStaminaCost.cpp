#include "SF/Combat/LightAttackStaminaCost.h"

#include <RE/Skyrim.h>
#include <SKSE/SKSE.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <mutex>
#include <string_view>
#include <unordered_map>

namespace SF::Combat
{
	namespace
	{
		// ---------------------------
		// Tweakables (hardcoded for now)
		// ---------------------------
		constexpr float kBaseUnarmed = 6.0f;      // Base cost for unarmed attacks
		constexpr float kBaseWeapon = 6.0f;       // Base cost for weapon attacks
		constexpr float kWeaponWeightMult = 1.0f; // Additional cost per weapon weight unit

		// Power attack multiplier
		constexpr float kPowerAttackMult = 2.0f;

		// How long the "damage scaled" window lasts after a low-stamina start.
		constexpr std::uint32_t kDamagePenaltyWindowMs = 200;

		// Pairing window: SoundPlay.WPNSwingUnarmed -> weaponSwing
		constexpr std::uint32_t kUnarmedPairWindowMs = 80;

		// How long we consider a "recent explicit hand tag" valid for resolving weaponSwing
		constexpr std::uint32_t kExplicitHandWindowMs = 250;

		// Session timeout (failsafe if graph never produces a spend tag)
		constexpr std::uint32_t kHandSessionTimeoutMs = 800;

		// How many ticks we re-assert stamina = 0 when engine overwrites it around attack start.
		constexpr int kForceZeroTicks = 0;

		// Debug (player-only)
		constexpr bool kDebugPlayerStart = true;
		constexpr bool kDebugPlayerSpend = true;
		constexpr bool kDebugPlayerSkips = true;

		// Log ALL player anim tags
		constexpr bool kDebugLogAllPlayerAnimTags = true;
		constexpr std::uint32_t kAllTagsDebounceMs = 5;

		inline std::uint32_t NowMs()
		{
			using namespace std::chrono;
			return static_cast<std::uint32_t>(
				duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count());
		}

		inline bool IsMeleeWeapon(const RE::TESObjectWEAP* weap)
		{
			if (!weap) {
				return false;
			}
			const auto type = weap->GetWeaponType();
			switch (type) {
			case RE::WEAPON_TYPE::kHandToHandMelee:
			case RE::WEAPON_TYPE::kOneHandSword:
			case RE::WEAPON_TYPE::kOneHandDagger:
			case RE::WEAPON_TYPE::kOneHandAxe:
			case RE::WEAPON_TYPE::kOneHandMace:
			case RE::WEAPON_TYPE::kTwoHandSword:
			case RE::WEAPON_TYPE::kTwoHandAxe:
				return true;
			default:
				return false;
			}
		}

		inline bool IsTwoHanded(const RE::TESObjectWEAP* weap)
		{
			if (!weap) {
				return false;
			}
			const auto type = weap->GetWeaponType();
			return type == RE::WEAPON_TYPE::kTwoHandSword || type == RE::WEAPON_TYPE::kTwoHandAxe;
		}

		inline bool IsUnarmed(const RE::TESObjectWEAP* weap)
		{
			return (!weap) || (weap->GetWeaponType() == RE::WEAPON_TYPE::kHandToHandMelee);
		}

		inline float GetWeaponWeight(const RE::TESObjectWEAP* weap)
		{
			return weap ? std::max(0.0f, weap->GetWeight()) : 0.0f;
		}

		inline RE::TESObjectWEAP* GetUnarmedWeapForm()
		{
			// Skyrim.esm "Unarmed" weapon
			return RE::TESForm::LookupByID<RE::TESObjectWEAP>(0x000001F4);
		}

		// Convert "Mod Power Attack Stamina" entry point into a multiplier.
		// IMPORTANT for this mod:
		// - We use it as a *global stamina cost multiplier* (applies to both light & power)
		// - Must NEVER zero-out cost (probe<=0 -> treat as 1.0)
		inline float GetStaminaCostMult(RE::Actor* actor, const RE::TESObjectWEAP* weapOrNull)
		{
			if (!actor) {
				return 1.0f;
			}

			// IMPORTANT: for unarmed we pass actual "Unarmed" WEAP form
			RE::TESObjectWEAP* weap = const_cast<RE::TESObjectWEAP*>(weapOrNull);
			if (!weap) {
				weap = GetUnarmedWeapForm();
			}

			float probe = 100.0f;
			RE::BGSEntryPoint::HandleEntryPoint(
				RE::BGSEntryPoint::ENTRY_POINT::kModPowerAttackStamina,
				actor,
				weap,
				&probe);

			if (probe <= 0.0f) {
				return 1.0f;
			}

			const float mult = probe / 100.0f;
			return std::clamp(mult, 0.05f, 10.0f);
		}

		// True power-attack detection (NPC-safe)
		inline bool IsPowerAttacking(RE::Actor* a_actor)
		{
			if (!a_actor) {
				return false;
			}

			auto currentProcess = a_actor->GetActorRuntimeData().currentProcess;
			if (!currentProcess) {
				return false;
			}
			auto highProcess = currentProcess->high;
			if (!highProcess) {
				return false;
			}
			auto attackData = highProcess->attackData;
			if (!attackData) {
				return false;
			}

			auto flags = attackData->data.flags;
			return flags.any(RE::AttackData::AttackFlag::kPowerAttack) &&
			       !flags.any(RE::AttackData::AttackFlag::kBashAttack);
		}

		inline float GetStamina(RE::Actor* actor)
		{
			auto* avo = actor ? actor->As<RE::ActorValueOwner>() : nullptr;
			if (!avo) {
				return 0.0f;
			}
			return std::max(0.0f, avo->GetActorValue(RE::ActorValue::kStamina));
		}

		// Adjust CURRENT stamina via DAMAGE layer only (does NOT touch base/max).
		// NOTE: For kDamage:
		//   negative -> decreases current stamina
		//   positive -> increases current stamina (undoes damage)
		inline void AdjustStaminaDamageLayer(RE::Actor* actor, float delta)
		{
			if (!actor || std::abs(delta) <= 1e-6f) {
				return;
			}
			auto* avo = actor->As<RE::ActorValueOwner>();
			if (!avo) {
				return;
			}
			avo->RestoreActorValue(RE::ACTOR_VALUE_MODIFIER::kDamage, RE::ActorValue::kStamina, delta);
		}

		inline void DrainStaminaDamageLayer(RE::Actor* actor, float amount)
		{
			if (!actor || amount <= 0.0f) {
				return;
			}
			AdjustStaminaDamageLayer(actor, -amount);
		}

		inline void DrainToZeroNow(RE::Actor* actor)
		{
			if (!actor) {
				return;
			}
			const float cur = GetStamina(actor);
			if (cur <= 0.0f) {
				return;
			}
			// drain a bit more to hard-clamp
			DrainStaminaDamageLayer(actor, cur + 1.0f);
		}

		inline void ForceZeroTicks(RE::ActorHandle h, int ticksLeft)
		{
			if (ticksLeft <= 0) {
				return;
			}

			auto* task = SKSE::GetTaskInterface();
			if (!task) {
				return;
			}

			task->AddTask([h, ticksLeft]() {
				auto ptr = h.get();
				auto* actor = ptr ? ptr.get() : nullptr;
				if (!actor) {
					return;
				}

				if (GetStamina(actor) > 0.0f) {
					DrainToZeroNow(actor);
				}

				ForceZeroTicks(h, ticksLeft - 1);
			});
		}

		// --------- TAGS ---------
		inline bool IsWeaponHandSwingTag(std::string_view t)
		{
			return (t == "weaponLeftSwing") || (t == "weaponRightSwing") ||
			       (t == "WeaponLeftSwing") || (t == "WeaponRightSwing");
		}

		inline bool IsWeaponSwingAmbiguous(std::string_view t)
		{
			return (t == "weaponSwing") || (t == "WeaponSwing");
		}

		inline bool IsUnarmedSwingSoundTag(std::string_view t)
		{
			return (t == "SoundPlay.WPNSwingUnarmed");
		}

		inline bool IsAttackStartTag(std::string_view t)
		{
			return (t == "attackStart") || (t == "attackStartLeft") || (t == "attackStartRight") ||
			       (t == "AttackStart") || (t == "AttackStartLeft") || (t == "AttackStartRight");
		}

		// Spend only on REAL swing tags to avoid x2/x3.
		inline bool IsSpendTag(std::string_view t)
		{
			return IsWeaponHandSwingTag(t) || IsWeaponSwingAmbiguous(t);
		}

		inline bool TagHasLeft(const std::string_view tagView)
		{
			return (tagView.find("Left") != std::string_view::npos) || (tagView.find("left") != std::string_view::npos);
		}

		inline bool TagHasRight(const std::string_view tagView)
		{
			return (tagView.find("Right") != std::string_view::npos) || (tagView.find("right") != std::string_view::npos);
		}

		// We implement scaling by temporarily adjusting AttackDamageMult.
		struct ActorState
		{
			// 0 = left, 1 = right
			std::array<std::uint32_t, 2> lastExplicitHandMs{ 0u, 0u };

			struct HandSession
			{
				bool active{ false };
				bool spent{ false };
				std::uint32_t startMs{ 0 };

				// IMPORTANT: snapshot stamina at attack start.
				// We'll enforce final stamina at spend time to cancel any vanilla drain (esp. power attacks).
				float startStamina{ 0.0f };

				// For debugging: what weapon we thought it was at start (optional)
				std::uint32_t startWeapFormID{ 0u };
				bool startWasTwoHanded{ false };
			};

			// sessions are indexed by "logical hand": for 2H we map both hands to the same index at runtime.
			std::array<HandSession, 2> session{};

			bool dmgScaleApplied{ false };
			std::uint32_t dmgScaleUntilMs{ 0 };
			float dmgScaleDelta{ 0.0f };

			// all-tags spam guard (player only)
			std::uint32_t lastAllTagLogMs{ 0 };
			RE::BSFixedString lastAllTag{};

			// weaponSwing decoding: remember recent unarmed swing sound
			std::uint32_t lastUnarmedSoundMs{ 0 };
			bool lastUnarmedHandIsLeft{ false };
			bool lastUnarmedHandValid{ false };
		};

		inline void ClearDamageScale(RE::Actor* a, ActorState& st)
		{
			if (!st.dmgScaleApplied) {
				return;
			}

			auto* avo = a ? a->As<RE::ActorValueOwner>() : nullptr;
			if (avo && std::abs(st.dmgScaleDelta) > 1e-6f) {
				avo->RestoreActorValue(RE::ACTOR_VALUE_MODIFIER::kTemporary,
					RE::ActorValue::kAttackDamageMult,
					-st.dmgScaleDelta);
			}

			st.dmgScaleApplied = false;
			st.dmgScaleUntilMs = 0;
			st.dmgScaleDelta = 0.0f;
		}

		inline void ClearDamageScaleIfExpired(RE::Actor* a, ActorState& st, std::uint32_t nowMs)
		{
			if (st.dmgScaleApplied && nowMs > st.dmgScaleUntilMs) {
				ClearDamageScale(a, st);
			}
		}

		inline void ApplyDamageScale(RE::Actor* a, ActorState& st, float scale01, std::uint32_t untilMs)
		{
			if (!a) {
				return;
			}

			auto* avo = a->As<RE::ActorValueOwner>();
			if (!avo) {
				return;
			}

			scale01 = std::clamp(scale01, 0.0f, 1.0f);

			if (st.dmgScaleApplied) {
				ClearDamageScale(a, st);
			}

			const float cur = avo->GetActorValue(RE::ActorValue::kAttackDamageMult);
			const float target = cur * scale01;
			const float delta = target - cur;

			if (std::abs(delta) > 1e-6f) {
				avo->RestoreActorValue(RE::ACTOR_VALUE_MODIFIER::kTemporary,
					RE::ActorValue::kAttackDamageMult,
					delta);
				st.dmgScaleApplied = true;
				st.dmgScaleUntilMs = untilMs;
				st.dmgScaleDelta = delta;
			}
		}

		inline void LogAllPlayerTagsIfEnabled(RE::Actor* actor, const std::string_view tagView, ActorState& st, std::uint32_t nowMs)
		{
			if (!kDebugLogAllPlayerAnimTags) {
				return;
			}
			if (!actor || !actor->IsPlayerRef()) {
				return;
			}

			if (st.lastAllTag == RE::BSFixedString(tagView.data()) && (nowMs - st.lastAllTagLogMs) <= kAllTagsDebounceMs) {
				return;
			}

			auto* objL = actor->GetEquippedObject(true);
			auto* objR = actor->GetEquippedObject(false);

			auto* weapL = objL ? objL->As<RE::TESObjectWEAP>() : nullptr;
			auto* weapR = objR ? objR->As<RE::TESObjectWEAP>() : nullptr;

			const auto idL = weapL ? weapL->GetFormID() : 0u;
			const auto idR = weapR ? weapR->GetFormID() : 0u;

			const char* nameL = weapL ? weapL->GetName() : "Unarmed/None";
			const char* nameR = weapR ? weapR->GetName() : "Unarmed/None";

			SKSE::log::info(
				"[AnimTag][Player] tag='{}'  L={:08X} '{}'  R={:08X} '{}'",
				tagView,
				idL,
				nameL ? nameL : "(null)",
				idR,
				nameR ? nameR : "(null)");

			st.lastAllTagLogMs = nowMs;
			st.lastAllTag = RE::BSFixedString(tagView.data());
		}

		inline void NoteExplicitHandIfAny(std::string_view tagView, ActorState& st, std::uint32_t nowMs)
		{
			if (TagHasLeft(tagView) || tagView == "weaponLeftSwing" || tagView == "WeaponLeftSwing") {
				st.lastExplicitHandMs[0] = nowMs;
			} else if (TagHasRight(tagView) || tagView == "weaponRightSwing" || tagView == "WeaponRightSwing") {
				st.lastExplicitHandMs[1] = nowMs;
			}
		}

		// For 2H weapons, both hands must share the same session index to prevent double spend.
		inline std::size_t MapHandToSessionIndex(const RE::TESObjectWEAP* weap, std::size_t resolvedHandIdx)
		{
			if (weap && IsTwoHanded(weap)) {
				return 1u;  // stable single slot for 2H
			}
			return resolvedHandIdx;
		}

		inline void BeginOrRefreshSession(ActorState& st, std::size_t sessionIdx, std::uint32_t nowMs, float startStamina, const RE::TESObjectWEAP* startWeap)
		{
			auto& s = st.session[sessionIdx];

			// If an active unspent session is still fresh, keep it (don't overwrite the snapshot).
			if (s.active && !s.spent && (nowMs - s.startMs) <= kHandSessionTimeoutMs) {
				return;
			}

			s.active = true;
			s.spent = false;
			s.startMs = nowMs;
			s.startStamina = std::max(0.0f, startStamina);
			s.startWeapFormID = startWeap ? startWeap->GetFormID() : 0u;
			s.startWasTwoHanded = startWeap ? IsTwoHanded(startWeap) : false;
		}

		inline bool CanSpendInSession(ActorState& st, std::size_t sessionIdx, std::uint32_t nowMs, float curStamina, const RE::TESObjectWEAP* curWeap)
		{
			auto& s = st.session[sessionIdx];

			if (!s.active) {
				// Some graphs may not emit attackStart; allow implicit session.
				s.active = true;
				s.spent = false;
				s.startMs = nowMs;
				s.startStamina = std::max(0.0f, curStamina);
				s.startWeapFormID = curWeap ? curWeap->GetFormID() : 0u;
				s.startWasTwoHanded = curWeap ? IsTwoHanded(curWeap) : false;
				return true;
			}

			if (nowMs - s.startMs > kHandSessionTimeoutMs) {
				// stale session -> restart snapshot
				s.active = true;
				s.spent = false;
				s.startMs = nowMs;
				s.startStamina = std::max(0.0f, curStamina);
				s.startWeapFormID = curWeap ? curWeap->GetFormID() : 0u;
				s.startWasTwoHanded = curWeap ? IsTwoHanded(curWeap) : false;
				return true;
			}

			return !s.spent;
		}

		inline void MarkSessionSpent(ActorState& st, std::size_t sessionIdx)
		{
			auto& s = st.session[sessionIdx];
			s.spent = true;
			s.active = false;
		}

		inline float GetSessionStartStamina(const ActorState& st, std::size_t sessionIdx)
		{
			return std::max(0.0f, st.session[sessionIdx].startStamina);
		}

		// Resolve hand & unarmed hint for this event.
		// Priority:
		// 1) explicit Left/Right in tag
		// 2) weaponLeftSwing/weaponRightSwing
		// 3) SoundPlay.WPNSwingUnarmed sets pairing state + guesses unarmed hand
		// 4) weaponSwing:
		//    4a) if paired with unarmed sound => unarmed with stored hand
		//    4b) else choose most recent explicit hand within window
		//    4c) else stable default RIGHT (prevents "left weapon makes right punch expensive")
		inline bool ResolveHandForTag(RE::Actor* actor, std::string_view tagView, ActorState& st, std::uint32_t nowMs, bool& outAmbiguous, bool& outTreatAsUnarmed)
		{
			outAmbiguous = false;
			outTreatAsUnarmed = false;

			// 1) explicit substrings
			if (TagHasLeft(tagView)) {
				return true;
			}
			if (TagHasRight(tagView)) {
				return false;
			}

			// 2) explicit swing tags
			if (tagView == "weaponLeftSwing" || tagView == "WeaponLeftSwing") {
				return true;
			}
			if (tagView == "weaponRightSwing" || tagView == "WeaponRightSwing") {
				return false;
			}

			// 3) unarmed sound tag sets pairing info
			if (IsUnarmedSwingSoundTag(tagView)) {
				outAmbiguous = true;
				outTreatAsUnarmed = true;

				st.lastUnarmedSoundMs = nowMs;

				if (!actor) {
					st.lastUnarmedHandValid = false;
					return false;
				}

				auto* objL = actor->GetEquippedObject(true);
				auto* objR = actor->GetEquippedObject(false);

				auto* weapL = objL ? objL->As<RE::TESObjectWEAP>() : nullptr;
				auto* weapR = objR ? objR->As<RE::TESObjectWEAP>() : nullptr;

				const bool leftHasWeapon = weapL && IsMeleeWeapon(weapL) && !IsUnarmed(weapL);
				const bool rightHasWeapon = weapR && IsMeleeWeapon(weapR) && !IsUnarmed(weapR);

				// If one side has weapon and other doesn't -> unarmed is empty side
				if (leftHasWeapon && !rightHasWeapon) {
					st.lastUnarmedHandIsLeft = false;  // right is unarmed
					st.lastUnarmedHandValid = true;
					return false;
				}
				if (rightHasWeapon && !leftHasWeapon) {
					st.lastUnarmedHandIsLeft = true;   // left is unarmed
					st.lastUnarmedHandValid = true;
					return true;
				}

				// fallback: use last explicit hand
				const auto dtL = nowMs - st.lastExplicitHandMs[0];
				const auto dtR = nowMs - st.lastExplicitHandMs[1];
				if (dtL <= kExplicitHandWindowMs || dtR <= kExplicitHandWindowMs) {
					const bool useLeft = (dtL <= dtR);
					st.lastUnarmedHandIsLeft = useLeft;
					st.lastUnarmedHandValid = true;
					return useLeft;
				}

				st.lastUnarmedHandIsLeft = false;  // default right
				st.lastUnarmedHandValid = true;
				return false;
			}

			// 4) weaponSwing ambiguous
			if (IsWeaponSwingAmbiguous(tagView)) {
				outAmbiguous = true;

				// 4a) paired unarmed
				if (st.lastUnarmedHandValid && (nowMs - st.lastUnarmedSoundMs) <= kUnarmedPairWindowMs) {
					outTreatAsUnarmed = true;
					return st.lastUnarmedHandIsLeft;
				}

				// 4b) most recent explicit hand
				const auto dtL = nowMs - st.lastExplicitHandMs[0];
				const auto dtR = nowMs - st.lastExplicitHandMs[1];
				if (dtL <= kExplicitHandWindowMs || dtR <= kExplicitHandWindowMs) {
					return (dtL <= dtR);
				}

				// 4c) stable default RIGHT
				return false;
			}

			// stable default RIGHT
			outAmbiguous = true;
			return false;
		}

		class AnimEventSink final : public RE::BSTEventSink<RE::BSAnimationGraphEvent>
		{
		public:
			static AnimEventSink* GetSingleton()
			{
				static AnimEventSink instance;
				return std::addressof(instance);
			}

			RE::BSEventNotifyControl ProcessEvent(
				const RE::BSAnimationGraphEvent* a_event,
				RE::BSTEventSource<RE::BSAnimationGraphEvent>*) override
			{
				if (!a_event || a_event->tag.empty()) {
					return RE::BSEventNotifyControl::kContinue;
				}

				const auto& tag = a_event->tag;
				const std::string_view tagView{ tag.c_str() ? tag.c_str() : "" };

				auto* holder = a_event->holder;
				auto* actor = holder ? const_cast<RE::Actor*>(holder->As<RE::Actor>()) : nullptr;
				if (!actor) {
					return RE::BSEventNotifyControl::kContinue;
				}

				const auto nowMs = NowMs();
				const auto id = actor->GetFormID();

				{
					std::scoped_lock _{ _lock };
					auto& st = _state[id];
					LogAllPlayerTagsIfEnabled(actor, tagView, st, nowMs);
					NoteExplicitHandIfAny(tagView, st, nowMs);
					ClearDamageScaleIfExpired(actor, st, nowMs);
				}

				// Pairing tag only
				if (IsUnarmedSwingSoundTag(tagView)) {
					std::scoped_lock _{ _lock };
					auto& st = _state[id];
					bool amb = false;
					bool un = false;
					(void)ResolveHandForTag(actor, tagView, st, nowMs, amb, un);
					if constexpr (kDebugPlayerStart) {
						if (actor->IsPlayerRef()) {
							SKSE::log::info("[LightAttackStaminaCost][UnarmedSound] tag={} hand={} (pairing only)",
								tagView, st.lastUnarmedHandValid ? (st.lastUnarmedHandIsLeft ? "L" : "R") : "?");
						}
					}
					return RE::BSEventNotifyControl::kContinue;
				}

				const bool isStart = IsAttackStartTag(tagView);
				const bool isSpend = IsSpendTag(tagView);

				if (!isStart && !isSpend) {
					return RE::BSEventNotifyControl::kContinue;
				}

				bool ambiguous = false;
				bool treatAsUnarmed = false;
				bool leftHand = false;
				std::size_t resolvedHandIdx = 1;

				// We'll also decide current weapon for session mapping.
				RE::TESObjectWEAP* curWeapForSession = nullptr;

				{
					std::scoped_lock _{ _lock };
					auto& st = _state[id];

					leftHand = ResolveHandForTag(actor, tagView, st, nowMs, ambiguous, treatAsUnarmed);
					resolvedHandIdx = leftHand ? 0u : 1u;
				}

				// Determine weapon for this hand (needed for 2H session mapping and snapshot).
				{
					auto* obj = actor->GetEquippedObject(leftHand);
					auto* weap = obj ? obj->As<RE::TESObjectWEAP>() : nullptr;

					// If decoded as unarmed -> force nullptr
					if (treatAsUnarmed) {
						weap = nullptr;
					}

					// If not melee, we keep nullptr for session mapping too (but we'll skip spend later).
					if (weap && !IsMeleeWeapon(weap)) {
						weap = nullptr;
					}

					curWeapForSession = weap;
				}

				// Map to logical session index (2H => single slot)
				const std::size_t sessionIdx = MapHandToSessionIndex(curWeapForSession, resolvedHandIdx);

				if (isStart) {
					const float snapStam = GetStamina(actor);

					{
						std::scoped_lock _{ _lock };
						auto& st = _state[id];
						BeginOrRefreshSession(st, sessionIdx, nowMs, snapStam, curWeapForSession);
					}

					if constexpr (kDebugPlayerStart) {
						if (actor->IsPlayerRef()) {
							const bool twoH = curWeapForSession ? IsTwoHanded(curWeapForSession) : false;
							SKSE::log::info("[LightAttackStaminaCost][Start] tag={} hand={} session={} twoH={} ambiguous={} unarmedHint={} snapStam={}",
								tagView,
								leftHand ? "L" : "R",
								(sessionIdx == 0 ? "L" : "R"),
								twoH,
								ambiguous,
								treatAsUnarmed,
								snapStam);
						}
					}
					return RE::BSEventNotifyControl::kContinue;
				}

				// Spend
				{
					std::scoped_lock _{ _lock };
					auto& st = _state[id];

					const float curStam = GetStamina(actor);
					if (!CanSpendInSession(st, sessionIdx, nowMs, curStam, curWeapForSession)) {
						if constexpr (kDebugPlayerSkips) {
							if (actor->IsPlayerRef()) {
								SKSE::log::info("[LightAttackStaminaCost][Skip] duplicate spend in session tag={} hand={} session={}",
									tagView, leftHand ? "L" : "R", (sessionIdx == 0 ? "L" : "R"));
							}
						}
						return RE::BSEventNotifyControl::kContinue;
					}

					// Each new spend defines its own scaling; clear previous immediately.
					ClearDamageScale(actor, st);
				}

				// Determine hand/weapon for actual cost calc
				auto* obj = actor->GetEquippedObject(leftHand);
				auto* weap = obj ? obj->As<RE::TESObjectWEAP>() : nullptr;

				if (treatAsUnarmed) {
					weap = nullptr;
				}

				if (weap && !IsMeleeWeapon(weap)) {
					if constexpr (kDebugPlayerSkips) {
						if (actor->IsPlayerRef()) {
							SKSE::log::info("[LightAttackStaminaCost][Skip] Not melee weapon. tag={} hand={}", tagView, leftHand ? "L" : "R");
						}
					}
					{
						std::scoped_lock _{ _lock };
						auto& st = _state[id];
						MarkSessionSpent(st, sessionIdx);
					}
					return RE::BSEventNotifyControl::kContinue;
				}

				const bool unarmed = IsUnarmed(weap);

				float baseCost = unarmed ? kBaseUnarmed : (kBaseWeapon + (GetWeaponWeight(weap) * kWeaponWeightMult));
				baseCost = std::max(0.0f, baseCost);

				if (baseCost <= 0.0f) {
					if constexpr (kDebugPlayerSkips) {
						if (actor->IsPlayerRef()) {
							SKSE::log::info("[LightAttackStaminaCost][Skip] baseCost<=0 tag={} hand={}", tagView, leftHand ? "L" : "R");
						}
					}
					{
						std::scoped_lock _{ _lock };
						auto& st = _state[id];
						MarkSessionSpent(st, sessionIdx);
					}
					return RE::BSEventNotifyControl::kContinue;
				}

				const bool isPower = IsPowerAttacking(actor);

				// multiplier applies to BOTH light and power
				const float entryMult = GetStaminaCostMult(actor, weap);

				float finalCost = baseCost * entryMult;
				if (isPower) {
					finalCost *= kPowerAttackMult;
				}
				finalCost = std::max(0.0f, finalCost);

				if (finalCost <= 0.0f) {
					if constexpr (kDebugPlayerSkips) {
						if (actor->IsPlayerRef()) {
							SKSE::log::info("[LightAttackStaminaCost][Skip] finalCost<=0 tag={} hand={} baseCost={} entryMult={} power={}",
								tagView, leftHand ? "L" : "R", baseCost, entryMult, isPower);
						}
					}
					{
						std::scoped_lock _{ _lock };
						auto& st = _state[id];
						MarkSessionSpent(st, sessionIdx);
					}
					return RE::BSEventNotifyControl::kContinue;
				}

				// IMPORTANT FIX:
				// We do NOT "drain additionally" from current stamina (which can include vanilla power-drain already).
				// Instead we enforce the final stamina based on snapshot at attack start:
				//   desired = startStamina - finalCost
				// This cancels any extra vanilla drain and prevents >x2 for 2H power attacks.
				const float staminaNow = GetStamina(actor);

				float startStam = 0.0f;
				{
					std::scoped_lock _{ _lock };
					startStam = GetSessionStartStamina(_state[id], sessionIdx);
				}

				const float paid = std::min(startStam, finalCost);
				const float ratio = (finalCost > 1e-6f) ? std::clamp(paid / finalCost, 0.0f, 1.0f) : 0.0f;

				const float desired = std::max(0.0f, startStam - finalCost);

				// Adjust current stamina to desired (may restore if vanilla already drained).
				AdjustStaminaDamageLayer(actor, desired - staminaNow);

				const float staminaAfter = GetStamina(actor);

				{
					std::scoped_lock _{ _lock };
					auto& st = _state[id];
					MarkSessionSpent(st, sessionIdx);
				}

				const bool insufficient = (startStam + 1e-4f < finalCost);
				if (insufficient) {
					ForceZeroTicks(actor->GetHandle(), kForceZeroTicks);
				}

				if constexpr (kDebugPlayerSpend) {
					if (actor->IsPlayerRef()) {
						const auto* name = weap ? weap->GetName() : "Unarmed";
						const bool twoH = weap ? IsTwoHanded(weap) : false;

						SKSE::log::info(
							"[LightAttackStaminaCost][Spend] tag={} power={} hand={} session={} twoH={} ambiguous={} treatAsUnarmed={} weap='{}' baseCost={} entryMult={} finalCost={} startStam={} curStamBefore={} desired={} paid={} ratio={} insuff={} stamAfter={}",
							tagView,
							isPower,
							leftHand ? "L" : "R",
							(sessionIdx == 0 ? "L" : "R"),
							twoH,
							ambiguous,
							treatAsUnarmed,
							name ? name : "(null)",
							baseCost,
							entryMult,
							finalCost,
							startStam,
							staminaNow,
							desired,
							paid,
							ratio,
							insufficient,
							staminaAfter);
					}
				}

				// Apply damage scaling if partial pay
				if (ratio + 1e-6f < 1.0f) {
					std::scoped_lock _{ _lock };
					auto& st = _state[id];
					ApplyDamageScale(actor, st, ratio, nowMs + kDamagePenaltyWindowMs);
				}

				return RE::BSEventNotifyControl::kContinue;
			}

		private:
			std::mutex _lock;
			std::unordered_map<std::uint32_t, ActorState> _state;
		};

		class ActorLoadedSink final : public RE::BSTEventSink<RE::TESObjectLoadedEvent>
		{
		public:
			static ActorLoadedSink* GetSingleton()
			{
				static ActorLoadedSink instance;
				return std::addressof(instance);
			}

			RE::BSEventNotifyControl ProcessEvent(
				const RE::TESObjectLoadedEvent* a_event,
				RE::BSTEventSource<RE::TESObjectLoadedEvent>*) override
			{
				if (!a_event || !a_event->loaded) {
					return RE::BSEventNotifyControl::kContinue;
				}

				auto* refr = RE::TESForm::LookupByID<RE::TESObjectREFR>(a_event->formID);
				auto* actor = refr ? refr->As<RE::Actor>() : nullptr;
				if (!actor) {
					return RE::BSEventNotifyControl::kContinue;
				}

				actor->AddAnimationGraphEventSink(AnimEventSink::GetSingleton());
				return RE::BSEventNotifyControl::kContinue;
			}
		};
	}

	void LightAttackStaminaCost::Install()
	{
		static std::once_flag once;
		std::call_once(once, []() {
			auto* sourceHolder = RE::ScriptEventSourceHolder::GetSingleton();
			if (!sourceHolder) {
				SKSE::log::warn("[LightAttackStaminaCost] ScriptEventSourceHolder is null");
				return;
			}
			sourceHolder->AddEventSink(ActorLoadedSink::GetSingleton());

			if (auto* pc = RE::PlayerCharacter::GetSingleton()) {
				pc->AddAnimationGraphEventSink(AnimEventSink::GetSingleton());
			}

			SKSE::log::info("[LightAttackStaminaCost] Installed (2H-safe sessions; power drain neutralized via startStamina snapshot)");
		});
	}
}
