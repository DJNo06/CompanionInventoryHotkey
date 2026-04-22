#pragma once
#include <RE/A/Actor.h>

namespace Companion
{
	RE::Actor* FindActiveCompanionCandidate();
	void LogCandidate();
}