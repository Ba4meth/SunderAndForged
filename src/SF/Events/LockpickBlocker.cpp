#include "SF/Events/LockpickBlocker.h"

#include <RE/Skyrim.h>
#include <RE/L/LockpickingMenu.h>
#include <RE/M/MenuOpenCloseEvent.h>
#include <RE/U/UI.h>
#include <RE/U/UIMessageQueue.h>

namespace SF::Events
{
	class LockpickBlockerSink final : public RE::BSTEventSink<RE::MenuOpenCloseEvent>
	{
	public:
		RE::BSEventNotifyControl ProcessEvent(
			const RE::MenuOpenCloseEvent* a_event,
			RE::BSTEventSource<RE::MenuOpenCloseEvent>*) override
		{
			if (!a_event) {
				return RE::BSEventNotifyControl::kContinue;
			}

			if (a_event->opening && a_event->menuName == RE::LockpickingMenu::MENU_NAME) {
				if (auto* queue = RE::UIMessageQueue::GetSingleton()) {
					queue->AddMessage(
						RE::LockpickingMenu::MENU_NAME.data(),
						RE::UI_MESSAGE_TYPE::kHide,
						nullptr);
				}
			}

			return RE::BSEventNotifyControl::kContinue;
		}
	};

	static LockpickBlockerSink g_sink;

	void LockpickBlocker::Install()
	{
		if (auto* ui = RE::UI::GetSingleton()) {
			ui->AddEventSink(&g_sink);
		}
	}
}
