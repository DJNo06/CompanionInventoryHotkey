#include "Companion.h"

#include <cstring>

#include <RE/A/Actor.h>
#include <RE/B/BGSBaseAlias.h>
#include <RE/T/TESQuest.h>
#include <RE/T/TESDataHandler.h>
#include <RE/T/TESObjectREFR.h>

namespace
{
    static RE::TESQuest* GetFollowersQuest()
    {
        auto dh = RE::TESDataHandler::GetSingleton();
        return dh ? dh->LookupForm<RE::TESQuest>(0x000289E4, "Fallout4.esm") : nullptr;
    }

    static RE::Actor* ResolveAliasActor(RE::TESQuest* q, std::uint32_t aliasID)
    {
        RE::ObjectRefHandle h{};
        q->GetAliasedRef(&h, aliasID);

        auto ref = h.get().get();
        return ref ? ref->As<RE::Actor>() : nullptr;
    }

    static RE::Actor* GetByAliasName(RE::TESQuest* q, const char* name)
    {
        for (auto* a : q->aliases) {
            if (!a)
                continue;

            const char* n = a->aliasName.c_str();
            if (n && _stricmp(n, name) == 0) {
                return ResolveAliasActor(q, a->aliasID);
            }
        }
        return nullptr;
    }
}

namespace Companion
{
    RE::Actor* FindActiveCompanionCandidate()
    {
        auto q = GetFollowersQuest();
        if (!q) return nullptr;

        if (auto* a = GetByAliasName(q, "Companion"); a) return a;
        if (auto* a = GetByAliasName(q, "DogmeatCompanion"); a) return a;

        return nullptr;
    }

    void LogCandidate()
    {
        if (auto* a = FindActiveCompanionCandidate(); a) {
            REX::INFO("Active follower: 0x{:08X}", a->GetFormID());
        }
        else {
            REX::INFO("No active follower");
        }
    }
}