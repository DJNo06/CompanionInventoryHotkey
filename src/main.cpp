#include "Hotkey.h"
#include "Settings.h"

#include <F4SE/F4SE.h>
#include <F4SE/Interfaces.h>

#include <REX/REX.h>

namespace
{
	bool g_started = false;

	void OnF4SEMessage(F4SE::MessagingInterface::Message* a_msg)
	{
		if (!a_msg) {
			return;
		}

		switch (a_msg->type) {
		case F4SE::MessagingInterface::kGameDataReady:
		case F4SE::MessagingInterface::kPostLoadGame:
			if (!g_started) {
				g_started = true;

				Settings::Load();
				Settings::StartWatcher();
				Hotkey::Start();
			}
			break;

		default:
			break;
		}
	}
}

F4SE_PLUGIN_LOAD(const F4SE::LoadInterface* a_f4se)
{
	F4SE::Init(a_f4se);

	if (auto msg = F4SE::GetMessagingInterface(); msg) {
		msg->RegisterListener(OnF4SEMessage);
	}
	else {
		REX::WARN("Messaging interface not available");
	}

	return true;
}