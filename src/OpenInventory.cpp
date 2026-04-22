#include "OpenInventory.h"

#include <RE/C/Console.h>

#include <cstdio>
#include <cstdint>

namespace OpenInventory
{
	void OpenForCompanion(RE::Actor* companion)
	{
		if (!companion) {
			return;
		}

		const std::uint32_t id = companion->GetFormID();

		char cmd[64]{};
		std::snprintf(cmd, sizeof(cmd), "%08X.OpenActorContainer 1", id);

		RE::Console::ExecuteCommand(cmd);
	}
}